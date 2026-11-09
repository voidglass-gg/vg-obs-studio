#include "voidglass-injector.h"
#include <voidglass-publication.h>
#include <tlhelp32.h>
#include "voidglass-module-files.h"
#include "voidglass-provenance-cache.h"
#include <inttypes.h>
#include <obs-module.h>
#include <obs-hotkey.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/windows/window-helpers.h>
#include <windows.h>
#include <dxgi.h>
#include <util/sse-intrin.h>
#include <util/util_uint64.h>
#include <ipc-util/pipe.h>
#include <util/windows/obfuscate.h>
#include "inject-library.h"
#include "compat-helpers.h"
#include "graphics-hook-info.h"
#include "graphics-hook-ver.h"
#include "cursor-capture.h"
#include "app-helpers.h"
#include "audio-helpers.h"
#include "nt-stuff.h"

#define do_log(level, format, ...) \
	blog(level, "[game-capture: '%s'] " format, obs_source_get_name(gc->source), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

/* clang-format off */

#define SETTING_MODE                 "capture_mode"
#define SETTING_CAPTURE_WINDOW       "window"
#define SETTING_WINDOW_PRIORITY      "priority"
#define SETTING_COMPATIBILITY        "sli_compatibility"
#define SETTING_CURSOR               "capture_cursor"
#define SETTING_TRANSPARENCY         "allow_transparency"
#define SETTING_PREMULTIPLIED_ALPHA  "premultiplied_alpha"
#define SETTING_LIMIT_FRAMERATE      "limit_framerate"
#define SETTING_CAPTURE_OVERLAYS     "capture_overlays"
#define SETTING_ANTI_CHEAT_HOOK      "anti_cheat_hook"
#define SETTING_HOOK_RATE            "hook_rate"
#define SETTING_RGBA10A2_SPACE       "rgb10a2_space"
#define SETTINGS_COMPAT_INFO         "compat_info"

/* deprecated */
#define SETTING_ANY_FULLSCREEN   "capture_any_fullscreen"

#define SETTING_MODE_ANY         "any_fullscreen"
#define SETTING_MODE_WINDOW      "window"
#define SETTING_MODE_HOTKEY      "hotkey"

#define HOTKEY_START             "hotkey_start"
#define HOTKEY_STOP              "hotkey_stop"

#define TEXT_MODE                  obs_module_text("Mode")
#define TEXT_GAME_CAPTURE          obs_module_text("GameCapture")
#define TEXT_ANY_FULLSCREEN        obs_module_text("GameCapture.AnyFullscreen")
#define TEXT_SLI_COMPATIBILITY     obs_module_text("SLIFix")
#define TEXT_ALLOW_TRANSPARENCY    obs_module_text("AllowTransparency")
#define TEXT_PREMULTIPLIED_ALPHA   obs_module_text("PremultipliedAlpha")
#define TEXT_WINDOW                obs_module_text("WindowCapture.Window")
#define TEXT_MATCH_PRIORITY        obs_module_text("WindowCapture.Priority")
#define TEXT_MATCH_TITLE           obs_module_text("WindowCapture.Priority.Title")
#define TEXT_MATCH_CLASS           obs_module_text("WindowCapture.Priority.Class")
#define TEXT_MATCH_EXE             obs_module_text("WindowCapture.Priority.Exe")
#define TEXT_CAPTURE_CURSOR        obs_module_text("CaptureCursor")
#define TEXT_LIMIT_FRAMERATE       obs_module_text("GameCapture.LimitFramerate")
#define TEXT_CAPTURE_OVERLAYS      obs_module_text("GameCapture.CaptureOverlays")
#define TEXT_ANTI_CHEAT_HOOK       obs_module_text("GameCapture.AntiCheatHook")
#define TEXT_HOOK_RATE             obs_module_text("GameCapture.HookRate")
#define TEXT_HOOK_RATE_SLOW        obs_module_text("GameCapture.HookRate.Slow")
#define TEXT_HOOK_RATE_NORMAL      obs_module_text("GameCapture.HookRate.Normal")
#define TEXT_HOOK_RATE_FAST        obs_module_text("GameCapture.HookRate.Fast")
#define TEXT_HOOK_RATE_FASTEST     obs_module_text("GameCapture.HookRate.Fastest")
#define TEXT_RGBA10A2_SPACE        obs_module_text("GameCapture.Rgb10a2Space")
#define TEXT_RGBA10A2_SPACE_SRGB   obs_module_text("GameCapture.Rgb10a2Space.Srgb")
#define TEXT_RGBA10A2_SPACE_2100PQ obs_module_text("GameCapture.Rgb10a2Space.2100PQ")

#define TEXT_MODE_ANY            TEXT_ANY_FULLSCREEN
#define TEXT_MODE_WINDOW         obs_module_text("GameCapture.CaptureWindow")
#define TEXT_MODE_HOTKEY         obs_module_text("GameCapture.UseHotkey")

#define TEXT_HOTKEY_START        obs_module_text("GameCapture.HotkeyStart")
#define TEXT_HOTKEY_STOP         obs_module_text("GameCapture.HotkeyStop")

/* clang-format on */

#define DEFAULT_RETRY_INTERVAL 2.0f
#define ERROR_RETRY_INTERVAL 4.0f

enum capture_mode { CAPTURE_MODE_ANY, CAPTURE_MODE_WINDOW, CAPTURE_MODE_HOTKEY };

enum hook_rate { HOOK_RATE_SLOW, HOOK_RATE_NORMAL, HOOK_RATE_FAST, HOOK_RATE_FASTEST };

#define RGBA10A2_SPACE_SRGB "srgb"
#define RGBA10A2_SPACE_2100PQ "2100pq"

struct game_capture_config {
	char *title;
	char *class;
	char *executable;
	enum window_priority priority;
	enum capture_mode mode;
	bool cursor;
	bool force_shmem;
	bool allow_transparency;
	bool premultiplied_alpha;
	bool limit_framerate;
	bool capture_overlays;
	bool anticheat_hook;
	enum hook_rate hook_rate;
	bool is_10a2_2100pq;
	bool capture_audio;
};

typedef DPI_AWARENESS_CONTEXT(WINAPI *PFN_SetThreadDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
typedef DPI_AWARENESS_CONTEXT(WINAPI *PFN_GetThreadDpiAwarenessContext)(VOID);
typedef DPI_AWARENESS_CONTEXT(WINAPI *PFN_GetWindowDpiAwarenessContext)(HWND);

struct game_capture {
	obs_source_t *source;
	obs_source_t *audio_source;

	struct cursor_data cursor_data;
	uint64_t injector_lease;
	uint64_t injector_creation;
	uint32_t cx;
	uint32_t cy;
	uint32_t pitch;
	DWORD process_id;
	DWORD thread_id;
	HWND next_window;
	HWND window;
	float retry_time;
	float fps_reset_time;
	float retry_interval;
	struct dstr title;
	struct dstr class;
	struct dstr executable;
	enum window_priority priority;
	obs_hotkey_pair_id hotkey_pair;
	volatile long hotkey_window;
	volatile bool deactivate_hook;
	volatile bool activate_hook_now;
	bool wait_for_target_startup;
	bool showing;
	bool active;
	bool capturing;
	bool activate_hook;
	bool process_is_64bit;
	bool error_acquiring;
	bool dwm_capture;
	bool initial_config;
	bool convert_16bit;
	bool is_app;
	bool cursor_hidden;

	struct game_capture_config config;

	ipc_pipe_server_t pipe;
	gs_texture_t *texture;
	gs_texture_t *extra_texture;
	gs_texrender_t *extra_texrender;
	bool is_10a2_2100pq;
	bool linear_sample;
	struct hook_info *global_hook_info;
	HANDLE keepalive_mutex;
	HANDLE hook_init;
	HANDLE hook_restart;
	HANDLE hook_stop;
	HANDLE hook_ready;
	HANDLE hook_exit;
	HANDLE hook_data_map;
	HANDLE global_hook_info_map;
	HANDLE target_process;
	HANDLE publication_map;
	const vg_pub_shared *publication_view;
	uint64_t publication_instance, publication_heartbeat;
	struct vg_provenance_cache publication_provenance;
	vg_dll_file *publication_reference;
	char *publication_hook_path;
	char publication_hook_sha256[65];
	HANDLE texture_mutexes[2];
	wchar_t *app_sid;
	int retrying;
	float cursor_check_time;

	union {
		struct {
			struct shmem_data *shmem_data;
			uint8_t *texture_buffers[2];
		};

		struct shtex_data *shtex_data;
		void *data;
	};

	void (*copy_texture)(struct game_capture *);

	PFN_SetThreadDpiAwarenessContext set_thread_dpi_awareness_context;
	PFN_GetThreadDpiAwarenessContext get_thread_dpi_awareness_context;
	PFN_GetWindowDpiAwarenessContext get_window_dpi_awareness_context;
};

struct graphics_offsets offsets32 = {0};
struct graphics_offsets offsets64 = {0};

static inline bool use_anticheat(struct game_capture *gc)
{
	return gc->config.anticheat_hook && !gc->is_app;
}

static inline HANDLE open_mutex_plus_id(struct game_capture *gc, const wchar_t *name, DWORD id)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", name, id);
	return gc->is_app ? open_app_mutex(gc->app_sid, new_name) : open_mutex(new_name);
}

static inline HANDLE open_mutex_gc(struct game_capture *gc, const wchar_t *name)
{
	return open_mutex_plus_id(gc, name, gc->process_id);
}

static inline HANDLE open_event_plus_id(struct game_capture *gc, const wchar_t *name, DWORD id)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", name, id);
	return gc->is_app ? open_app_event(gc->app_sid, new_name) : open_event(new_name);
}

static inline HANDLE open_event_gc(struct game_capture *gc, const wchar_t *name)
{
	return open_event_plus_id(gc, name, gc->process_id);
}

static inline HANDLE open_map_plus_id(struct game_capture *gc, const wchar_t *name, DWORD id)
{
	wchar_t new_name[64];
	swprintf(new_name, 64, L"%s%lu", name, id);

	debug("map id: %S", new_name);

	return gc->is_app ? open_app_map(gc->app_sid, new_name) : OpenFileMappingW(GC_MAPPING_FLAGS, false, new_name);
}

static inline HANDLE open_hook_info(struct game_capture *gc)
{
	return open_map_plus_id(gc, SHMEM_HOOK_INFO, gc->process_id);
}

static inline enum gs_color_format convert_format(uint32_t format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return GS_RGBA;
	case DXGI_FORMAT_B8G8R8X8_UNORM:
		return GS_BGRX;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return GS_BGRA;
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return GS_R10G10B10A2;
	case DXGI_FORMAT_R16G16B16A16_UNORM:
		return GS_RGBA16;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return GS_RGBA16F;
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
		return GS_RGBA32F;
	}

	return GS_UNKNOWN;
}

static void close_handle(HANDLE *p_handle)
{
	HANDLE handle = *p_handle;
	if (handle) {
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
		*p_handle = NULL;
	}
}

static inline HMODULE kernel32(void)
{
	static HMODULE kernel32_handle = NULL;
	if (!kernel32_handle)
		kernel32_handle = GetModuleHandleW(L"kernel32");
	return kernel32_handle;
}

