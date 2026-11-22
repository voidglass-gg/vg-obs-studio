#pragma once
#include "platform.h"

/* Windows diagnostic build. Timestamps are host monotonic nanoseconds.
 * Object addresses are opaque, process-local correlation IDs. */
enum vg_video_stage {
	VG_ARRIVE, VG_DECODE, VG_PUBLISH, VG_ASYNC_QUEUE, VG_UPLOAD, VG_OBSERVE,
	VG_TICK, VG_MIX_LOCK, VG_GRAPHICS_LOCK, VG_RENDER, VG_CONVERT, VG_STAGE,
	VG_MAP, VG_FLUSH, VG_COPY, VG_WORK, VG_SLEEP, VG_CACHE, VG_CONSUME,
	VG_ENCODE, VG_PACKET, VG_OUTPUT_START, VG_OUTPUT_END, VG_VIDEO_END, VG_POLL,
	VG_RESOURCES, VG_CONTEXT_WAIT, VG_CONTEXT_HOLD
};

#ifdef __cplusplus
extern "C" {
#endif
#ifdef _WIN32
EXPORT uint64_t vg_timing_now(void);
EXPORT void vg_timing_event(enum vg_video_stage kind, uint64_t begin, const void *object,
			    uint64_t frame, int64_t a, int64_t b);
uint64_t vg_timing_thread_cycles(void);
void vg_timing_init(void);
void vg_timing_finish(void);
#define VG_END(kind, begin, object, frame, a, b) do { if (begin) vg_timing_event(kind, begin, object, frame, a, b); } while (0)
#else
#define vg_timing_now() ((uint64_t)0)
#define vg_timing_thread_cycles() ((uint64_t)0)
#define vg_timing_init() ((void)0)
#define vg_timing_finish() ((void)0)
#define VG_END(kind, begin, object, frame, a, b) ((void)(begin))
#endif
#ifdef __cplusplus
}
#endif
