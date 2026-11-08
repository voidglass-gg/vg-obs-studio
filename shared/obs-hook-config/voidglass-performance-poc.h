#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define VG_PERF_MAGIC 0x31504750u
#define VG_PERF_VERSION 1u
#define VG_PERF_CAPACITY 65536u
#define VG_PERF_IDLE 0
#define VG_PERF_RECORDING 1
#define VG_PERF_DONE 2
struct vg_perf_frame {
	uint64_t begin, before_present, after_present, capture_ticks, signal_ticks;
	uint32_t capture_calls, signal_calls, errors, reserved;
};
struct vg_perf_shared {
	uint32_t magic, version, size, capacity;
	uint32_t pid, owner_tid, target_fps, width, height, device_flags;
	uint32_t vendor_id, device_id, adapter_low, adapter_high;
	wchar_t adapter_name[128];
	uint64_t creation, frequency, begin, end, count, fences_created;
	volatile LONG phase;
	uint32_t current, overflow, errors;
	vg_perf_frame frames[VG_PERF_CAPACITY];
};
static __inline uint64_t vg_perf_qpc()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return (uint64_t)t.QuadPart;
}
static __inline void vg_perf_name(wchar_t *name, DWORD pid, uint64_t creation)
{
	swprintf_s(name, 128, L"Local\\VoidGlass_GamePerf_POC_v1_%lu_%016llx", pid, (unsigned long long)creation);
}
