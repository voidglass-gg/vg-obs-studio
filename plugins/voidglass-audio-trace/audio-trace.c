#include "audio-trace.h"

#include <stdbool.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <audioclient.h>

enum {
	RING_SIZE = 64,
	MAX_FRAMES = 4096,
	TRACE_BEGIN_SECONDS = 130,
	TRACE_END_SECONDS = 172,
	TRACE_BYTE_LIMIT = 20 * 1024 * 1024,
};

struct trace_header {
	char magic[8];
	uint64_t stage, frequency, begin, end;
};

struct trace_packet {
	uint64_t sequence, qpc;
	struct vg_audio_chunk chunk;
};

struct trace_slot {
	struct trace_packet packet;
	float samples[MAX_FRAMES * 2];
};

struct vg_audio_trace {
	struct trace_header header;
	HANDLE file, stop, writer;
	uint64_t bytes, packets;
	volatile LONG producer, head, tail;
	volatile LONG64 attempted, lost;
	bool failed;
	struct trace_slot slots[RING_SIZE];
};

static uint64_t clock_ticks(void)
{
	LARGE_INTEGER clock;
	QueryPerformanceCounter(&clock);
	return (uint64_t)clock.QuadPart;
}

static LONG load_index(volatile LONG *value)
{
	return InterlockedCompareExchange(value, 0, 0);
}

static void write_bytes(struct vg_audio_trace *trace, const void *data, DWORD size)
{
	if (trace->failed)
		return;
	DWORD written = 0;
	if (trace->bytes + size > TRACE_BYTE_LIMIT || !WriteFile(trace->file, data, size, &written, NULL) ||
	    written != size) {
		trace->failed = true;
		return;
	}
	trace->bytes += written;
}

static void drain(struct vg_audio_trace *trace)
{
	LONG tail = load_index(&trace->tail);
	while (tail != load_index(&trace->head)) {
		const struct trace_slot *slot = &trace->slots[tail];
		write_bytes(trace, &slot->packet, sizeof(slot->packet));
		write_bytes(trace, slot->samples, slot->packet.chunk.frames * 2 * sizeof(float));
		trace->packets++;
		tail = (tail + 1) % RING_SIZE;
		InterlockedExchange(&trace->tail, tail);
	}
}

static DWORD WINAPI writer(void *param)
{
	struct vg_audio_trace *trace = param;
	write_bytes(trace, &trace->header, sizeof(trace->header));
	for (;;) {
		drain(trace);
		const uint64_t now = clock_ticks();
		const DWORD wait_ms = now < trace->header.begin ? 100 : (now < trace->header.end ? 5 : INFINITE);
		if (WaitForSingleObject(trace->stop, wait_ms) != WAIT_TIMEOUT)
			break;
	}
	drain(trace);
	/* The zero-frame record seals attempted, written and lost counts. No valid seal on IO failure. */
	const struct trace_packet seal = {
		.sequence = (uint64_t)trace->attempted,
		.qpc = clock_ticks(),
		.chunk = {.timestamp = trace->packets, .position = (uint64_t)trace->lost},
	};
	write_bytes(trace, &seal, sizeof(seal));
	return 0;
}

static const wchar_t *known_label(enum vg_audio_stage stage, const char *label)
{
	if (!label)
		return NULL;
	if (stage == VG_WASAPI) {
		if (strcmp(label, "game-audio") == 0)
			return L"s1-game";
		if (strcmp(label, "independent-audio") == 0)
			return L"s1-independent";
		if (strcmp(label, "camera-audio") == 0)
			return L"s1-camera";
	}
	if (stage == VG_AAC_INPUT) {
		if (strcmp(label, "game-audio/game-track") == 0)
			return L"s3-game";
		if (strcmp(label, "independent-audio/independent-track") == 0)
			return L"s3-independent";
		if (strcmp(label, "camera-audio/camera-track") == 0)
			return L"s3-camera";
		if (strcmp(label, "mix/mix") == 0)
			return L"s3-mix";
	}
	return NULL;
}