static inline HANDLE open_process(DWORD desired_access, bool inherit_handle, DWORD process_id)
{
	typedef HANDLE(WINAPI * PFN_OpenProcess)(DWORD, BOOL, DWORD);
	static PFN_OpenProcess open_process_proc = NULL;
	if (!open_process_proc)
		open_process_proc =
			(PFN_OpenProcess)ms_get_obfuscated_func(kernel32(), "NuagUykjcxr", 0x1B694B59451ULL);

	return open_process_proc(desired_access, inherit_handle, process_id);
}

static inline float hook_rate_to_float(enum hook_rate rate)
{
	switch (rate) {
	case HOOK_RATE_SLOW:
		return 2.0f;
	case HOOK_RATE_FAST:
		return 0.5f;
	case HOOK_RATE_FASTEST:
		return 0.1f;
	case HOOK_RATE_NORMAL:
		/* FALLTHROUGH */
	default:
		return 1.0f;
	}
}

static void game_capture_clear_publication_proof(struct game_capture *gc)
{
	gc->publication_instance = gc->publication_heartbeat = 0;
	bfree(gc->publication_hook_path);
	gc->publication_hook_path = NULL;
	gc->publication_hook_sha256[0] = 0;
}

static void game_capture_reset_publication(struct game_capture *gc)
{
	if (gc->publication_view) {
		UnmapViewOfFile(gc->publication_view);
		gc->publication_view = NULL;
	}
	close_handle(&gc->publication_map);
	game_capture_clear_publication_proof(gc);
	if (gc->publication_reference) {
		vg_dll_close(gc->publication_reference);
		bfree(gc->publication_reference);
		gc->publication_reference = NULL;
	}
	memset(&gc->publication_provenance, 0, sizeof(gc->publication_provenance));
}

static void release_injector(struct game_capture *gc)
{
	if (gc->injector_lease) {
		struct vg_injector_status status = vg_injector_get_status(gc->injector_lease);
		if (status.helper_pid)
			info("releasing helper request: target=%lu helper=%lu compatibility=%d",
			     (unsigned long)status.target_pid, (unsigned long)status.helper_pid, status.compatibility);
		vg_injector_release(gc->injector_lease);
		gc->injector_lease = gc->injector_creation = 0;
	}
}

static void stop_capture(struct game_capture *gc, bool terminal)
{
	if (terminal)
		release_injector(gc);
	game_capture_reset_publication(gc);
	ipc_pipe_server_free(&gc->pipe);

	if (gc->hook_stop) {
		SetEvent(gc->hook_stop);
	}
	if (gc->global_hook_info) {
		UnmapViewOfFile(gc->global_hook_info);
		gc->global_hook_info = NULL;
	}
	if (gc->data) {
		UnmapViewOfFile(gc->data);
		gc->data = NULL;
	}

	if (gc->app_sid) {
		LocalFree(gc->app_sid);
		gc->app_sid = NULL;
	}

	close_handle(&gc->hook_restart);
	close_handle(&gc->hook_stop);
	close_handle(&gc->hook_ready);
	close_handle(&gc->hook_exit);
	close_handle(&gc->hook_init);
	close_handle(&gc->hook_data_map);
	close_handle(&gc->keepalive_mutex);
	close_handle(&gc->global_hook_info_map);
	close_handle(&gc->target_process);
	close_handle(&gc->texture_mutexes[0]);
	close_handle(&gc->texture_mutexes[1]);

	obs_enter_graphics();
	gs_texrender_destroy(gc->extra_texrender);
	gc->extra_texrender = NULL;
	gs_texture_destroy(gc->extra_texture);
	gc->extra_texture = NULL;
	gs_texture_destroy(gc->texture);
	gc->texture = NULL;
	obs_leave_graphics();

	if (gc->active)
		info("capture stopped");

	// if it was previously capturing, send an unhooked signal
	if (gc->capturing) {
		gc->capturing = false;
		signal_handler_t *sh = obs_source_get_signal_handler(gc->source);
		calldata_t data = {0};
		calldata_set_ptr(&data, "source", gc->source);
		signal_handler_signal(sh, "unhooked", &data);
		calldata_free(&data);

		// Also update audio source to stop capturing
		if (gc->audio_source)
			reconfigure_audio_source(gc->audio_source, NULL);
	}

	gc->copy_texture = NULL;
	gc->wait_for_target_startup = false;
	gc->active = false;

	if (gc->retrying)
		gc->retrying--;
}

static inline void free_config(struct game_capture_config *config)
{
	bfree(config->title);
	bfree(config->class);
	bfree(config->executable);
	memset(config, 0, sizeof(*config));
}

static void game_capture_destroy(void *data)
{
	struct game_capture *gc = data;
	stop_capture(gc, true);

	if (gc->audio_source)
		destroy_audio_source(gc->source, &gc->audio_source);

	signal_handler_t *sh = obs_source_get_signal_handler(gc->source);
	signal_handler_disconnect(sh, "rename", rename_audio_source, &gc->audio_source);

	if (gc->hotkey_pair)
		obs_hotkey_pair_unregister(gc->hotkey_pair);

	obs_enter_graphics();
	cursor_data_free(&gc->cursor_data);
	obs_leave_graphics();

	dstr_free(&gc->title);
	dstr_free(&gc->class);
	dstr_free(&gc->executable);
	free_config(&gc->config);
	bfree(gc);
}

static inline bool using_older_non_mode_format(obs_data_t *settings)
{
	return obs_data_has_user_value(settings, SETTING_ANY_FULLSCREEN) &&
	       !obs_data_has_user_value(settings, SETTING_MODE);
}

static inline void get_config(struct game_capture_config *cfg, obs_data_t *settings, const char *window)
{
	const char *mode_str = NULL;

	ms_build_window_strings(window, &cfg->class, &cfg->title, &cfg->executable);

	if (using_older_non_mode_format(settings)) {
		bool any = obs_data_get_bool(settings, SETTING_ANY_FULLSCREEN);
		mode_str = any ? SETTING_MODE_ANY : SETTING_MODE_WINDOW;
	} else {
		mode_str = obs_data_get_string(settings, SETTING_MODE);
	}

	if (mode_str && strcmp(mode_str, SETTING_MODE_WINDOW) == 0)
		cfg->mode = CAPTURE_MODE_WINDOW;
	else if (mode_str && strcmp(mode_str, SETTING_MODE_HOTKEY) == 0)
		cfg->mode = CAPTURE_MODE_HOTKEY;
	else
		cfg->mode = CAPTURE_MODE_ANY;

	cfg->priority = (enum window_priority)obs_data_get_int(settings, SETTING_WINDOW_PRIORITY);
	cfg->force_shmem = obs_data_get_bool(settings, SETTING_COMPATIBILITY);
	cfg->cursor = obs_data_get_bool(settings, SETTING_CURSOR);
	cfg->allow_transparency = obs_data_get_bool(settings, SETTING_TRANSPARENCY);
	cfg->premultiplied_alpha = obs_data_get_bool(settings, SETTING_PREMULTIPLIED_ALPHA);
	cfg->limit_framerate = obs_data_get_bool(settings, SETTING_LIMIT_FRAMERATE);
	cfg->capture_overlays = obs_data_get_bool(settings, SETTING_CAPTURE_OVERLAYS);
	cfg->anticheat_hook = obs_data_get_bool(settings, SETTING_ANTI_CHEAT_HOOK);
	cfg->hook_rate = (enum hook_rate)obs_data_get_int(settings, SETTING_HOOK_RATE);
	cfg->is_10a2_2100pq = strcmp(obs_data_get_string(settings, SETTING_RGBA10A2_SPACE), "2100pq") == 0;
	cfg->capture_audio = obs_data_get_bool(settings, SETTING_CAPTURE_AUDIO);
}

static inline int s_cmp(const char *str1, const char *str2)
{
	if (!str1 || !str2)
		return -1;

	return strcmp(str1, str2);
}

static inline bool capture_needs_reset(struct game_capture_config *cfg1, struct game_capture_config *cfg2)
{
	if (cfg1->mode != cfg2->mode) {
		return true;

	} else if (cfg1->mode == CAPTURE_MODE_WINDOW &&
		   (s_cmp(cfg1->class, cfg2->class) != 0 || s_cmp(cfg1->title, cfg2->title) != 0 ||
		    s_cmp(cfg1->executable, cfg2->executable) != 0 || cfg1->priority != cfg2->priority)) {
		return true;

	} else if (cfg1->force_shmem != cfg2->force_shmem) {
		return true;

	} else if (cfg1->limit_framerate != cfg2->limit_framerate) {
		return true;

	} else if (cfg1->capture_overlays != cfg2->capture_overlays) {
		return true;
	}

	return false;
}

static bool hotkey_start(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct game_capture *gc = data;

	if (pressed && gc->config.mode == CAPTURE_MODE_HOTKEY) {
		info("Activate hotkey pressed");
		os_atomic_set_long(&gc->hotkey_window, (long)(uintptr_t)GetForegroundWindow());
		os_atomic_set_bool(&gc->deactivate_hook, true);
		os_atomic_set_bool(&gc->activate_hook_now, true);
	}

	return true;
}

static bool hotkey_stop(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct game_capture *gc = data;

	if (pressed && gc->config.mode == CAPTURE_MODE_HOTKEY) {
		info("Deactivate hotkey pressed");
		os_atomic_set_bool(&gc->deactivate_hook, true);
	}

	return true;
}

static void game_capture_get_hooked(void *data, calldata_t *cd)
{
	struct game_capture *gc = data;
	if (!gc)
		return;

	calldata_set_bool(cd, "hooked", gc->capturing);

	if (gc->capturing) {
		calldata_set_string(cd, "title", gc->title.array);
		calldata_set_string(cd, "class", gc->class.array);
		calldata_set_string(cd, "executable", gc->executable.array);
	} else {
		calldata_set_string(cd, "title", "");
		calldata_set_string(cd, "class", "");
		calldata_set_string(cd, "executable", "");
	}
}

/* Best-effort file provenance, NOT an attestation of a potentially hostile process.
 * Serialize this procedure with tick/destruction on the OBS graphics thread. */
static bool game_capture_prepare_hook_reference(void *data)
{
	struct game_capture *gc = data;
	char *path = obs_module_file("graphics-hook64.dll");
	if (gc->publication_reference) {
		vg_dll_close(gc->publication_reference);
		bfree(gc->publication_reference);
	}
	gc->publication_reference = bzalloc(sizeof(*gc->publication_reference));
	const bool valid = gc->publication_reference &&
			   vg_module_open(path, IMAGE_FILE_MACHINE_AMD64, true, gc->publication_reference);
	bfree(path);
	return valid;
}

static int game_capture_observe_hook(void *data)
{
	struct game_capture *gc = data;
	game_capture_clear_publication_proof(gc);
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, gc->process_id);
	int result = VG_HOOK_UNKNOWN, found = 0;
	if (snapshot == INVALID_HANDLE_VALUE)
		return result;
	MODULEENTRY32W entry = {0};
	entry.dwSize = sizeof(entry);
	if (Module32FirstW(snapshot, &entry))
		do {
			if (_wcsicmp(entry.szModule, L"graphics-hook64.dll"))
				continue;
			++found;
			result = vg_provenance_file(gc->publication_reference, entry.szExePath);
			bfree(gc->publication_hook_path);
			gc->publication_hook_path = NULL;
			os_wcs_to_utf8_ptr(entry.szExePath, 0, &gc->publication_hook_path);
		} while (Module32NextW(snapshot, &entry));
	if (GetLastError() != ERROR_NO_MORE_FILES || found != 1)
		result = VG_HOOK_UNKNOWN;
	CloseHandle(snapshot);
	if (result == VG_HOOK_RUNTIME)
		vg_dll_hex(gc->publication_reference->sha256, gc->publication_hook_sha256);
	return result;
}

