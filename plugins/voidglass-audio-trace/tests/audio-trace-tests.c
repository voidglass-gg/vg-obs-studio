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
	trace->header.count = 2;
	trace->windows[0] = (struct trace_window){10, 20};
	trace->windows[1] = (struct trace_window){40, 50};
	trace->file = INVALID_HANDLE_VALUE;
	float borrowed[] = {1, 2, 3, 4};
	const struct vg_audio_chunk packet = {4321, 208, 42, 2, 48000, AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY};
	push_at(trace, 10, packet, borrowed, NULL);
	CHECK(trace->attempted == 0);
	InterlockedExchange(&trace->armed, 1);
	push_at(trace, 9, packet, borrowed, NULL);
	push_at(trace, 20, packet, borrowed, NULL);
	push_at(trace, 39, packet, borrowed, NULL);
	push_at(trace, 50, packet, borrowed, NULL);
	CHECK(trace->attempted == 0);
	CHECK(writer_wait(trace, 9) == 100 && writer_wait(trace, 10) == 5);
	CHECK(writer_wait(trace, 20) == 100 && writer_wait(trace, 40) == 5);
	CHECK(writer_wait(trace, 50) == INFINITE);
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
	push_at(trace, 40, packet, borrowed, NULL);
	CHECK(trace->head == 0);
	CHECK(trace->slots[RING_SIZE - 1].packet.sequence == RING_SIZE + 1);
	struct vg_audio_chunk wrong = packet;
	wrong.rate = 44100;
	InterlockedExchange(&trace->tail, 2);
	push_at(trace, 49, wrong, borrowed, NULL);
	CHECK(trace->lost == 2 && trace->head == 0);
	vg_audio_trace_close(trace);
}

static void write_exact(HANDLE file, const void *buffer, DWORD bytes)
{
	DWORD written = 0;
	CHECK(WriteFile(file, buffer, bytes, &written, NULL) && written == bytes);
}

static void worker_test(void)
{
	wchar_t directory[MAX_PATH], path[MAX_PATH], schedule[MAX_PATH];
	const DWORD length = GetTempPathW(MAX_PATH, directory);
	CHECK(length && length < MAX_PATH);
	CHECK(GetTempFileNameW(directory, L"vga", 0, path));
	CHECK(GetTempFileNameW(directory, L"vgw", 0, schedule));
	CHECK(!open_trace(VG_AAC_INPUT, path, schedule));
	CHECK(DeleteFileW(path));
	struct vg_audio_trace *trace = open_trace(VG_AAC_INPUT, path, schedule);
	CHECK(trace);
	const uint64_t now = clock_ticks(), freq = trace->header.frequency;
	const struct schedule_header plan = {"VGAWIN01", freq, 2};
	const struct trace_window windows[] = {{now, now + 5 * freq}, {now + 10 * freq, now + 15 * freq}};
	HANDLE file = CreateFileW(schedule, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	write_exact(file, &plan, sizeof(plan));
	write_exact(file, windows, sizeof(windows));
	CHECK(CloseHandle(file));
	for (int i = 0; i < 100 && !load_index(&trace->armed); ++i)
		Sleep(5);
	CHECK(load_index(&trace->armed));
	const struct vg_audio_chunk chunk = {1024, 3000000000, 0, 2, 48000, 0};
	float left[] = {1, 2}, right[] = {3, 4};
	vg_audio_trace_push(trace, chunk, left, right);
	vg_audio_trace_push(trace, chunk, left, NULL);
	left[0] = right[0] = -1;
	vg_audio_trace_close(trace);
	file = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	struct trace_header header;
	struct trace_window observed[2];
	struct trace_packet packet, seal;
	float samples[4];
	CHECK(read_exact(file, &header, sizeof(header)));
	CHECK(read_exact(file, observed, sizeof(observed)));
	CHECK(read_exact(file, &packet, sizeof(packet)));
	CHECK(read_exact(file, samples, sizeof(samples)));
	CHECK(read_exact(file, &seal, sizeof(seal)));
	CHECK(memcmp(header.magic, "VGATR002", 8) == 0 && sizeof(header) == 40);
	CHECK(sizeof(packet) == 48 && header.stage == VG_AAC_INPUT);
	CHECK(header.frequency == freq && header.count == 2 && header.armed_qpc >= now);
	CHECK(memcmp(observed, windows, sizeof(windows)) == 0);
	CHECK(packet.qpc >= header.armed_qpc && packet.qpc < windows[0].end);
	CHECK(packet.sequence == 1 && packet.chunk.timestamp == 1024 && packet.chunk.position == 3000000000);
	CHECK(samples[0] == 1 && samples[1] == 2 && samples[2] == 3 && samples[3] == 4);
	CHECK(seal.chunk.frames == 0 && seal.sequence == 2 && seal.chunk.timestamp == 1 && seal.chunk.position == 1);
	DWORD extra = 1;
	CHECK(ReadFile(file, samples, 1, &extra, NULL) && !extra);
	CHECK(CloseHandle(file));
	CHECK(DeleteFileW(path));
	CHECK(DeleteFileW(schedule));
}

int main(void)
{
	queue_test();
	worker_test();
	return 0;
}
