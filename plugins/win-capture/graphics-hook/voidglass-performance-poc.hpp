#pragma once
#ifdef VG_GAMEPUB_PERF
#include <voidglass-performance-poc.h>
// Fixture-only diagnostic export; lookup once during startup, never bind arbitrary runtime paths.
static inline vg_perf_shared *vg_perf_state()
{
	using getter = vg_perf_shared *(*)();
	static getter get = (getter)GetProcAddress(GetModuleHandleW(nullptr), "vg_perf_poc_v1_state");
	auto *state = get ? get() : nullptr;
	return state && state->magic == VG_PERF_MAGIC && state->version == VG_PERF_VERSION &&
			       state->size == sizeof(vg_perf_shared)
		       ? state
		       : nullptr;
}
static inline vg_perf_frame *vg_perf_current()
{
	auto *s = vg_perf_state();
	return s && s->phase == VG_PERF_RECORDING && s->current < VG_PERF_CAPACITY ? &s->frames[s->current] : nullptr;
}
struct vg_perf_capture_scope {
	vg_perf_frame *frame = vg_perf_current();
	uint64_t begin = frame ? vg_perf_qpc() : 0;
	~vg_perf_capture_scope()
	{
		if (frame) {
			frame->capture_ticks += vg_perf_qpc() - begin;
			++frame->capture_calls;
		}
	}
};
static inline void vg_perf_signal(uint64_t ticks, HRESULT hr)
{
	if (auto *f = vg_perf_current()) {
		f->signal_ticks += ticks;
		++f->signal_calls;
		if (FAILED(hr))
			++f->errors;
	}
}
static inline void vg_perf_fence_created()
{
	if (auto *s = vg_perf_state())
		++s->fences_created;
}
#else
struct vg_perf_capture_scope {
	~vg_perf_capture_scope() {}
};
static inline void vg_perf_signal(uint64_t, HRESULT) {}
static inline void vg_perf_fence_created() {}
#endif