static void game_capture_get_publication(void *data, calldata_t *cd)
{
	struct game_capture *gc = data;
	int status = VG_PUB_UNAVAILABLE, provenance = VG_HOOK_UNKNOWN;
	vg_pub_payload value = {0};
	uint64_t creation = 0;
	if (!gc)
		goto done;
	if (!gc->target_process || WaitForSingleObject(gc->target_process, 0) != WAIT_TIMEOUT ||
	    GetProcessId(gc->target_process) != gc->process_id || !(creation = vg_pub_creation(gc->target_process))) {
		game_capture_reset_publication(gc);
		goto done;
	}
	if (!gc->process_is_64bit) {
		game_capture_reset_publication(gc);
		status = VG_PUB_ABSENT;
		goto done;
	}
	struct vg_provenance_key key = {
		.pid = gc->process_id,
		.creation = creation,
		.process = (uintptr_t)gc->target_process,
		.window = (uintptr_t)gc->window,
		.map_id = gc->global_hook_info ? gc->global_hook_info->map_id : 0,
		.capturing = gc->capturing,
	};
	if (vg_provenance_target_changed(&gc->publication_provenance, &key))
		game_capture_reset_publication(gc);
	status = vg_pub_open_reader(gc->process_id, creation, &gc->publication_map, &gc->publication_view);
	key.channel = gc->publication_view != NULL;
	if (vg_provenance_select(&gc->publication_provenance, &key))
		game_capture_clear_publication_proof(gc);
	provenance = vg_provenance_poll(&gc->publication_provenance, GetTickCount64(), gc,
					game_capture_prepare_hook_reference, game_capture_observe_hook);
	if (status != VG_PUB_VALID)
		goto done;
	status = vg_pub_read(gc->publication_view, sizeof(vg_pub_shared), &value);
	if (status != VG_PUB_VALID)
		goto done;
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	if (provenance != VG_HOOK_RUNTIME || value.pid != gc->process_id || value.creation != creation ||
	    !value.instance || value.frequency != (uint64_t)frequency.QuadPart || !value.heartbeat_qpc ||
	    (value.native_initialized && (value.api != 11 || value.transport != 1 || !gc->capturing ||
					  !gc->global_hook_info || value.map_id != gc->global_hook_info->map_id))) {
		status = VG_PUB_UNAVAILABLE;
		goto done;
	}
	if (gc->publication_instance != value.instance) {
		gc->publication_instance = value.instance;
		gc->publication_heartbeat = value.heartbeat_qpc;
		status = VG_PUB_UNAVAILABLE;
		goto done;
	}
	const uint64_t now = vg_pub_qpc();
	if (value.heartbeat_qpc <= gc->publication_heartbeat || now < value.heartbeat_qpc ||
	    now - value.heartbeat_qpc > value.frequency * 2) {
		status = VG_PUB_UNAVAILABLE;
		goto done;
	}
done:
	calldata_set_int(cd, "status", status);
	calldata_set_int(cd, "hook_provenance", provenance);
	calldata_set_bool(cd, "connected", gc && gc->capturing);
	calldata_set_string(cd, "hook_path", gc && gc->publication_hook_path ? gc->publication_hook_path : "");
	calldata_set_string(cd, "hook_sha256", gc ? gc->publication_hook_sha256 : "");
	calldata_set_bool(cd, "publication_supported", status == VG_PUB_VALID && value.capability == VG_CAP_SUPPORTED);
	if (status != VG_PUB_VALID)
		calldata_set_int(cd, "capability", gc && !gc->process_is_64bit ? VG_CAP_UNSUPPORTED : VG_CAP_PENDING);
	if (status != VG_PUB_VALID)
		return;
#define VG_PUB_FIELD(name) calldata_set_int(cd, #name, (long long)value.name)
	VG_PUB_FIELD(capability);
	VG_PUB_FIELD(pid);
	VG_PUB_FIELD(creation);
	VG_PUB_FIELD(instance);
	VG_PUB_FIELD(generation);
	VG_PUB_FIELD(hwnd);
	VG_PUB_FIELD(map_id);
	VG_PUB_FIELD(dxgi_installed);
	VG_PUB_FIELD(native_initialized);
	VG_PUB_FIELD(submitted);
	VG_PUB_FIELD(completed);
	VG_PUB_FIELD(heartbeat_qpc);
	VG_PUB_FIELD(frequency);
	VG_PUB_FIELD(initialized_qpc);
	VG_PUB_FIELD(first_submitted_qpc);
	VG_PUB_FIELD(first_completed_qpc);
	VG_PUB_FIELD(last_completed_qpc);
	VG_PUB_FIELD(device_flags);
	VG_PUB_FIELD(error);
#undef VG_PUB_FIELD
}

static void game_capture_update(void *data, obs_data_t *settings)
{
	struct game_capture *gc = data;
	struct game_capture_config cfg;
	bool reset_capture = false;
	const char *window = obs_data_get_string(settings, SETTING_CAPTURE_WINDOW);

	get_config(&cfg, settings, window);
	reset_capture = capture_needs_reset(&cfg, &gc->config);

	gc->error_acquiring = false;

	if (cfg.mode == CAPTURE_MODE_HOTKEY && gc->config.mode != CAPTURE_MODE_HOTKEY) {
		gc->activate_hook = false;
	} else {
		gc->activate_hook = !!window && !!*window;
	}

	free_config(&gc->config);
	gc->config = cfg;
	gc->retry_interval = DEFAULT_RETRY_INTERVAL * hook_rate_to_float(gc->config.hook_rate);
	gc->wait_for_target_startup = false;
	gc->is_10a2_2100pq = gc->config.is_10a2_2100pq;

	dstr_free(&gc->title);
	dstr_free(&gc->class);
	dstr_free(&gc->executable);

	if (cfg.mode == CAPTURE_MODE_WINDOW) {
		dstr_copy(&gc->title, gc->config.title);
		dstr_copy(&gc->class, gc->config.class);
		dstr_copy(&gc->executable, gc->config.executable);
		gc->priority = gc->config.priority;
	}

	if (!gc->initial_config) {
		if (reset_capture) {
			stop_capture(gc, true);
		}
	} else {
		gc->initial_config = false;
	}

	/* Linked audio capture source stuff */
	setup_audio_source(gc->source, &gc->audio_source, cfg.mode == CAPTURE_MODE_WINDOW ? window : NULL,
			   cfg.capture_audio, cfg.priority);
}

extern void wait_for_hook_initialization(void);

static void *game_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct game_capture *gc = bzalloc(sizeof(*gc));

	wait_for_hook_initialization();

	gc->source = source;
	gc->initial_config = true;
	gc->retry_interval = DEFAULT_RETRY_INTERVAL * hook_rate_to_float(gc->config.hook_rate);
	gc->hotkey_pair = obs_hotkey_pair_register_source(gc->source, HOTKEY_START, TEXT_HOTKEY_START, HOTKEY_STOP,
							  TEXT_HOTKEY_STOP, hotkey_start, hotkey_stop, gc, gc);

	const HMODULE hModuleUser32 = GetModuleHandle(L"User32.dll");
	if (hModuleUser32) {
		PFN_SetThreadDpiAwarenessContext set_thread_dpi_awareness_context =
			(PFN_SetThreadDpiAwarenessContext)GetProcAddress(hModuleUser32, "SetThreadDpiAwarenessContext");
		PFN_GetThreadDpiAwarenessContext get_thread_dpi_awareness_context =
			(PFN_GetThreadDpiAwarenessContext)GetProcAddress(hModuleUser32, "GetThreadDpiAwarenessContext");
		PFN_GetWindowDpiAwarenessContext get_window_dpi_awareness_context =
			(PFN_GetWindowDpiAwarenessContext)GetProcAddress(hModuleUser32, "GetWindowDpiAwarenessContext");
		if (set_thread_dpi_awareness_context && get_thread_dpi_awareness_context &&
		    get_window_dpi_awareness_context) {
			gc->set_thread_dpi_awareness_context = set_thread_dpi_awareness_context;
			gc->get_thread_dpi_awareness_context = get_thread_dpi_awareness_context;
			gc->get_window_dpi_awareness_context = get_window_dpi_awareness_context;
		}
	}

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	signal_handler_add(sh, "void unhooked(ptr source)");
	signal_handler_add(sh, "void hooked(ptr source, string title, string class, string executable)");

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void get_capture_publication(out int status)", game_capture_get_publication, gc);
	proc_handler_add(ph,
			 "void get_hooked(out bool hooked, out string title, out string class, out string executable)",
			 game_capture_get_hooked, gc);

	signal_handler_connect(sh, "rename", rename_audio_source, &gc->audio_source);

	game_capture_update(gc, settings);
	return gc;
}

#define STOP_BEING_BAD                                                      \
	"  This is most likely due to security software. Please make sure " \
	"that the OBS installation folder is excluded/ignored in the "      \
	"settings of the security software you are using."


static inline bool is_64bit_windows(void)
{
#ifdef _WIN64
	return true;
#else
	BOOL x86 = false;
	bool success = !!IsWow64Process(GetCurrentProcess(), &x86);
	return success && !!x86;
#endif
}

static inline bool is_64bit_process(HANDLE process)
{
	BOOL x86 = true;
	if (is_64bit_windows()) {
		bool success = !!IsWow64Process(process, &x86);
		if (!success) {
			return false;
		}
	}

	return !x86;
}

static inline bool open_target_process(struct game_capture *gc)
{
	gc->target_process = open_process(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, false, gc->process_id);
	if (!gc->target_process) {
		warn("could not open process: %s", gc->config.executable);
		return false;
	}

	gc->process_is_64bit = is_64bit_process(gc->target_process);
	gc->is_app = is_app(gc->target_process);
	if (gc->is_app) {
		gc->app_sid = get_app_sid(gc->target_process);
	}
	return true;
}

static inline bool init_keepalive(struct game_capture *gc)
{
	wchar_t new_name[64];
	swprintf(new_name, 64, WINDOW_HOOK_KEEPALIVE L"%lu", gc->process_id);

	gc->keepalive_mutex = gc->is_app ? create_app_mutex(gc->app_sid, new_name)
					 : CreateMutexW(NULL, false, new_name);
	if (!gc->keepalive_mutex) {
		warn("Failed to create keepalive mutex: %lu", GetLastError());
		return false;
	}

	return true;
}

static inline bool init_texture_mutexes(struct game_capture *gc)
{
	gc->texture_mutexes[0] = open_mutex_gc(gc, MUTEX_TEXTURE1);
	gc->texture_mutexes[1] = open_mutex_gc(gc, MUTEX_TEXTURE2);

	if (!gc->texture_mutexes[0] || !gc->texture_mutexes[1]) {
		DWORD error = GetLastError();
		if (error == 2) {
			if (!gc->retrying) {
				gc->retrying = 2;
				info("hook not loaded yet, retrying..");
			}
		} else {
			warn("failed to open texture mutexes: %lu", GetLastError());
		}
		return false;
	}

	return true;
}

