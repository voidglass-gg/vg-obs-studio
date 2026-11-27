#pragma once

#include <windows.h>
#include <inttypes.h>
#include <wchar.h>
#include "graphics-hook-info.h"

static inline HWND capture_window_from_wire(uint32_t window)
{
	return (HWND)LongToHandle((LONG)window);
}

static inline void capture_mapping_name(wchar_t name[64], uint64_t window, uint32_t map_id)
{
	swprintf(name, 64, SHMEM_TEXTURE "_%" PRIu64 "_%u", window, map_id);
}

/* The hook formats (uintptr_t)GA_ROOT in the producer's address space. */
static inline uint64_t capture_mapping_key(HWND window, bool producer64)
{
	if (!producer64)
		return (uint32_t)(uintptr_t)window;
#ifdef _WIN64
	return (uint64_t)(uintptr_t)window;
#else
	return (uint64_t)(int64_t)(int32_t)(uintptr_t)window;
#endif
}

typedef HANDLE (*capture_map_open_fn)(void *, const wchar_t *);

static inline HANDLE open_capture_mapping(HWND selected, uint32_t hook_window, uint32_t map_id, bool producer64,
					  void *context, capture_map_open_fn open_map,
					  HWND(WINAPI *ancestor)(HWND, UINT), DWORD *error)
{
	wchar_t name[64];
	capture_mapping_name(name, capture_mapping_key(selected, producer64), map_id);
	HANDLE map = open_map(context, name);
	if (map)
		return map;

	*error = GetLastError();
	HWND root = ancestor(capture_window_from_wire(hook_window), GA_ROOT);
	if (root) {
		capture_mapping_name(name, capture_mapping_key(root, producer64), map_id);
		map = open_map(context, name);
	}
	return map;
}
