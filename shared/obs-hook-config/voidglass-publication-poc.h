#pragma once
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define VG_PUB_MAGIC 0x31504756u
#define VG_PUB_MAJOR 1u
#define VG_PUB_MINOR 0u
enum { VG_PUB_ABSENT = 1, VG_PUB_INCOMPATIBLE = 2, VG_PUB_UNAVAILABLE = 3, VG_PUB_VALID = 4 };
enum { VG_CAP_PENDING = 0, VG_CAP_SUPPORTED = 1, VG_CAP_UNSUPPORTED = 2, VG_CAP_ERROR = 3 };
typedef struct vg_pub_payload {
	uint32_t magic, size, major, minor;
	uint32_t capability, api, transport, pid;
	uint64_t creation, instance, generation, hwnd;
	uint64_t submitted, completed, heartbeat_qpc, frequency;
	uint64_t initialized_qpc, first_submitted_qpc, first_completed_qpc, last_completed_qpc;
	uint64_t signal_ticks, signal_max_ticks;
	uint32_t map_id, dxgi_installed, native_initialized, device_flags;
	int32_t error;
	uint32_t reserved;
} vg_pub_payload;
typedef struct __declspec(align(8)) vg_pub_shared {
	volatile LONG64 sequence;
	vg_pub_payload data;
} vg_pub_shared;
static __inline uint64_t vg_pub_qpc(void)
{
	LARGE_INTEGER value;
	QueryPerformanceCounter(&value);
	return (uint64_t)value.QuadPart;
}
static __inline uint64_t vg_pub_creation(HANDLE process)
{
	FILETIME a, b, c, d;
	if (!GetProcessTimes(process, &a, &b, &c, &d))
		return 0;
	return ((uint64_t)a.dwHighDateTime << 32) | a.dwLowDateTime;
}
static __inline void vg_pub_name(wchar_t *name, DWORD pid, uint64_t creation)
{
	swprintf_s(name, 128, L"Local\\VoidGlass_GamePub_POC_v1_%lu_%016llx", pid, (unsigned long long)creation);
}
static __inline int vg_pub_read(const vg_pub_shared *view, size_t bytes, vg_pub_payload *out)
{
	if (!view || bytes < sizeof(vg_pub_shared))
		return VG_PUB_INCOMPATIBLE;
	for (int i = 0; i < 3; ++i) {
		LONG64 before = view->sequence;
		if (before & 1)
			continue;
		MemoryBarrier();
		memcpy(out, &view->data, sizeof(*out));
		MemoryBarrier();
		LONG64 after = view->sequence;
		if (before != after || (after & 1))
			continue;
		if (out->magic != VG_PUB_MAGIC || out->major != VG_PUB_MAJOR || out->minor != VG_PUB_MINOR ||
		    out->size != sizeof(*out) || out->completed > out->submitted)
			return VG_PUB_INCOMPATIBLE;
		return VG_PUB_VALID;
	}
	return VG_PUB_UNAVAILABLE;
}
#ifdef __cplusplus
extern "C" {
#endif
void vg_pub_start(void);
void vg_pub_tick(void);
void vg_pub_shutdown(void);
void vg_pub_dxgi_installed(void);
void vg_pub_capture_init(void *device, void *context, HWND hwnd, uint32_t map_id);
void vg_pub_capture_free(void);
void vg_pub_submitted(void);
#ifdef __cplusplus
}
#endif