/* if there's already a hook in the process, then signal and start */
static inline bool attempt_existing_hook(struct game_capture *gc)
{
	gc->hook_restart = open_event_gc(gc, EVENT_CAPTURE_RESTART);
	if (gc->hook_restart) {
		debug("existing hook found, signaling process: %s", gc->config.executable);
		SetEvent(gc->hook_restart);
		return true;
	}

	return false;
}

static inline void reset_frame_interval(struct game_capture *gc)
{
	struct obs_video_info ovi;
	uint64_t interval = 0;

	if (obs_get_video_info(&ovi)) {
		interval = util_mul_div64(ovi.fps_den, 1000000000ULL, ovi.fps_num);

		/* Always limit capture framerate to some extent.  If a game
		 * running at 900 FPS is being captured without some sort of
		 * limited capture interval, it will dramatically reduce
		 * performance. */
		if (!gc->config.limit_framerate)
			interval /= 2;
	}

	gc->global_hook_info->frame_interval = interval;
}

static inline bool init_hook_info(struct game_capture *gc)
{
	gc->global_hook_info_map = open_hook_info(gc);
	if (!gc->global_hook_info_map) {
		warn("init_hook_info: get_hook_info failed: %lu", GetLastError());
		return false;
	}

	gc->global_hook_info =
		MapViewOfFile(gc->global_hook_info_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*gc->global_hook_info));
	if (!gc->global_hook_info) {
		warn("init_hook_info: failed to map data view: %lu", GetLastError());
		return false;
	}

	if (gc->config.force_shmem) {
		warn("init_hook_info: user is forcing shared memory "
		     "(multi-adapter compatibility mode)");
	}

	gc->global_hook_info->offsets = gc->process_is_64bit ? offsets64 : offsets32;
	gc->global_hook_info->capture_overlay = gc->config.capture_overlays;
	gc->global_hook_info->force_shmem = gc->config.force_shmem;
	gc->global_hook_info->UNUSED_use_scale = false;
	gc->global_hook_info->allow_srgb_alias = true;
	reset_frame_interval(gc);

	obs_enter_graphics();
	if (!gs_shared_texture_available()) {
		warn("init_hook_info: shared texture capture unavailable");
		gc->global_hook_info->force_shmem = true;
	}
	obs_leave_graphics();

	return true;
}

static void pipe_log(void *param, uint8_t *data, size_t size)
{
	struct game_capture *gc = param;
	if (data && size)
		info("%s", data);
}

static inline bool init_pipe(struct game_capture *gc)
{
	char name[64];
	snprintf(name, sizeof(name), "%s%lu", PIPE_NAME, gc->process_id);

	if (!ipc_pipe_server_start(&gc->pipe, name, pipe_log, gc)) {
		warn("init_pipe: failed to start pipe");
		return false;
	}

	return true;
}

static inline int inject_library(HANDLE process, const wchar_t *dll)
{
	return inject_library_obf(process, dll, "D|hkqkW`kl{k\\osofj", 0xa178ef3655e5ade7, "[uawaRzbhh{tIdkj~~",
				  0x561478dbd824387c, "[fr}pboIe`dlN}", 0x395bfbc9833590fd, "\\`zs}gmOzhhBq",
				  0x12897dd89168789a, "GbfkDaezbp~X", 0x76aff7238788f7db);
}

static inline bool hook_direct(struct game_capture *gc, const char *hook_path_rel)
{
	wchar_t hook_path_abs_w[MAX_PATH];
	wchar_t *hook_path_rel_w;
	wchar_t *path_ret;
	HANDLE process;
	int ret;

	os_utf8_to_wcs_ptr(hook_path_rel, 0, &hook_path_rel_w);
	if (!hook_path_rel_w) {
		warn("hook_direct: could not convert string");
		return false;
	}

	path_ret = _wfullpath(hook_path_abs_w, hook_path_rel_w, MAX_PATH);
	bfree(hook_path_rel_w);

	if (path_ret == NULL) {
		warn("hook_direct: could not make absolute path");
		return false;
	}

	process = open_process(PROCESS_ALL_ACCESS, false, gc->process_id);
	if (!process) {
		warn("hook_direct: could not open process: %s (%lu)", gc->config.executable, GetLastError());
		return false;
	}

	ret = inject_library(process, hook_path_abs_w);
	CloseHandle(process);

	if (ret != 0) {
		warn("hook_direct: inject failed: %d", ret);
		return false;
	}

	return true;
}

/* Already canonicalized and held open by inject_hook until CreateProcess returns. */
struct inject_launch_context {
	struct game_capture *gc;
	const wchar_t *helper, *hook;
	bool compatibility;
};

static struct vg_injector_child launch_inject_process(void *data)
{
	struct inject_launch_context *context = data;
	struct game_capture *gc = context->gc;
	const wchar_t *helper = context->helper, *hook = context->hook;
	wchar_t *command = malloc(2 * VG_DLL_PATH_CAPACITY * sizeof(wchar_t));
	wchar_t *directory = malloc(VG_DLL_PATH_CAPACITY * sizeof(wchar_t));
	const bool anti_cheat = context->compatibility;
	struct vg_injector_child child = {.error = ERROR_INVALID_PARAMETER};
	PROCESS_INFORMATION pi = {0};
	STARTUPINFOW si = {0};
	si.cb = sizeof(si);
	if (!command || !directory) {
		child.error = ERROR_NOT_ENOUGH_MEMORY;
		goto done;
	}
	if (wcscpy_s(directory, VG_DLL_PATH_CAPACITY, helper))
		goto done;
	wchar_t *slash = wcsrchr(directory, L'\\');
	if (!slash)
		goto done;
	*slash = 0;
	if (swprintf_s(command, 2 * VG_DLL_PATH_CAPACITY, L"\"%s\" \"%s\" %lu %lu", helper, hook,
		       (unsigned long)anti_cheat, anti_cheat ? gc->thread_id : gc->process_id) < 0)
		goto done;
	// Check the pinned target again; never adopt a helper for a stale target identity.
	FILETIME creation, exit, kernel, user;
	if (!GetProcessTimes(gc->target_process, &creation, &exit, &kernel, &user) ||
	    (((uint64_t)creation.dwHighDateTime << 32) | creation.dwLowDateTime) != gc->injector_creation ||
	    GetProcessId(gc->target_process) != gc->process_id)
		goto done;
	DWORD selected_pid = 0;
	DWORD selected_tid = GetWindowThreadProcessId(gc->next_window ? gc->next_window : gc->window, &selected_pid);
	if (selected_pid != gc->process_id || selected_tid != gc->thread_id)
		goto done;
	if (CreateProcessW(helper, command, NULL, NULL, false, CREATE_NO_WINDOW, NULL, directory, &si, &pi)) {
		CloseHandle(pi.hThread);
		child.handle = pi.hProcess;
		child.pid = pi.dwProcessId;
		child.error = 0;
		info("inject helper process started (pid %lu)", pi.dwProcessId);
	} else {
		child.error = GetLastError();
	}
done:
	if (!child.handle)
		warn("inject helper not started: %lu", (unsigned long)child.error);
	free(command);
	free(directory);
	return child;
}

static inline bool create_inject_process(struct game_capture *gc, const wchar_t *helper, const wchar_t *hook,
					 bool compatibility)
{
	struct inject_launch_context context = {gc, helper, hook, compatibility};
	return vg_injector_start(gc->injector_lease, compatibility, launch_inject_process, &context);
}

extern char *get_hook_path(bool b64);

static inline bool inject_hook(struct game_capture *gc)
{
	if (!vg_injector_can_start(gc->injector_lease))
		return false;
	const bool anti_cheat = use_anticheat(gc);
	bool success = false;
	char *inject_path = obs_module_file(gc->process_is_64bit ? "inject-helper64.exe" : "inject-helper32.exe");
	char *hook_path = get_hook_path(gc->process_is_64bit);
	const WORD machine = gc->process_is_64bit ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
	vg_dll_file *helper = bzalloc(sizeof(*helper)), *hook = bzalloc(sizeof(*hook));
	if (!helper || !hook || !vg_module_open(inject_path, machine, false, helper) ||
	    !vg_hook_open(hook_path, machine, hook)) {
		warn("Rejected inject helper/Hook: expected absolute installed files and matching architecture");
		goto cleanup;
	}
	char *helper_utf8 = NULL, *hook_utf8 = NULL;
	char helper_sha[65], hook_sha[65];
	os_wcs_to_utf8_ptr(helper->path, 0, &helper_utf8);
	os_wcs_to_utf8_ptr(hook->path, 0, &hook_utf8);
	vg_dll_hex(helper->sha256, helper_sha);
	vg_dll_hex(hook->sha256, hook_sha);
	info("selected helper '%s' sha256=%s; Hook '%s' sha256=%s", helper_utf8 ? helper_utf8 : "", helper_sha,
	     hook_utf8 ? hook_utf8 : "", hook_sha);
	bfree(helper_utf8);
	bfree(hook_utf8);
	bool matching_architecture;
#ifdef _WIN64
	matching_architecture = gc->process_is_64bit;
#else
	matching_architecture = !gc->process_is_64bit;
#endif
	if (matching_architecture && !anti_cheat) {
		char *canonical = NULL;
		os_wcs_to_utf8_ptr(hook->path, 0, &canonical);
		if (canonical && vg_injector_claim_direct(gc->injector_lease))
			success = hook_direct(gc, canonical);
		bfree(canonical);
	} else {
		info("using helper (%s hook)", anti_cheat ? "compatibility" : "direct");
		success = create_inject_process(gc, helper->path, hook->path, anti_cheat);
	}
cleanup:
	if (helper)
		vg_dll_close(helper);
	if (hook)
		vg_dll_close(hook);
	bfree(helper);
	bfree(hook);
	bfree(inject_path);
	bfree(hook_path);
	return success;
}

static const char *blacklisted_exes[] = {
	"explorer",
	"steam",
	"battle.net",
	"galaxyclient",
	"skype",
	"uplay",
	"origin",
	"devenv",
	"taskmgr",
	"chrome",
	"discord",
	"firefox",
	"systemsettings",
	"applicationframehost",
	"cmd",
	"shellexperiencehost",
	"winstore.app",
	"searchui",
	"lockapp",
	"windowsinternal.composableshell.experiences.textinput.inputapp",
	NULL,
};

static bool is_blacklisted_exe(const char *exe)
{
	char cur_exe[MAX_PATH];

	if (!exe)
		return false;

	for (const char **vals = blacklisted_exes; *vals; vals++) {
		strcpy(cur_exe, *vals);
		strcat(cur_exe, ".exe");

		if (strcmpi(cur_exe, exe) == 0)
			return true;
	}

	return false;
}

static bool target_suspended(struct game_capture *gc)
{
	return thread_is_suspended(gc->process_id, gc->thread_id);
}

static bool init_events(struct game_capture *gc);