static struct vg_audio_trace *open_trace(enum vg_audio_stage stage, const wchar_t *path, uint64_t delay,
					uint64_t duration)
{
	LARGE_INTEGER freq;
	if (!QueryPerformanceFrequency(&freq))
		return NULL;
	struct vg_audio_trace *trace = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*trace));
	if (!trace)
		return NULL;
	trace->header = (struct trace_header){"VGATR001", stage, (uint64_t)freq.QuadPart, 0, 0};
	trace->header.begin = clock_ticks() + delay * trace->header.frequency;
	trace->header.end = trace->header.begin + duration * trace->header.frequency;
	trace->file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (trace->file != INVALID_HANDLE_VALUE) {
		trace->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (trace->stop)
			trace->writer = CreateThread(NULL, 0, writer, trace, 0, NULL);
	}
	if (!trace->writer) {
		vg_audio_trace_close(trace);
		return NULL;
	}
	return trace;
}

struct vg_audio_trace *vg_audio_trace_open(enum vg_audio_stage stage, const char *label)
{
	const wchar_t *name = known_label(stage, label);
	if (!name)
		return NULL;
	wchar_t path[MAX_PATH];
	const DWORD n = GetEnvironmentVariableW(L"VOIDGLASS_STRESS_AUDIO_TRACE_DIR", path, MAX_PATH);
	if (n < 3 || n + 28 >= MAX_PATH || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/'))
		return NULL;
	if (swprintf_s(path + n, MAX_PATH - n, L"\\%ls.vgat", name) < 0)
		return NULL;
	return open_trace(stage, path, TRACE_BEGIN_SECONDS, TRACE_END_SECONDS - TRACE_BEGIN_SECONDS);
}

static void push_at(struct vg_audio_trace *trace, uint64_t qpc, struct vg_audio_chunk chunk, const void *first,
		    const void *second)
{
	if (qpc < trace->header.begin || qpc >= trace->header.end)
		return;
	if (InterlockedCompareExchange(&trace->producer, 1, 0)) {
		InterlockedIncrement64(&trace->attempted);
		InterlockedIncrement64(&trace->lost);
		return;
	}
	const uint64_t sequence = (uint64_t)InterlockedIncrement64(&trace->attempted);
	const LONG head = load_index(&trace->head);
	const LONG next = (head + 1) % RING_SIZE;
	const bool silent = trace->header.stage == VG_WASAPI && (chunk.flags & AUDCLNT_BUFFERFLAGS_SILENT);
	if (next == load_index(&trace->tail) || !chunk.frames || chunk.frames > MAX_FRAMES || chunk.rate != 48000 ||
	    (!silent && !first) || (trace->header.stage == VG_AAC_INPUT && !second)) {
		InterlockedIncrement64(&trace->lost);
	} else {
		struct trace_slot *slot = &trace->slots[head];
		const size_t plane = chunk.frames * sizeof(float);
		slot->packet = (struct trace_packet){sequence, qpc, chunk};
		if (silent)
			memset(slot->samples, 0, plane * 2);
		else if (trace->header.stage == VG_AAC_INPUT) {
			memcpy(slot->samples, first, plane);
			memcpy(slot->samples + chunk.frames, second, plane);
		} else {
			memcpy(slot->samples, first, plane * 2);
		}
		InterlockedExchange(&trace->head, next);
	}
	InterlockedExchange(&trace->producer, 0);
}

void vg_audio_trace_push(struct vg_audio_trace *trace, struct vg_audio_chunk chunk, const void *first,
			 const void *second)
{
	if (trace)
		push_at(trace, clock_ticks(), chunk, first, second);
}

void vg_audio_trace_close(struct vg_audio_trace *trace)
{
	if (!trace)
		return;
	if (trace->writer) {
		SetEvent(trace->stop);
		WaitForSingleObject(trace->writer, INFINITE);
		CloseHandle(trace->writer);
	}
	if (trace->stop)
		CloseHandle(trace->stop);
	if (trace->file != INVALID_HANDLE_VALUE)
		CloseHandle(trace->file);
	HeapFree(GetProcessHeap(), 0, trace);
}
