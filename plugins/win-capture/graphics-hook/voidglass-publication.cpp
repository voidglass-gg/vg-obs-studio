#include <windows.h>
#include <new>
#include <voidglass-publication.h>
#if defined(_M_X64) || defined(__x86_64__) || defined(VG_PUBLICATION_TEST_BACKEND)
#ifndef VG_PUBLICATION_TEST_BACKEND
#include <d3d11_4.h>
#include <wrl/client.h>
#include "graphics-hook.h"
using Microsoft::WRL::ComPtr;
namespace {
struct publication_backend {
	ComPtr<ID3D11Fence> fence;
	ComPtr<ID3D11DeviceContext4> immediate;
	HRESULT signal(uint64_t value) { return immediate->Signal(fence.Get(), value); }
	uint64_t completed() { return fence->GetCompletedValue(); }
};
publication_backend *make_backend(void *dev, void *ctx, UINT &flags, HRESULT &hr)
{
	auto *device = static_cast<ID3D11Device *>(dev);
	auto *context = static_cast<ID3D11DeviceContext *>(ctx);
	flags = device->GetCreationFlags();
	hr = E_NOINTERFACE;
	if (flags & D3D11_CREATE_DEVICE_SINGLETHREADED)
		return nullptr;
	if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
		return nullptr;
	auto *backend = new (std::nothrow) publication_backend;
	if (!backend) {
		hr = E_OUTOFMEMORY;
		return nullptr;
	}
	ComPtr<ID3D11Device5> device5;
	hr = device->QueryInterface(IID_PPV_ARGS(&device5));
	if (SUCCEEDED(hr))
		hr = context->QueryInterface(IID_PPV_ARGS(&backend->immediate));
	if (SUCCEEDED(hr))
		hr = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&backend->fence));
	if (FAILED(hr)) {
		delete backend;
		return nullptr;
	}
	return backend;
}
} // namespace
#endif // Tests substitute only the driver adapter, not the state/locking code below.
namespace {
SRWLOCK mutex = SRWLOCK_INIT;
vg_pub_payload state{};
struct capture_slot {
	LONG references = 1; // Owner plus operation leases; no COM calls while taking a lease.
	publication_backend *backend;
	bool signaling = false; // Reject overlap instead of serializing a game context or waiting.
	explicit capture_slot(publication_backend *value) : backend(value) {}
	~capture_slot() { delete backend; }
};
capture_slot *current = nullptr;
HANDLE mapping = nullptr;
vg_pub_shared *shared = nullptr;
bool running = false;
void release(capture_slot *slot)
{
	// Every caller invokes this OUTSIDE mutex: final COM Release can reenter the hook.
	if (slot && InterlockedDecrement(&slot->references) == 0)
		delete slot;
}
capture_slot *lease_locked()
{
	if (current)
		InterlockedIncrement(&current->references);
	return current;
}
void publish_locked()
{
	if (!shared)
		return;
	InterlockedIncrement(&shared->sequence);
	MemoryBarrier();
	memcpy(&shared->data, &state, sizeof(state));
	MemoryBarrier();
	InterlockedIncrement(&shared->sequence);
}
void reset_generation_locked()
{
	++state.generation;
	state.submitted = state.completed = 0;
	state.initialized_qpc = state.first_submitted_qpc = state.first_completed_qpc = state.last_completed_qpc = 0;
	state.capability = VG_CAP_PENDING;
	state.error = S_OK;
}
capture_slot *fail_locked(HRESULT error)
{
	// Keep the last committed counts monotone within this generation; capability invalidates them.
	state.capability = VG_CAP_ERROR;
	state.error = error;
	auto *retired = current;
	current = nullptr;
	publish_locked();
	return retired;
}
} // namespace
extern "C" void vg_pub_start(void)
{
	// Channel creation/closure belongs to the existing hook worker. No Win32/COM calls under mutex.
	vg_pub_payload initial{};
	initial.magic = VG_PUB_MAGIC;
	initial.size = sizeof(initial);
	initial.major = VG_PUB_MAJOR;
	initial.minor = VG_PUB_MINOR;
	initial.pid = GetCurrentProcessId();
	initial.creation = vg_pub_creation(GetCurrentProcess());
	initial.instance = vg_pub_qpc() ^ ((uint64_t)initial.pid << 32);
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	initial.frequency = frequency.QuadPart;
	wchar_t name[128];
	vg_pub_name(name, initial.pid, initial.creation);
	HANDLE opened =
		CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(vg_pub_shared), name);
	if (opened && GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(opened);
		opened = nullptr;
	}
	auto *view = opened ? static_cast<vg_pub_shared *>(
				      MapViewOfFile(opened, FILE_MAP_WRITE, 0, 0, sizeof(vg_pub_shared)))
			    : nullptr;
	AcquireSRWLockExclusive(&mutex);
	state = initial;
	mapping = opened;
	shared = view;
	running = true;
	publish_locked();
	ReleaseSRWLockExclusive(&mutex);
}
extern "C" void vg_pub_dxgi_installed(void)
{
	AcquireSRWLockExclusive(&mutex);
	state.dxgi_installed = 1;
	publish_locked();
	ReleaseSRWLockExclusive(&mutex);
}
extern "C" void vg_pub_capture_init(void *dev, void *ctx, HWND hwnd, uint32_t map_id)
{
	AcquireSRWLockExclusive(&mutex);
	auto *retired = current;
	current = nullptr;
	reset_generation_locked();
	const auto generation = state.generation;
	state.api = 11;
	state.transport = 1;
	state.hwnd = (uintptr_t)hwnd;
	state.map_id = map_id;
	state.native_initialized = 1;
	state.device_flags = 0;
	state.initialized_qpc = vg_pub_qpc();
	const bool available = running && shared;
	publish_locked();
	ReleaseSRWLockExclusive(&mutex);
	release(retired);
	UINT flags = 0;
	HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
	auto *backend = available ? make_backend(dev, ctx, flags, hr) : nullptr;
	auto *candidate = backend ? new (std::nothrow) capture_slot(backend) : nullptr;
	if (backend && !candidate) {
		delete backend;
		hr = E_OUTOFMEMORY;
	}
	AcquireSRWLockExclusive(&mutex);
	if (running && state.generation == generation && state.native_initialized) {
		current = candidate;
		candidate = nullptr;
		state.device_flags = flags;
		state.error = hr;
		state.capability = current ? VG_CAP_SUPPORTED
					   : (hr == E_NOINTERFACE || hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)
						      ? VG_CAP_UNSUPPORTED
						      : VG_CAP_ERROR);
		publish_locked();
	}
	ReleaseSRWLockExclusive(&mutex);
	release(candidate); // A free/rebuild during factory creation cannot resurrect the retired generation.
}
extern "C" void vg_pub_submitted(void)
{
	capture_slot *slot = nullptr, *retired = nullptr;
	uint64_t generation = 0, next = 0;
	AcquireSRWLockExclusive(&mutex);
	if (current && state.capability == VG_CAP_SUPPORTED) {
		if (current->signaling || state.submitted == UINT64_MAX - 1) {
			retired = fail_locked(HRESULT_FROM_WIN32(ERROR_BUSY));
		} else {
			slot = lease_locked();
			slot->signaling = true;
			generation = state.generation;
			next = state.submitted + 1;
		}
	}
	ReleaseSRWLockExclusive(&mutex);
	release(retired);
	if (!slot)
		return;
	const auto start = vg_pub_qpc();
	const HRESULT hr = slot->backend->signal(next);
	AcquireSRWLockExclusive(&mutex);
	if (current == slot && state.generation == generation) {
		slot->signaling = false;
		if (SUCCEEDED(hr)) {
			state.submitted = next;
			if (!state.first_submitted_qpc)
				state.first_submitted_qpc = start;
			publish_locked();
		} else
			retired = fail_locked(hr);
	}
	ReleaseSRWLockExclusive(&mutex);
	release(retired);
	release(slot);
}
extern "C" void vg_pub_capture_free(void)
{
	AcquireSRWLockExclusive(&mutex);
	auto *retired = current;
	current = nullptr;
	reset_generation_locked();
	state.native_initialized = 0;
	state.hwnd = 0;
	state.map_id = 0;
	state.device_flags = 0;
	publish_locked();
	ReleaseSRWLockExclusive(&mutex);
	release(retired); // No wait for in-flight Signal/GetCompletedValue and no COM Release under SRWLOCK.
}
extern "C" void vg_pub_tick(void)
{
	AcquireSRWLockExclusive(&mutex);
	auto *slot = lease_locked();
	const auto generation = state.generation;
	ReleaseSRWLockExclusive(&mutex);
	const auto completed = slot ? slot->backend->completed() : 0, now = vg_pub_qpc();
	capture_slot *retired = nullptr;
	AcquireSRWLockExclusive(&mutex);
	if (slot && current == slot && generation == state.generation) {
		if (completed == UINT64_MAX)
			retired = fail_locked(DXGI_ERROR_DEVICE_REMOVED);
		else if (completed > state.submitted && !slot->signaling)
			retired = fail_locked(E_UNEXPECTED);
		else if (completed < state.completed)
			retired = fail_locked(E_UNEXPECTED);
		else {
			// Signal may have completed on the GPU before its successful return is committed here.
			const auto committed = completed < state.submitted ? completed : state.submitted;
			if (committed > state.completed) {
				state.completed = committed;
				state.last_completed_qpc = now;
				if (!state.first_completed_qpc)
					state.first_completed_qpc = now;
			}
		}
	}
	state.heartbeat_qpc = now;
	publish_locked();
	ReleaseSRWLockExclusive(&mutex);
	release(retired);
	release(slot);
}
extern "C" void vg_pub_shutdown(void)
{
	AcquireSRWLockExclusive(&mutex);
	running = false;
	auto *retired = current;
	current = nullptr;
	reset_generation_locked();
	state.native_initialized = 0;
	state.hwnd = 0;
	state.map_id = 0;
	state.device_flags = 0;
	publish_locked();
	auto *view = shared;
	shared = nullptr;
	HANDLE opened = mapping;
	mapping = nullptr;
	ReleaseSRWLockExclusive(&mutex);
	release(retired);
	if (view)
		UnmapViewOfFile(view);
	if (opened)
		CloseHandle(opened);
}

#else
extern "C" void vg_pub_start(void) {}
extern "C" void vg_pub_tick(void) {}
extern "C" void vg_pub_shutdown(void) {}
extern "C" void vg_pub_dxgi_installed(void) {}
extern "C" void vg_pub_capture_init(void *, void *, HWND, uint32_t) {}
extern "C" void vg_pub_capture_free(void) {}
extern "C" void vg_pub_submitted(void) {}
#endif
