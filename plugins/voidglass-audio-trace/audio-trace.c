#include "audio-trace.h"

#include <stdbool.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <audioclient.h>

enum {
	RING_SIZE = 64,
	MAX_FRAMES = 4096,
	MAX_WINDOWS = 8,
	MAX_CAPTURE_SECONDS = 42,
	TRACE_BYTE_LIMIT = 20 * 1024 * 1024,
};

struct trace_window {
	uint64_t begin, end;
};

struct schedule_header {
	char magic[8];
	uint64_t frequency, count;
};

struct trace_header {
	char magic[8];
	uint64_t stage, frequency, count, armed_qpc;
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
	struct trace_window windows[MAX_WINDOWS];
	wchar_t schedule_path[MAX_PATH];
	HANDLE file, stop, writer;
	uint64_t bytes, packets;
	volatile LONG armed, producer, head, tail;
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

static bool read_exact(HANDLE file, void *data, DWORD size)
{
	DWORD read = 0;
	return ReadFile(file, data, size, &read, NULL) && read == size;
}

/* The harness publishes a complete schedule by rename at the scenario's QPC origin. */
static bool load_schedule(struct vg_audio_trace *trace)
{
	HANDLE file = CreateFileW(trace->schedule_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
				  FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	struct schedule_header header;
	LARGE_INTEGER size;
	bool valid = read_exact(file, &header, sizeof(header)) && memcmp(header.magic, "VGAWIN01", 8) == 0 &&
		     header.frequency == trace->header.frequency && header.count > 0 && header.count <= MAX_WINDOWS &&
		     GetFileSizeEx(file, &size) &&
		     size.QuadPart == (LONGLONG)(sizeof(header) + header.count * sizeof(struct trace_window)) &&
		     read_exact(file, trace->windows, (DWORD)(header.count * sizeof(struct trace_window)));
	CloseHandle(file);
	if (!valid)
		return false;

	const uint64_t budget = MAX_CAPTURE_SECONDS * header.frequency;
	uint64_t previous_end = 0, total = 0;
	for (uint64_t i = 0; i < header.count; ++i) {
		const struct trace_window window = trace->windows[i];
		if (window.begin == 0 || window.begin < previous_end || window.end <= window.begin ||
		    window.end - window.begin > budget - total)
			return false;
		total += window.end - window.begin;
		previous_end = window.end;
	}
	trace->header.count = header.count;
	trace->header.armed_qpc = clock_ticks();
	InterlockedExchange(&trace->armed, 1);
	return true;
}

static DWORD writer_wait(const struct vg_audio_trace *trace, uint64_t now)
{
	for (uint64_t i = 0; i < trace->header.count; ++i) {
		if (now < trace->windows[i].begin)
			return 100;
		if (now < trace->windows[i].end)
			return 5;
	}
	return INFINITE;
}

static DWORD WINAPI writer(void *param)
{
	struct vg_audio_trace *trace = param;
	while (!load_schedule(trace) && WaitForSingleObject(trace->stop, 25) == WAIT_TIMEOUT)
		;
	write_bytes(trace, &trace->header, sizeof(trace->header));
	write_bytes(trace, trace->windows, (DWORD)(trace->header.count * sizeof(*trace->windows)));
	for (;;) {
		drain(trace);
		if (WaitForSingleObject(trace->stop, writer_wait(trace, clock_ticks())) != WAIT_TIMEOUT)
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

static struct vg_audio_trace *open_trace(enum vg_audio_stage stage, const wchar_t *path, const wchar_t *schedule)
{
	LARGE_INTEGER freq;
	if (!QueryPerformanceFrequency(&freq))
		return NULL;
	struct vg_audio_trace *trace = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*trace));
	if (!trace)
		return NULL;
	trace->header = (struct trace_header){"VGATR002", stage, (uint64_t)freq.QuadPart, 0, 0};
	wcscpy_s(trace->schedule_path, MAX_PATH, schedule);
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
	wchar_t path[MAX_PATH], schedule[MAX_PATH];
	const DWORD n = GetEnvironmentVariableW(L"VOIDGLASS_STRESS_AUDIO_TRACE_DIR", path, MAX_PATH);
	if (n < 3 || n >= MAX_PATH - 28 || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/'))
		return NULL;
	if (swprintf_s(schedule, MAX_PATH, L"%ls\\schedule.vgw", path) < 0 ||
	    swprintf_s(path + n, MAX_PATH - n, L"\\%ls.vgat", name) < 0)
		return NULL;
	return open_trace(stage, path, schedule);
}

static bool in_window(struct vg_audio_trace *trace, uint64_t qpc)
{
	if (!load_index(&trace->armed))
		return false;
	for (uint64_t i = 0; i < trace->header.count; ++i) {
		if (qpc < trace->windows[i].begin)
			return false;
		if (qpc < trace->windows[i].end)
			return true;
	}
	return false;
}

static void push_at(struct vg_audio_trace *trace, uint64_t qpc, struct vg_audio_chunk chunk, const void *first,
		    const void *second)
{
	if (!in_window(trace, qpc))
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