static bool init_hook(struct game_capture *gc)
{
	struct dstr exe = {0};
	bool blacklisted_process = false;

	if (gc->config.mode == CAPTURE_MODE_ANY) {
		if (ms_get_window_exe(&exe, gc->next_window)) {
			info("attempting to hook fullscreen process: %s", exe.array);
		}
	} else {
		if (ms_get_window_exe(&exe, gc->next_window)) {
			info("attempting to hook process: %s", exe.array);
		}
	}

	blacklisted_process = is_blacklisted_exe(exe.array);
	if (blacklisted_process)
		info("cannot capture %s due to being blacklisted", exe.array);
	dstr_free(&exe);

	if (blacklisted_process) {
		return false;
	}
	if (target_suspended(gc)) {
		return false;
	}
	if (!open_target_process(gc)) {
		return false;
	}
	FILETIME creation, exit, kernel, user;
	uint64_t born = 0;
	if (GetProcessTimes(gc->target_process, &creation, &exit, &kernel, &user))
		born = ((uint64_t)creation.dwHighDateTime << 32) | creation.dwLowDateTime;
	if (gc->injector_lease) {
		struct vg_injector_status status = vg_injector_get_status(gc->injector_lease);
		if (status.target_pid != gc->process_id || status.creation != born)
			release_injector(gc);
	}
	if (born && !gc->injector_lease) {
		gc->injector_creation = born;
		gc->injector_lease = vg_injector_acquire(gc->process_id, born);
	}
	HANDLE existing = open_event_gc(gc, EVENT_CAPTURE_RESTART);
	if (existing)
		CloseHandle(existing);
	// No fresh injection without a verified identity. A historical Hook remains usable.
	if (!existing && (!born || !vg_injector_can_start(gc->injector_lease)))
		return false;

	if (!init_keepalive(gc)) {
		return false;
	}
	if (!init_pipe(gc)) {
		return false;
	}
	if (!attempt_existing_hook(gc)) {
		if (!inject_hook(gc)) {
			return false;
		}
	}
	if (!init_texture_mutexes(gc)) {
		return false;
	}
	if (!init_hook_info(gc)) {
		return false;
	}
	if (!init_events(gc)) {
		return false;
	}

	SetEvent(gc->hook_init);

	gc->window = gc->next_window;
	gc->next_window = NULL;
	gc->active = true;
	gc->retrying = 0;
	return true;
}

static void setup_window(struct game_capture *gc, HWND window)
{
	HANDLE hook_restart;
	HANDLE process;

	GetWindowThreadProcessId(window, &gc->process_id);
	if (gc->process_id) {
		process = open_process(PROCESS_QUERY_INFORMATION, false, gc->process_id);
		if (process) {
			gc->is_app = is_app(process);
			if (gc->is_app) {
				gc->app_sid = get_app_sid(process);
			}
			CloseHandle(process);
		}
	}

	/* do not wait if we're re-hooking a process */
	hook_restart = open_event_gc(gc, EVENT_CAPTURE_RESTART);
	if (hook_restart) {
		gc->wait_for_target_startup = false;
		CloseHandle(hook_restart);
	}

	/* otherwise if it's an unhooked process, always wait a bit for the
	 * target process to start up before starting the hook process;
	 * sometimes they have important modules to load first or other hooks
	 * (such as steam) need a little bit of time to load.  ultimately this
	 * helps prevent crashes */
	if (gc->wait_for_target_startup) {
		gc->retry_interval = 3.0f * hook_rate_to_float(gc->config.hook_rate);
		gc->wait_for_target_startup = false;
	} else {
		gc->next_window = window;
	}
}

static void get_fullscreen_window(struct game_capture *gc)
{
	HWND window = GetForegroundWindow();
	MONITORINFO mi = {0};
	HMONITOR monitor;
	DWORD styles;
	RECT rect;

	gc->next_window = NULL;

	if (!window) {
		return;
	}
	if (!GetWindowRect(window, &rect)) {
		return;
	}

	/* ignore regular maximized windows */
	styles = (DWORD)GetWindowLongPtr(window, GWL_STYLE);
	if ((styles & WS_MAXIMIZE) != 0 && (styles & WS_BORDER) != 0) {
		return;
	}

	monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
	if (!monitor) {
		return;
	}

	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfo(monitor, &mi)) {
		return;
	}

	if (rect.left == mi.rcMonitor.left && rect.right == mi.rcMonitor.right && rect.bottom == mi.rcMonitor.bottom &&
	    rect.top == mi.rcMonitor.top) {
		setup_window(gc, window);
	} else {
		gc->wait_for_target_startup = true;
	}
}

static void get_selected_window(struct game_capture *gc)
{
	HWND window;

	if (dstr_cmpi(&gc->class, "dwm") == 0) {
		wchar_t class_w[512];
		os_utf8_to_wcs(gc->class.array, 0, class_w, 512);
		window = FindWindowW(class_w, NULL);
	} else if (gc->priority == WINDOW_PRIORITY_EXE && !dstr_is_empty(&gc->executable)) {
		/* An executable-only selector has no class or title. The window
		 * search needs empty strings for these optional criteria. */
		window = ms_find_window(INCLUDE_MINIMIZED, gc->priority, gc->class.array ? gc->class.array : "",
					gc->title.array ? gc->title.array : "", gc->executable.array);
	} else {
		window = ms_find_window(INCLUDE_MINIMIZED, gc->priority, gc->class.array, gc->title.array,
					gc->executable.array);
	}

	if (window) {
		setup_window(gc, window);
	} else {
		gc->wait_for_target_startup = true;
	}
}

static void try_hook(struct game_capture *gc)
{
	if (gc->config.mode == CAPTURE_MODE_ANY) {
		get_fullscreen_window(gc);
	} else {
		get_selected_window(gc);
	}

	if (gc->next_window) {
		gc->thread_id = GetWindowThreadProcessId(gc->next_window, &gc->process_id);

		// Make sure we never try to hook ourselves (projector)
		if (gc->process_id == GetCurrentProcessId())
			return;

		if (!gc->thread_id && gc->process_id)
			return;
		if (!gc->process_id) {
			warn("error acquiring, failed to get window "
			     "thread/process ids: %lu",
			     GetLastError());
			gc->error_acquiring = true;
			return;
		}

		if (!init_hook(gc)) {
			stop_capture(gc, false);
		}
	} else {
		gc->active = false;
	}
}

static inline bool init_events(struct game_capture *gc)
{
	if (!gc->hook_restart) {
		gc->hook_restart = open_event_gc(gc, EVENT_CAPTURE_RESTART);
		if (!gc->hook_restart) {
			warn("init_events: failed to get hook_restart "
			     "event: %lu",
			     GetLastError());
			return false;
		}
	}

	if (!gc->hook_stop) {
		gc->hook_stop = open_event_gc(gc, EVENT_CAPTURE_STOP);
		if (!gc->hook_stop) {
			warn("init_events: failed to get hook_stop event: %lu", GetLastError());
			return false;
		}
	}

	if (!gc->hook_init) {
		gc->hook_init = open_event_gc(gc, EVENT_HOOK_INIT);
		if (!gc->hook_init) {
			warn("init_events: failed to get hook_init event: %lu", GetLastError());
			return false;
		}
	}

	if (!gc->hook_ready) {
		gc->hook_ready = open_event_gc(gc, EVENT_HOOK_READY);
		if (!gc->hook_ready) {
			warn("init_events: failed to get hook_ready event: %lu", GetLastError());
			return false;
		}
	}

	if (!gc->hook_exit) {
		gc->hook_exit = open_event_gc(gc, EVENT_HOOK_EXIT);
		if (!gc->hook_exit) {
			warn("init_events: failed to get hook_exit event: %lu", GetLastError());
			return false;
		}
	}

	return true;
}

enum capture_result { CAPTURE_FAIL, CAPTURE_RETRY, CAPTURE_SUCCESS };

static inline bool init_data_map(struct game_capture *gc, HWND window)
{
	wchar_t name[64];
	swprintf(name, 64, SHMEM_TEXTURE "_%" PRIu64 "_", (uint64_t)(uintptr_t)window);

	gc->hook_data_map = open_map_plus_id(gc, name, gc->global_hook_info->map_id);
	return !!gc->hook_data_map;
}

static inline enum capture_result init_capture_data(struct game_capture *gc)
{
	gc->cx = gc->global_hook_info->cx;
	gc->cy = gc->global_hook_info->cy;
	gc->pitch = gc->global_hook_info->pitch;

	if (gc->data) {
		UnmapViewOfFile(gc->data);
		gc->data = NULL;
	}

	CloseHandle(gc->hook_data_map);

	DWORD error = 0;
	if (!init_data_map(gc, gc->window)) {
		HWND retry_hwnd = (HWND)(uintptr_t)gc->global_hook_info->window;
		error = GetLastError();

		/* if there's an error, just override.  some windows don't play
		 * nice. */
		if (init_data_map(gc, retry_hwnd)) {
			error = 0;
		}
	}

	if (!gc->hook_data_map) {
		if (error == 2) {
			return CAPTURE_RETRY;
		} else {
			warn("init_capture_data: failed to open file "
			     "mapping: %lu",
			     error);
		}
		return CAPTURE_FAIL;
	}

	gc->data = MapViewOfFile(gc->hook_data_map, FILE_MAP_ALL_ACCESS, 0, 0, gc->global_hook_info->map_size);
	if (!gc->data) {
		warn("init_capture_data: failed to map data view: %lu", GetLastError());
		return CAPTURE_FAIL;
	}

	return CAPTURE_SUCCESS;
}

static inline uint32_t convert_5_to_8bit(uint16_t val)
{
	return (uint32_t)((double)(val & 0x1F) * (255.0 / 31.0));
}

static inline uint32_t convert_6_to_8bit(uint16_t val)
{
	return (uint32_t)((double)(val & 0x3F) * (255.0 / 63.0));
}

static void copy_b5g6r5_tex(struct game_capture *gc, int cur_texture, uint8_t *data, uint32_t pitch)
{
	uint8_t *input = gc->texture_buffers[cur_texture];
	uint32_t gc_cx = gc->cx;
	uint32_t gc_cy = gc->cy;
	uint32_t gc_pitch = gc->pitch;

	for (size_t y = 0; y < gc_cy; y++) {
		uint8_t *row = input + (gc_pitch * y);
		uint8_t *out = data + (pitch * y);

		for (size_t x = 0; x < gc_cx; x += 8) {
			__m128i pixels_blue, pixels_green, pixels_red;
			__m128i pixels_result;
			__m128i *pixels_dest;

			__m128i *pixels_src = (__m128i *)(row + x * sizeof(uint16_t));
			__m128i pixels = _mm_load_si128(pixels_src);

			__m128i zero = _mm_setzero_si128();
			__m128i pixels_low = _mm_unpacklo_epi16(pixels, zero);
			__m128i pixels_high = _mm_unpackhi_epi16(pixels, zero);

			__m128i blue_channel_mask = _mm_set1_epi32(0x0000001F);
			__m128i blue_offset = _mm_set1_epi32(0x00000003);
			__m128i green_channel_mask = _mm_set1_epi32(0x000007E0);
			__m128i green_offset = _mm_set1_epi32(0x00000008);
			__m128i red_channel_mask = _mm_set1_epi32(0x0000F800);
			__m128i red_offset = _mm_set1_epi32(0x00000300);

			pixels_blue = _mm_and_si128(pixels_low, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_low, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 5);

			pixels_red = _mm_and_si128(pixels_low, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 8);

			pixels_result = _mm_set1_epi32(0xFF000000);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);
			pixels_result = _mm_or_si128(pixels_result, pixels_red);

			pixels_dest = (__m128i *)(out + x * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);

			pixels_blue = _mm_and_si128(pixels_high, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_high, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 5);

			pixels_red = _mm_and_si128(pixels_high, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 8);

			pixels_result = _mm_set1_epi32(0xFF000000);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);
			pixels_result = _mm_or_si128(pixels_result, pixels_red);

			pixels_dest = (__m128i *)(out + (x + 4) * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);
		}
	}
}

