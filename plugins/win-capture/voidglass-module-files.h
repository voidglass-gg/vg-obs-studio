#pragma once

#include <obs-module.h>
#include <util/platform.h>
#include <util/windows/voidglass-dll-security.h>

static inline bool vg_module_open(const char *path, WORD machine, bool dll, vg_dll_file *file)
{
	wchar_t *data = NULL, *wide = NULL;
	wchar_t root[VG_DLL_PATH_CAPACITY];
	const char *module_data = obs_get_module_data_path(obs_current_module());
	bool ok = path && module_data && os_utf8_to_wcs_ptr(module_data, 0, &data) &&
		  os_utf8_to_wcs_ptr(path, 0, &wide) && vg_dll_directory(data, root) &&
		  vg_dll_open(wide, root, machine, dll, file);
	bfree(data);
	bfree(wide);
	return ok;
}

/* Only the explicit upstream deployment policy can select an external Hook.
 * VoidGlass never reaches this branch; provenance checks always use module files. */
static inline bool vg_hook_open(const char *path, WORD machine, vg_dll_file *file)
{
#ifdef VOIDGLASS_RUNTIME_HOOKS
	return vg_module_open(path, machine, true, file);
#else
	wchar_t *wide = NULL;
	wchar_t parent[VG_DLL_PATH_CAPACITY], root[VG_DLL_PATH_CAPACITY];
	bool ok = path && os_utf8_to_wcs_ptr(path, 0, &wide) && vg_dll_absolute(wide) &&
		  !wcscpy_s(parent, VG_DLL_PATH_CAPACITY, wide);
	wchar_t *slash = ok ? wcsrchr(parent, L'\\') : NULL;
	if (!slash && ok)
		slash = wcsrchr(parent, L'/');
	if (slash)
		*slash = 0;
	ok = slash && vg_dll_directory(parent, root) && vg_dll_open(wide, root, machine, true, file);
	bfree(wide);
	return ok;
#endif
}
