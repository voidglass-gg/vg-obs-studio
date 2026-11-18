/* Use the private queue directly to exercise overflow without timing races. */
#include "../audio-trace.c"
#include <stdio.h>

#define CHECK(condition)                                           \
	do {                                                        \
		if (!(condition)) {                                  \
			fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
			exit(1);                                      \
		}                                                   \
	} while (0)

static void queue_test(void)
{
	struct vg_audio_trace *trace = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*trace));
	CHECK(trace);
	trace->header.stage = VG_WASAPI;
	trace->header.begin = 10;
	trace->header.end = 20;
	trace->file = INVALID_HANDLE_VALUE;
	float borrowed[] = {1, 2, 3, 4};
	const struct vg_audio_chunk packet = {4321, 208, 42, 2, 48000, AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY};
	push_at(trace, 9, packet, borrowed, NULL);
	push_at(trace, 20, packet, borrowed, NULL);
	CHECK(trace->attempted == 0);
	push_at(trace, 10, packet, borrowed, NULL);
	borrowed[0] = -7;
	CHECK(trace->slots[0].samples[0] == 1);
	CHECK(trace->slots[0].samples[3] == 4);
	CHECK(trace->slots[0].packet.sequence == 1);
	CHECK(trace->slots[0].packet.qpc == 10);
	CHECK(trace->slots[0].packet.chunk.timestamp == 4321);
	CHECK(trace->slots[0].packet.chunk.position == 208);
	CHECK(trace->slots[0].packet.chunk.target_pid == 42);
	CHECK(trace->slots[0].packet.chunk.flags == AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY);
	struct vg_audio_chunk silent = packet;
	silent.flags = AUDCLNT_BUFFERFLAGS_SILENT | AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR;
	trace->slots[1].samples[0] = 42;
	push_at(trace, 11, silent, NULL, NULL);
	CHECK(trace->slots[1].packet.chunk.flags == silent.flags);
	CHECK(trace->slots[1].samples[0] == 0);
	for (int i = 2; i < RING_SIZE - 1; ++i)
		push_at(trace, 12, packet, borrowed, NULL);
	push_at(trace, 13, packet, borrowed, NULL);
	CHECK(trace->attempted == RING_SIZE);
	CHECK(trace->lost == 1);
	CHECK(trace->slots[0].samples[0] == 1);
	InterlockedExchange(&trace->tail, 1);
	push_at(trace, 14, packet, borrowed, NULL);
	CHECK(trace->head == 0);
	CHECK(trace->slots[RING_SIZE - 1].packet.sequence == RING_SIZE + 1);
	struct vg_audio_chunk wrong = packet;
	wrong.rate = 44100;
	InterlockedExchange(&trace->tail, 2);
	push_at(trace, 15, wrong, borrowed, NULL);
	CHECK(trace->lost == 2 && trace->head == 0);
	vg_audio_trace_close(trace);
}

static void read_exact(HANDLE file, void *buffer, DWORD bytes)
{
	DWORD read = 0;
	CHECK(ReadFile(file, buffer, bytes, &read, NULL) && read == bytes);
}

static void worker_test(void)
{
	wchar_t directory[MAX_PATH], path[MAX_PATH];
	const DWORD length = GetTempPathW(MAX_PATH, directory);
	CHECK(length && length < MAX_PATH);
	CHECK(GetTempFileNameW(directory, L"vga", 0, path));
	CHECK(!open_trace(VG_AAC_INPUT, path, 0, 10));
	CHECK(DeleteFileW(path));
	struct vg_audio_trace *trace = open_trace(VG_AAC_INPUT, path, 0, 10);
	CHECK(trace);
	const struct vg_audio_chunk chunk = {1024, 3000000000, 0, 2, 48000, 0};
	float left[] = {1, 2}, right[] = {3, 4};
	vg_audio_trace_push(trace, chunk, left, right);
	vg_audio_trace_push(trace, chunk, left, NULL);
	left[0] = right[0] = -1;
	vg_audio_trace_close(trace);
	HANDLE file = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	struct trace_header header;
	struct trace_packet packet, seal;
	float samples[4];
	read_exact(file, &header, sizeof(header));
	read_exact(file, &packet, sizeof(packet));
	read_exact(file, samples, sizeof(samples));
	read_exact(file, &seal, sizeof(seal));
	CHECK(memcmp(header.magic, "VGATR001", 8) == 0 && sizeof(header) == 40);
	CHECK(sizeof(packet) == 48 && header.stage == VG_AAC_INPUT);
	CHECK(header.frequency && header.end == header.begin + 10 * header.frequency);
	CHECK(packet.qpc >= header.begin && packet.qpc < header.end);
	CHECK(packet.sequence == 1 && packet.chunk.timestamp == 1024 && packet.chunk.position == 3000000000);
	CHECK(samples[0] == 1 && samples[1] == 2 && samples[2] == 3 && samples[3] == 4);
	CHECK(seal.chunk.frames == 0 && seal.sequence == 2 && seal.chunk.timestamp == 1 && seal.chunk.position == 1);
	DWORD extra = 1;
	CHECK(ReadFile(file, samples, 1, &extra, NULL) && !extra);
	CHECK(CloseHandle(file));
	CHECK(DeleteFileW(path));
}

int main(void)
{
	queue_test();
	worker_test();
	return 0;
}