static void copy_b5g5r5a1_tex(struct game_capture *gc, int cur_texture, uint8_t *data, uint32_t pitch)
{
	uint8_t *input = gc->texture_buffers[cur_texture];
	uint32_t gc_cx = gc->cx;
	uint32_t gc_cy = gc->cy;
	uint32_t gc_pitch = gc->pitch;

	for (size_t y = 0; y < gc_cy; y++) {
		uint8_t *row = input + (gc_pitch * y);
		uint8_t *out = data + (pitch * y);

		for (size_t x = 0; x < gc_cx; x += 8) {
			__m128i pixels_blue, pixels_green, pixels_red, pixels_alpha;
			__m128i pixels_result;
			__m128i *pixels_dest;

			__m128i *pixels_src = (__m128i *)(row + x * sizeof(uint16_t));
			__m128i pixels = _mm_load_si128(pixels_src);

			__m128i zero = _mm_setzero_si128();
			__m128i pixels_low = _mm_unpacklo_epi16(pixels, zero);
			__m128i pixels_high = _mm_unpackhi_epi16(pixels, zero);

			__m128i blue_channel_mask = _mm_set1_epi32(0x0000001F);
			__m128i blue_offset = _mm_set1_epi32(0x00000003);
			__m128i green_channel_mask = _mm_set1_epi32(0x000003E0);
			__m128i green_offset = _mm_set1_epi32(0x000000C);
			__m128i red_channel_mask = _mm_set1_epi32(0x00007C00);
			__m128i red_offset = _mm_set1_epi32(0x00000180);
			__m128i alpha_channel_mask = _mm_set1_epi32(0x00008000);
			__m128i alpha_offset = _mm_set1_epi32(0x00000001);
			__m128i alpha_mask32 = _mm_set1_epi32(0xFF000000);

			pixels_blue = _mm_and_si128(pixels_low, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_low, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 6);

			pixels_red = _mm_and_si128(pixels_low, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 9);

			pixels_alpha = _mm_and_si128(pixels_low, alpha_channel_mask);
			pixels_alpha = _mm_srli_epi32(pixels_alpha, 15);
			pixels_alpha = _mm_sub_epi32(pixels_alpha, alpha_offset);
			pixels_alpha = _mm_andnot_si128(pixels_alpha, alpha_mask32);

			pixels_result = pixels_red;
			pixels_result = _mm_or_si128(pixels_result, pixels_alpha);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);

			pixels_dest = (__m128i *)(out + x * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);

			pixels_blue = _mm_and_si128(pixels_high, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_high, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 6);

			pixels_red = _mm_and_si128(pixels_high, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 9);

			pixels_alpha = _mm_and_si128(pixels_high, alpha_channel_mask);
			pixels_alpha = _mm_srli_epi32(pixels_alpha, 15);
			pixels_alpha = _mm_sub_epi32(pixels_alpha, alpha_offset);
			pixels_alpha = _mm_andnot_si128(pixels_alpha, alpha_mask32);

			pixels_result = pixels_red;
			pixels_result = _mm_or_si128(pixels_result, pixels_alpha);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);

			pixels_dest = (__m128i *)(out + (x + 4) * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);
		}
	}
}

static inline void copy_16bit_tex(struct game_capture *gc, int cur_texture, uint8_t *data, uint32_t pitch)
{
	if (gc->global_hook_info->format == DXGI_FORMAT_B5G5R5A1_UNORM) {
		copy_b5g5r5a1_tex(gc, cur_texture, data, pitch);

	} else if (gc->global_hook_info->format == DXGI_FORMAT_B5G6R5_UNORM) {
		copy_b5g6r5_tex(gc, cur_texture, data, pitch);
	}
}

static void copy_shmem_tex(struct game_capture *gc)
{
	int cur_texture;
	HANDLE mutex = NULL;
	uint32_t pitch;
	int next_texture;
	uint8_t *data;

	if (!gc->shmem_data)
		return;

	cur_texture = gc->shmem_data->last_tex;

	if (cur_texture < 0 || cur_texture > 1)
		return;

	next_texture = cur_texture == 1 ? 0 : 1;

	if (object_signalled(gc->texture_mutexes[cur_texture])) {
		mutex = gc->texture_mutexes[cur_texture];

	} else if (object_signalled(gc->texture_mutexes[next_texture])) {
		mutex = gc->texture_mutexes[next_texture];
		cur_texture = next_texture;

	} else {
		return;
	}

	if (gs_texture_map(gc->texture, &data, &pitch)) {
		if (gc->convert_16bit) {
			copy_16bit_tex(gc, cur_texture, data, pitch);

		} else if (pitch == gc->pitch) {
			memcpy(data, gc->texture_buffers[cur_texture], (size_t)pitch * (size_t)gc->cy);
		} else {
			uint8_t *input = gc->texture_buffers[cur_texture];
			uint32_t best_pitch = pitch < gc->pitch ? pitch : gc->pitch;

			for (size_t y = 0; y < gc->cy; y++) {
				uint8_t *line_in = input + gc->pitch * y;
				uint8_t *line_out = data + pitch * y;
				memcpy(line_out, line_in, best_pitch);
			}
		}

		gs_texture_unmap(gc->texture);
	}

	ReleaseMutex(mutex);
}

static inline bool is_16bit_format(uint32_t format)
{
	return format == DXGI_FORMAT_B5G5R5A1_UNORM || format == DXGI_FORMAT_B5G6R5_UNORM;
}

static inline bool init_shmem_capture(struct game_capture *gc)
{
	const uint32_t dxgi_format = gc->global_hook_info->format;
	const bool convert_16bit = is_16bit_format(dxgi_format);
	const enum gs_color_format format = convert_16bit ? GS_BGRA : convert_format(dxgi_format);

	obs_enter_graphics();
	gs_texrender_destroy(gc->extra_texrender);
	gc->extra_texrender = NULL;
	gs_texture_destroy(gc->extra_texture);
	gc->extra_texture = NULL;
	gs_texture_destroy(gc->texture);
	gc->texture = NULL;
	gs_texture_t *const texture = gs_texture_create(gc->cx, gc->cy, format, 1, NULL, GS_DYNAMIC);
	obs_leave_graphics();

	bool success = texture != NULL;
	if (success) {
		const bool linear_sample = format != GS_R10G10B10A2;

		gs_texrender_t *extra_texrender = NULL;
		if (!linear_sample) {
			extra_texrender = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
			success = extra_texrender != NULL;
			if (!success)
				warn("init_shmem_capture: failed to create extra texrender");
		}

		if (success) {
			gc->texture_buffers[0] = (uint8_t *)gc->data + gc->shmem_data->tex1_offset;
			gc->texture_buffers[1] = (uint8_t *)gc->data + gc->shmem_data->tex2_offset;
			gc->convert_16bit = convert_16bit;

			gc->texture = texture;
			gc->extra_texture = NULL;
			gc->extra_texrender = extra_texrender;
			gc->linear_sample = linear_sample;
			gc->copy_texture = copy_shmem_tex;
		} else {
			gs_texture_destroy(texture);
		}
	} else {
		warn("init_shmem_capture: failed to create texture");
	}

	return success;
}

static inline bool init_shtex_capture(struct game_capture *gc)
{
	obs_enter_graphics();
	gs_texrender_destroy(gc->extra_texrender);
	gc->extra_texrender = NULL;
	gs_texture_destroy(gc->extra_texture);
	gc->extra_texture = NULL;
	gs_texture_destroy(gc->texture);
	gc->texture = NULL;
	gs_texture_t *const texture = gs_texture_open_shared(gc->shtex_data->tex_handle);
	bool success = texture != NULL;
	if (success) {
		enum gs_color_format format = gs_texture_get_color_format(texture);
		const bool ten_bit_srgb = (format == GS_R10G10B10A2);
		enum gs_color_format linear_format = ten_bit_srgb ? GS_RGBA16F : gs_generalize_format(format);
		const bool linear_sample = (linear_format == format);
		gs_texture_t *extra_texture = NULL;
		gs_texrender_t *extra_texrender = NULL;
		if (!linear_sample) {
			if (ten_bit_srgb) {
				extra_texrender = gs_texrender_create(linear_format, GS_ZS_NONE);
				success = extra_texrender != NULL;
				if (!success)
					warn("init_shtex_capture: failed to create extra texrender");
			} else {
				extra_texture = gs_texture_create(gs_texture_get_width(texture),
								  gs_texture_get_height(texture), linear_format, 1,
								  NULL, 0);
				success = extra_texture != NULL;
				if (!success)
					warn("init_shtex_capture: failed to create extra texture");
			}
		}

		if (success) {
			gc->texture = texture;
			gc->linear_sample = linear_sample;
			gc->extra_texture = extra_texture;
			gc->extra_texrender = extra_texrender;
		} else {
			gs_texture_destroy(texture);
		}
	} else {
		warn("init_shtex_capture: failed to open shared handle");
	}
	obs_leave_graphics();

	return success;
}

static bool start_capture(struct game_capture *gc)
{
	debug("Starting capture");

	/* prevent from using a DLL version that's higher than current */
	if (gc->global_hook_info->hook_ver_major > HOOK_VER_MAJOR) {
		warn("cannot initialize hook, DLL hook version is "
		     "%" PRIu32 ".%" PRIu32 ", current plugin hook major version is %d.%d",
		     gc->global_hook_info->hook_ver_major, gc->global_hook_info->hook_ver_minor, HOOK_VER_MAJOR,
		     HOOK_VER_MINOR);
		return false;
	}

	if (gc->global_hook_info->type == CAPTURE_TYPE_MEMORY) {
		if (!init_shmem_capture(gc)) {
			return false;
		}

		info("memory capture successful");
	} else {
		if (!init_shtex_capture(gc)) {
			return false;
		}

		info("shared texture capture successful");
	}

	return true;
}

static inline bool capture_valid(struct game_capture *gc)
{
	if (!gc->dwm_capture && !IsWindow(gc->window))
		return false;

	return !object_signalled(gc->target_process);
}

static void check_foreground_window(struct game_capture *gc, float seconds)
{
	// Hides the cursor if the user isn't actively in the game
	gc->cursor_check_time += seconds;
	if (gc->cursor_check_time >= 0.1f) {
		DWORD foreground_process_id;
		GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process_id);
		if (gc->process_id != foreground_process_id)
			gc->cursor_hidden = true;
		else
			gc->cursor_hidden = false;
		gc->cursor_check_time = 0.0f;
	}
}

