#pragma once

#include <voidglass-publication.h>
#include <util/windows/voidglass-dll-security.h>

struct vg_provenance_key {
	DWORD pid;
	uint64_t creation;
	uintptr_t process, window;
	uint32_t map_id;
	bool capturing, channel;
};

struct vg_provenance_cache {
	struct vg_provenance_key key;
	unsigned attempts;
	uint64_t next_attempt_ms;
	int result;
	bool reference_checked, reference_valid;
};

static inline bool vg_provenance_target_changed(const struct vg_provenance_cache *cache,
						const struct vg_provenance_key *key)
{
	return cache->key.pid != key->pid || cache->key.creation != key->creation || cache->key.process != key->process;
}

/* Caller releases the pinned reference on a target change/STOP. Capture changes
 * invalidate observations, but the still-pinned installed file needs no rehash. */
static inline bool vg_provenance_select(struct vg_provenance_cache *cache, const struct vg_provenance_key *key)
{
	const bool target_changed = vg_provenance_target_changed(cache, key);
	const bool changed = target_changed || cache->key.window != key->window || cache->key.map_id != key->map_id ||
			     cache->key.capturing != key->capturing || cache->key.channel != key->channel;
	if (target_changed) {
		cache->reference_checked = false;
		cache->reference_valid = false;
	}
	if (changed) {
		if (!cache->reference_valid)
			cache->reference_checked = false;
		cache->key = *key;
		cache->attempts = 0;
		cache->next_attempt_ms = 0;
		cache->result = VG_HOOK_UNKNOWN;
	}
	return changed;
}

static inline int vg_provenance_poll(struct vg_provenance_cache *cache, uint64_t now_ms, void *context,
				     bool (*prepare)(void *), int (*observe)(void *))
{
	if (!cache->key.pid || !cache->key.creation || cache->result != VG_HOOK_UNKNOWN || cache->attempts == 3 ||
	    (cache->attempts && now_ms < cache->next_attempt_ms))
		return cache->result;
	++cache->attempts;
	cache->next_attempt_ms = now_ms + (cache->attempts == 1 ? 250 : 1000);
	if (!cache->reference_checked) {
		cache->reference_checked = true;
		cache->reference_valid = prepare(context);
	}
	if (cache->reference_valid) {
		const int result = observe(context);
		if (result == VG_HOOK_RUNTIME || result == VG_HOOK_EXTERNAL)
			cache->result = result;
	}
	return cache->result;
}

/* The reference stays open without write/delete sharing. File identity and the
 * observed canonical path therefore suffice; no second SHA read is necessary. */
static inline int vg_provenance_file(const vg_dll_file *reference, const wchar_t *observed)
{
	if (!vg_dll_absolute(observed))
		return VG_HOOK_UNKNOWN;
	HANDLE file = CreateFileW(observed, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return VG_HOOK_UNKNOWN;
	wchar_t canonical[VG_DLL_PATH_CAPACITY];
	BY_HANDLE_FILE_INFORMATION expected, actual;
	int result = VG_HOOK_UNKNOWN;
	if (vg_dll_final_path(file, canonical) && GetFileInformationByHandle(reference->handle, &expected) &&
	    GetFileInformationByHandle(file, &actual)) {
		result = !_wcsicmp(canonical, reference->path) &&
					 expected.dwVolumeSerialNumber == actual.dwVolumeSerialNumber &&
					 expected.nFileIndexHigh == actual.nFileIndexHigh &&
					 expected.nFileIndexLow == actual.nFileIndexLow
				 ? VG_HOOK_RUNTIME
				 : VG_HOOK_EXTERNAL;
	}
	CloseHandle(file);
	return result;
}
