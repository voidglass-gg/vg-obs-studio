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
enum { VG_HOOK_UNKNOWN = 0, VG_HOOK_RUNTIME = 1, VG_HOOK_EXTERNAL = 2 };
enum { VG_CAP_PENDING = 0, VG_CAP_SUPPORTED = 1, VG_CAP_UNSUPPORTED = 2, VG_CAP_ERROR = 3 };
#pragma pack(push, 8)
typedef struct vg_pub_payload {
	uint32_t magic, size, major, minor;
	uint32_t capability, api, transport, pid;
	uint64_t creation, instance, generation, hwnd;
	uint64_t submitted, completed, heartbeat_qpc, frequency;
	uint64_t initialized_qpc, first_submitted_qpc, first_completed_qpc, last_completed_qpc;
	uint32_t map_id, dxgi_installed, native_initialized, device_flags;
	int32_t error;
	uint32_t reserved;
} vg_pub_payload;
typedef struct __declspec(align(8)) vg_pub_shared {
	volatile LONG sequence;
	vg_pub_payload data;
} vg_pub_shared;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(vg_pub_payload) == 152 && sizeof(vg_pub_shared) == 160, "publication wire layout");
#else
_Static_assert(sizeof(vg_pub_payload) == 152 && sizeof(vg_pub_shared) == 160, "publication wire layout");
#endif
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
	swprintf_s(name, 128, L"Local\\VoidGlass_GamePublication_v1_%lu_%016llx", pid, (unsigned long long)creation);
}
/* Historical Hooks create no additional channel. Absence is a normal result. */
static __inline int vg_pub_open_reader(DWORD pid, uint64_t creation, HANDLE *mapping, const vg_pub_shared **view)
{
	if (*view)
		return VG_PUB_VALID;
	wchar_t name[128];
	vg_pub_name(name, pid, creation);
	*mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
	if (!*mapping)
		return GetLastError() == ERROR_FILE_NOT_FOUND ? VG_PUB_ABSENT : VG_PUB_UNAVAILABLE;
	*view = (const vg_pub_shared *)MapViewOfFile(*mapping, FILE_MAP_READ, 0, 0, sizeof(vg_pub_shared));
	if (*view)
		return VG_PUB_VALID;
	CloseHandle(*mapping);
	*mapping = NULL;
	return VG_PUB_INCOMPATIBLE;
}
static __inline int vg_pub_read(const vg_pub_shared *view, size_t bytes, vg_pub_payload *out)
{
	if (!view || bytes < sizeof(vg_pub_shared))
		return VG_PUB_INCOMPATIBLE;
	for (int i = 0; i < 3; ++i) {
		LONG before = view->sequence;
		if (before & 1)
			continue;
		MemoryBarrier();
		memcpy(out, &view->data, sizeof(*out));
		MemoryBarrier();
		LONG after = view->sequence;
		if (before != after || (after & 1))
			continue;
		if (out->magic != VG_PUB_MAGIC || out->major != VG_PUB_MAJOR || out->minor != VG_PUB_MINOR ||
		    out->size != sizeof(*out) || out->capability > VG_CAP_ERROR || out->completed > out->submitted)
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