static void game_capture_tick(void *data, float seconds)
{
	struct game_capture *gc = data;
	bool deactivate = os_atomic_set_bool(&gc->deactivate_hook, false);
	bool activate_now = os_atomic_set_bool(&gc->activate_hook_now, false);

	if (activate_now) {
		HWND hwnd = (HWND)(uintptr_t)os_atomic_load_long(&gc->hotkey_window);

		if (ms_is_uwp_window(hwnd))
			hwnd = ms_get_uwp_actual_window(hwnd);

		if (ms_get_window_exe(&gc->executable, hwnd)) {
			ms_get_window_title(&gc->title, hwnd);
			ms_get_window_class(&gc->class, hwnd);

			gc->priority = WINDOW_PRIORITY_CLASS;
			gc->retry_time = 10.0f * hook_rate_to_float(gc->config.hook_rate);
			gc->activate_hook = true;
		} else {
			deactivate = false;
			activate_now = false;
		}
	} else if (deactivate) {
		gc->activate_hook = false;
	}

	if (!obs_source_showing(gc->source)) {
		if (gc->showing) {
			if (gc->active || gc->injector_lease)
				stop_capture(gc, true);
			gc->showing = false;
		}
		return;

	} else if (!gc->showing) {
		gc->retry_time = 10.0f * hook_rate_to_float(gc->config.hook_rate);
	}

	if (gc->hook_stop && object_signalled(gc->hook_stop)) {
		debug("hook stop signal received");
		stop_capture(gc, true);
	}
	if (deactivate) {
		stop_capture(gc, true);
	}

	if (gc->active && !gc->hook_ready && gc->process_id) {
		gc->hook_ready = open_event_gc(gc, EVENT_HOOK_READY);
	}

	if (gc->hook_ready && object_signalled(gc->hook_ready)) {
		debug("capture initializing!");
		enum capture_result result = init_capture_data(gc);

		if (result == CAPTURE_SUCCESS)
			gc->capturing = start_capture(gc);
		else
			debug("init_capture_data failed");

		// If capture was successful, send a hooked signal
		if (gc->capturing) {
			if (gc->config.mode == CAPTURE_MODE_ANY) {
				ms_get_window_exe(&gc->executable, gc->window);
				ms_get_window_title(&gc->title, gc->window);
				ms_get_window_class(&gc->class, gc->window);
			}
			signal_handler_t *sh = obs_source_get_signal_handler(gc->source);
			calldata_t data = {0};

			calldata_set_ptr(&data, "source", gc->source);
			calldata_set_string(&data, "title", gc->title.array);
			calldata_set_string(&data, "class", gc->class.array);
			calldata_set_string(&data, "executable", gc->executable.array);

			signal_handler_signal(sh, "hooked", &data);
			calldata_free(&data);

			if (gc->audio_source) {
				reconfigure_audio_source(gc->audio_source, gc->window);
			}
		}
		if (result != CAPTURE_RETRY && !gc->capturing) {
			gc->retry_interval = ERROR_RETRY_INTERVAL * hook_rate_to_float(gc->config.hook_rate);
			stop_capture(gc, false);
		}
	}

	gc->retry_time += seconds;

	if (!gc->active) {
		if (!gc->error_acquiring && gc->retry_time > gc->retry_interval) {
			if (gc->config.mode == CAPTURE_MODE_ANY || gc->activate_hook) {
				try_hook(gc);
				gc->retry_time = 0.0f;
			}
		}
	} else {
		if (!capture_valid(gc)) {
			info("capture window no longer exists, "
			     "terminating capture");
			stop_capture(gc, true);
		} else {
			if (gc->copy_texture) {
				obs_enter_graphics();
				gc->copy_texture(gc);
				obs_leave_graphics();
			}

			if (gc->config.cursor) {
				DPI_AWARENESS_CONTEXT previous = NULL;
				if (gc->get_window_dpi_awareness_context != NULL) {
					const DPI_AWARENESS_CONTEXT context =
						gc->get_window_dpi_awareness_context(gc->window);
					previous = gc->set_thread_dpi_awareness_context(context);
				}

				check_foreground_window(gc, seconds);
				obs_enter_graphics();
				cursor_capture(&gc->cursor_data);
				obs_leave_graphics();

				if (previous) {
					gc->set_thread_dpi_awareness_context(previous);
				}
			}

			gc->fps_reset_time += seconds;
			if (gc->fps_reset_time >= gc->retry_interval) {
				reset_frame_interval(gc);
				gc->fps_reset_time = 0.0f;
			}
		}
	}

	if (!gc->showing)
		gc->showing = true;
}

static inline void game_capture_render_cursor(struct game_capture *gc)
{
	POINT p = {0};
	HWND window;

	if (!gc->global_hook_info->cx || !gc->global_hook_info->cy)
		return;

	window = !!gc->global_hook_info->window ? (HWND)(uintptr_t)gc->global_hook_info->window : gc->window;

	DPI_AWARENESS_CONTEXT previous = NULL;
	if (gc->get_window_dpi_awareness_context != NULL) {
		const DPI_AWARENESS_CONTEXT context = gc->get_window_dpi_awareness_context(gc->window);
		previous = gc->set_thread_dpi_awareness_context(context);
	}

	ClientToScreen(window, &p);

	if (previous)
		gc->set_thread_dpi_awareness_context(previous);

	cursor_draw(&gc->cursor_data, -p.x, -p.y, gc->global_hook_info->cx, gc->global_hook_info->cy);
}

static void game_capture_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);

	struct game_capture *gc = data;
	if (!gc->texture || !gc->active)
		return;

	const bool allow_transparency = gc->config.allow_transparency;
	gs_effect_t *const effect = obs_get_base_effect(allow_transparency ? OBS_EFFECT_DEFAULT : OBS_EFFECT_OPAQUE);

	gs_texture_t *texture = gc->texture;
	enum gs_color_space source_space = GS_CS_SRGB;
	bool is_10a2_compressed = false;
	if (gs_texture_get_color_format(texture) == GS_R10G10B10A2) {
		is_10a2_compressed = true;
		source_space = gc->is_10a2_2100pq ? GS_CS_709_EXTENDED : GS_CS_SRGB_16F;
	} else if (gs_texture_get_color_format(texture) == GS_RGBA16F) {
		source_space = GS_CS_709_SCRGB;
	}

	bool linear_sample = gc->linear_sample;
	const enum gs_color_space current_space = gs_get_color_space();
	const bool texcoords_centered = obs_source_get_texcoords_centered(gc->source);
	if (!linear_sample && !texcoords_centered) {
		if (gs_texture_get_color_format(texture) == GS_R10G10B10A2) {
			gs_texrender_t *const texrender = gc->extra_texrender;
			gs_texrender_reset(texrender);
			const uint32_t cx = gs_texture_get_width(texture);
			const uint32_t cy = gs_texture_get_height(texture);
			if (gs_texrender_begin(texrender, cx, cy)) {
				gs_effect_t *const default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
				gs_enable_blending(false);
				gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
				gs_eparam_t *const image = gs_effect_get_param_by_name(default_effect, "image");
				gs_effect_set_texture(image, texture);

				const char *tech_name = "DrawSrgbDecompress";
				if (gc->is_10a2_2100pq) {
					tech_name = "DrawPQ";

					const float multiplier = 10000.f / obs_get_video_sdr_white_level();
					gs_effect_set_float(gs_effect_get_param_by_name(default_effect, "multiplier"),
							    multiplier);
				}

				while (gs_effect_loop(default_effect, tech_name)) {
					gs_draw_sprite(texture, 0, 0, 0);
				}
				gs_enable_blending(true);

				gs_texrender_end(texrender);

				texture = gs_texrender_get_texture(texrender);
			}

			is_10a2_compressed = false;
		} else {
			gs_texture_t *const extra_texture = gc->extra_texture;
			if (extra_texture) {
				gs_copy_texture(extra_texture, texture);
				texture = extra_texture;
			} else {
				gs_texrender_t *const texrender = gc->extra_texrender;
				gs_texrender_reset(texrender);
				const uint32_t cx = gs_texture_get_width(texture);
				const uint32_t cy = gs_texture_get_height(texture);
				if (gs_texrender_begin(texrender, cx, cy)) {
					gs_effect_t *const default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
					const bool previous = gs_framebuffer_srgb_enabled();
					gs_enable_framebuffer_srgb(false);
					gs_enable_blending(false);
					gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
					gs_eparam_t *const image = gs_effect_get_param_by_name(default_effect, "image");
					gs_effect_set_texture(image, texture);
					while (gs_effect_loop(default_effect, "Draw")) {
						gs_draw_sprite(texture, 0, 0, 0);
					}
					gs_enable_blending(true);
					gs_enable_framebuffer_srgb(previous);

					gs_texrender_end(texrender);

					texture = gs_texrender_get_texture(texrender);
				}
			}
		}

		linear_sample = true;
	}

	gs_eparam_t *const image = gs_effect_get_param_by_name(effect, "image");
	const uint32_t flip = gc->global_hook_info->flip ? GS_FLIP_V : 0;

	const char *tech_name = "Draw";
	float multiplier = 1.f;
	switch (source_space) {
	case GS_CS_SRGB:
		switch (current_space) {
		case GS_CS_SRGB:
			if (allow_transparency && !linear_sample)
				tech_name = "DrawSrgbDecompress";
			break;
		case GS_CS_SRGB_16F:
		case GS_CS_709_EXTENDED:
			if (!linear_sample)
				tech_name = "DrawSrgbDecompress";
			break;
		case GS_CS_709_SCRGB:
			if (linear_sample)
				tech_name = "DrawMultiply";
			else
				tech_name = "DrawSrgbDecompressMultiply";
			multiplier = obs_get_video_sdr_white_level() / 80.f;
		}
		break;
	case GS_CS_SRGB_16F:
		linear_sample = true;
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
		case GS_CS_709_EXTENDED:
			tech_name = is_10a2_compressed ? "DrawSrgbDecompress" : "Draw";
			break;
		case GS_CS_709_SCRGB:
			tech_name = is_10a2_compressed ? "DrawSrgbDecompressMultiply" : "DrawMultiply";
			multiplier = obs_get_video_sdr_white_level() / 80.f;
		}
		break;
	case GS_CS_709_EXTENDED:
		linear_sample = true;
		if (is_10a2_compressed) {
			switch (current_space) {
			case GS_CS_SRGB:
			case GS_CS_SRGB_16F:
				tech_name = "DrawTonemapPQ";
				multiplier = 10000.f / obs_get_video_sdr_white_level();
				break;
			case GS_CS_709_EXTENDED:
				tech_name = "DrawPQ";
				multiplier = 10000.f / obs_get_video_sdr_white_level();
				break;
			case GS_CS_709_SCRGB:
				tech_name = "DrawPQ";
				multiplier = 10000.f / 80.f;
			}
		} else {
			switch (current_space) {
			case GS_CS_SRGB:
			case GS_CS_SRGB_16F:
				tech_name = "DrawTonemap";
				break;
			case GS_CS_709_SCRGB:
				tech_name = "DrawMultiply";
				multiplier = obs_get_video_sdr_white_level() / 80.f;
			}
		}
		break;
	case GS_CS_709_SCRGB:
		linear_sample = true;
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawMultiplyTonemap";
			multiplier = 80.f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_EXTENDED:
			tech_name = "DrawMultiply";
			multiplier = 80.f / obs_get_video_sdr_white_level();
		}
	}

	while (gs_effect_loop(effect, tech_name)) {
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(allow_transparency || linear_sample);
		gs_blend_state_push();
		gs_enable_blending(allow_transparency);
		gs_blend_function_separate(gc->config.premultiplied_alpha ? GS_BLEND_ONE : GS_BLEND_SRCALPHA,
					   GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		if (linear_sample)
			gs_effect_set_texture_srgb(image, texture);
		else
			gs_effect_set_texture(image, texture);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), multiplier);
		gs_draw_sprite(texture, flip, 0, 0);
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previous);
	}

	if (gc->config.cursor && !gc->cursor_hidden) {
		gs_effect_t *const default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

		const char *cursor_tech_name = "Draw";
		float cursor_multiplier = 1.f;
		if (current_space == GS_CS_709_SCRGB) {
			cursor_tech_name = "DrawMultiply";
			cursor_multiplier = obs_get_video_sdr_white_level() / 80.f;
		}

		const bool previous = gs_set_linear_srgb(true);

		while (gs_effect_loop(default_effect, cursor_tech_name)) {
			gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), cursor_multiplier);
			game_capture_render_cursor(gc);
		}

		gs_set_linear_srgb(previous);
	}
}

