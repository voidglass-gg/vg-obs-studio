#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum vg_audio_stage { VG_WASAPI = 1, VG_AAC_INPUT = 3 };

struct vg_audio_trace;

/* Interleaved float stereo for S1. Separate float planes for S3. */
struct vg_audio_chunk {
	uint64_t timestamp;
	uint64_t position;
	uint32_t target_pid;
	uint32_t frames;
	uint32_t rate;
	uint32_t flags;
};

struct vg_audio_trace *vg_audio_trace_open(enum vg_audio_stage stage, const char *label);
void vg_audio_trace_push(struct vg_audio_trace *trace, struct vg_audio_chunk chunk, const void *first,
			 const void *second);
/* The owner must have quiesced the producer before closing. */
void vg_audio_trace_close(struct vg_audio_trace *trace);

#ifdef __cplusplus
}
#endif
