#include "voidglass-video-timing.h"
#ifdef _WIN32
#include <windows.h>
#include <inttypes.h>
#include <stdlib.h>

#define TRACE_CAPACITY 98304
struct timing_event {
	uint64_t begin, end, object, frame;
	int64_t a, b;
	DWORD tid;
	enum vg_video_stage kind;
};
static struct timing_event *events;
static wchar_t path[32768];
static volatile LONG count;
static const char *const names[] = {
	"arrive", "decode", "publish", "async_queue", "upload", "observe",
	"tick", "mix_lock", "graphics_lock", "render", "convert", "stage",
	"map", "flush", "copy", "work", "sleep", "cache", "consume",
	"encode", "packet", "output_start", "output_end", "video_end", "poll",
	"resources", "context_wait", "context_hold"
};

void vg_timing_init(void)
{
	DWORD n = GetEnvironmentVariableW(L"VOIDGLASS_VIDEO_TIMING", path, 32768);
	if (n && n < 32768) {
		count = 0;
		events = calloc(TRACE_CAPACITY, sizeof(*events));
	}
}

uint64_t vg_timing_now(void)
{
	return events ? os_gettime_ns() : 0;
}

uint64_t vg_timing_thread_cycles(void)
{
	ULONG64 cycles = 0;
	if (events)
		QueryThreadCycleTime(GetCurrentThread(), &cycles);
	return cycles;
}

void vg_timing_event(enum vg_video_stage kind, uint64_t begin, const void *object, uint64_t frame, int64_t a, int64_t b)
{
	uint64_t end = os_gettime_ns();
	LONG i = InterlockedIncrement(&count) - 1;
	if (i < TRACE_CAPACITY)
		events[i] = (struct timing_event){begin, end, (uintptr_t)object, frame, a, b, GetCurrentThreadId(), kind};
}

void vg_timing_finish(void)
{
	/* Producers have joined. File I/O and formatting never run in a capture callback. */
	if (!events)
		return;
	FILE *file = _wfopen(path, L"wb");
	if (file) {
		fprintf(file, "# attempted=%ld capacity=%d\nkind,tid,begin,end,object,frame,a,b\n", count, TRACE_CAPACITY);
		for (LONG i = 0; i < count && i < TRACE_CAPACITY; i++) {
			const struct timing_event *e = events + i;
			fprintf(file, "%s,%lu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRId64 ",%" PRId64 "\n",
				names[e->kind], e->tid, e->begin, e->end, e->object, e->frame, e->a, e->b);
		}
		fclose(file);
	}
	free(events);
	events = NULL;
}
#endif