static uint32_t game_capture_width(void *data)
{
	struct game_capture *gc = data;
	return (gc->active && gc->capturing) ? gc->cx : 0;
}

static uint32_t game_capture_height(void *data)
{
	struct game_capture *gc = data;
	return (gc->active && gc->capturing) ? gc->cy : 0;
}

static const char *game_capture_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_GAME_CAPTURE;
}

static void game_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SETTING_MODE, SETTING_MODE_ANY);
	obs_data_set_default_int(settings, SETTING_WINDOW_PRIORITY, (int)WINDOW_PRIORITY_EXE);
	obs_data_set_default_bool(settings, SETTING_COMPATIBILITY, false);
	obs_data_set_default_bool(settings, SETTING_CURSOR, true);
	obs_data_set_default_bool(settings, SETTING_TRANSPARENCY, false);
	obs_data_set_default_bool(settings, SETTING_PREMULTIPLIED_ALPHA, false);
	obs_data_set_default_bool(settings, SETTING_LIMIT_FRAMERATE, false);
	obs_data_set_default_bool(settings, SETTING_CAPTURE_OVERLAYS, false);
	obs_data_set_default_bool(settings, SETTING_ANTI_CHEAT_HOOK, true);
	obs_data_set_default_int(settings, SETTING_HOOK_RATE, (int)HOOK_RATE_NORMAL);
	obs_data_set_default_string(settings, SETTING_RGBA10A2_SPACE, RGBA10A2_SPACE_SRGB);
}

static bool mode_callback(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	bool capture_window;

	if (using_older_non_mode_format(settings)) {
		capture_window = !obs_data_get_bool(settings, SETTING_ANY_FULLSCREEN);
	} else {
		const char *mode = obs_data_get_string(settings, SETTING_MODE);
		capture_window = strcmp(mode, SETTING_MODE_WINDOW) == 0;
	}

	p = obs_properties_get(ppts, SETTING_CAPTURE_WINDOW);
	obs_property_set_visible(p, capture_window);

	p = obs_properties_get(ppts, SETTING_WINDOW_PRIORITY);
	obs_property_set_visible(p, capture_window);

	return true;
}

static bool window_changed_callback(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	bool modified = ms_check_window_property_setting(ppts, p, settings, SETTING_CAPTURE_WINDOW, 1);

	bool capture_any;
	if (using_older_non_mode_format(settings)) {
		capture_any = obs_data_get_bool(settings, SETTING_ANY_FULLSCREEN);
	} else {
		const char *mode = obs_data_get_string(settings, SETTING_MODE);
		capture_any = strcmp(mode, SETTING_MODE_ANY) == 0 || strcmp(mode, SETTING_MODE_HOTKEY) == 0;
	}

	if (capture_any)
		return modified;

	const char *window = obs_data_get_string(settings, SETTING_CAPTURE_WINDOW);

	char *class;
	char *exe;
	char *title;
	ms_build_window_strings(window, &class, &title, &exe);
	struct compat_result *compat = check_compatibility(title, class, exe, GAME_CAPTURE);
	bfree(title);
	bfree(exe);
	bfree(class);

	obs_property_t *p_warn = obs_properties_get(ppts, SETTINGS_COMPAT_INFO);

	if (!compat) {
		modified = obs_property_visible(p_warn) || modified;
		obs_property_set_visible(p_warn, false);
		return modified;
	}

	obs_property_set_long_description(p_warn, compat->message);
	obs_property_text_set_info_type(p_warn, compat->severity);
	obs_property_set_visible(p_warn, true);

	compat_result_free(compat);

	return true;
}

static BOOL CALLBACK EnumFirstMonitor(HMONITOR monitor, HDC hdc, LPRECT rc, LPARAM data)
{
	*(HMONITOR *)data = monitor;

	UNUSED_PARAMETER(hdc);
	UNUSED_PARAMETER(rc);
	return false;
}

static bool window_not_blacklisted(const char *title, const char *class, const char *exe)
{
	UNUSED_PARAMETER(title);
	UNUSED_PARAMETER(class);

	return !is_blacklisted_exe(exe);
}

static obs_properties_t *game_capture_properties(void *data)
{
	HMONITOR monitor;
	uint32_t cx = 1920;
	uint32_t cy = 1080;

	/* scaling is free form, this is mostly just to provide some common
	 * values */
	bool success = !!EnumDisplayMonitors(NULL, NULL, EnumFirstMonitor, (LPARAM)&monitor);
	if (success) {
		MONITORINFO mi = {0};
		mi.cbSize = sizeof(mi);

		if (!!GetMonitorInfo(monitor, &mi)) {
			cx = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
			cy = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
		}
	}

	/* update from deprecated settings */
	if (data) {
		struct game_capture *gc = data;
		obs_data_t *settings = obs_source_get_settings(gc->source);
		if (using_older_non_mode_format(settings)) {
			bool any = obs_data_get_bool(settings, SETTING_ANY_FULLSCREEN);
			const char *mode = any ? SETTING_MODE_ANY : SETTING_MODE_WINDOW;

			obs_data_set_string(settings, SETTING_MODE, mode);
		}
		obs_data_release(settings);
	}

	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(ppts, SETTING_MODE, TEXT_MODE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, TEXT_MODE_ANY, SETTING_MODE_ANY);
	obs_property_list_add_string(p, TEXT_MODE_WINDOW, SETTING_MODE_WINDOW);
	obs_property_list_add_string(p, TEXT_MODE_HOTKEY, SETTING_MODE_HOTKEY);

	obs_property_set_modified_callback(p, mode_callback);

	p = obs_properties_add_list(ppts, SETTING_CAPTURE_WINDOW, TEXT_WINDOW, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "", "");
	ms_fill_window_list(p, INCLUDE_MINIMIZED, window_not_blacklisted);

	obs_property_set_modified_callback(p, window_changed_callback);

	p = obs_properties_add_list(ppts, SETTING_WINDOW_PRIORITY, TEXT_MATCH_PRIORITY, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TEXT_MATCH_TITLE, WINDOW_PRIORITY_TITLE);
	obs_property_list_add_int(p, TEXT_MATCH_CLASS, WINDOW_PRIORITY_CLASS);
	obs_property_list_add_int(p, TEXT_MATCH_EXE, WINDOW_PRIORITY_EXE);

	p = obs_properties_add_text(ppts, SETTINGS_COMPAT_INFO, NULL, OBS_TEXT_INFO);
	obs_property_set_enabled(p, false);

	if (audio_capture_available()) {
		p = obs_properties_add_bool(ppts, SETTING_CAPTURE_AUDIO, TEXT_CAPTURE_AUDIO);
		obs_property_set_long_description(p, TEXT_CAPTURE_AUDIO_TT);
	}

	obs_properties_add_bool(ppts, SETTING_COMPATIBILITY, TEXT_SLI_COMPATIBILITY);

	obs_properties_add_bool(ppts, SETTING_TRANSPARENCY, TEXT_ALLOW_TRANSPARENCY);

	obs_properties_add_bool(ppts, SETTING_PREMULTIPLIED_ALPHA, TEXT_PREMULTIPLIED_ALPHA);

	obs_properties_add_bool(ppts, SETTING_LIMIT_FRAMERATE, TEXT_LIMIT_FRAMERATE);

	obs_properties_add_bool(ppts, SETTING_CURSOR, TEXT_CAPTURE_CURSOR);

	obs_properties_add_bool(ppts, SETTING_ANTI_CHEAT_HOOK, TEXT_ANTI_CHEAT_HOOK);

	obs_properties_add_bool(ppts, SETTING_CAPTURE_OVERLAYS, TEXT_CAPTURE_OVERLAYS);

	p = obs_properties_add_list(ppts, SETTING_HOOK_RATE, TEXT_HOOK_RATE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TEXT_HOOK_RATE_SLOW, HOOK_RATE_SLOW);
	obs_property_list_add_int(p, TEXT_HOOK_RATE_NORMAL, HOOK_RATE_NORMAL);
	obs_property_list_add_int(p, TEXT_HOOK_RATE_FAST, HOOK_RATE_FAST);
	obs_property_list_add_int(p, TEXT_HOOK_RATE_FASTEST, HOOK_RATE_FASTEST);

	p = obs_properties_add_list(ppts, SETTING_RGBA10A2_SPACE, TEXT_RGBA10A2_SPACE, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, TEXT_RGBA10A2_SPACE_SRGB, RGBA10A2_SPACE_SRGB);
	obs_property_list_add_string(p, TEXT_RGBA10A2_SPACE_2100PQ, RGBA10A2_SPACE_2100PQ);

	return ppts;
}

enum gs_color_space game_capture_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	enum gs_color_space capture_space = GS_CS_SRGB;

	struct game_capture *const gc = data;
	if (gc->texture) {
		const enum gs_color_format format = gs_texture_get_color_format(gc->texture);
		if (((format == GS_R10G10B10A2) && gc->is_10a2_2100pq) || (format == GS_RGBA16F)) {
			for (size_t i = 0; i < count; ++i) {
				if (preferred_spaces[i] == GS_CS_709_SCRGB)
					return GS_CS_709_SCRGB;
			}

			capture_space = GS_CS_709_EXTENDED;
		} else if (format == GS_R10G10B10A2) {
			for (size_t i = 0; i < count; ++i) {
				if (preferred_spaces[i] == GS_CS_SRGB_16F)
					return GS_CS_SRGB_16F;
			}

			capture_space = GS_CS_709_EXTENDED;
		}
	}

	enum gs_color_space space = capture_space;
	for (size_t i = 0; i < count; ++i) {
		const enum gs_color_space preferred_space = preferred_spaces[i];
		space = preferred_space;
		if (preferred_space == capture_space)
			break;
	}

	return space;
}

static void game_capture_enum(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct game_capture *gc = data;
	if (gc->audio_source)
		cb(gc->source, gc->audio_source, param);
}

struct obs_source_info game_capture_info = {
	.id = "game_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_SRGB,
	.get_name = game_capture_name,
	.create = game_capture_create,
	.destroy = game_capture_destroy,
	.get_width = game_capture_width,
	.get_height = game_capture_height,
	.get_defaults = game_capture_defaults,
	.get_properties = game_capture_properties,
	.enum_active_sources = game_capture_enum,
	.update = game_capture_update,
	.video_tick = game_capture_tick,
	.video_render = game_capture_render,
	.icon_type = OBS_ICON_TYPE_GAME_CAPTURE,
	.video_get_color_space = game_capture_get_color_space,
};
