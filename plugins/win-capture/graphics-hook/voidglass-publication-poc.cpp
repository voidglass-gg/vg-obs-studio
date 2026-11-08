#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include "graphics-hook.h"
#include <voidglass-publication-poc.h>
using Microsoft::WRL::ComPtr;
namespace {
SRWLOCK mutex = SRWLOCK_INIT;
vg_pub_payload state{};
ComPtr<ID3D11Fence> fence;
ComPtr<ID3D11DeviceContext4> immediate;
HANDLE mapping = nullptr, gate = nullptr;
vg_pub_shared *shared = nullptr;
bool allowed = true;
void publish_locked()
{
	if (!shared)
		return;
	InterlockedIncrement64(&shared->sequence);
	MemoryBarrier();
	memcpy(&shared->data, &state, sizeof(state));
	MemoryBarrier();
	InterlockedIncrement64(&shared->sequence);
}
} // namespace
extern "C" void vg_pub_start(void)
{
	AcquireSRWLockExclusive(&mutex);
	state.magic = VG_PUB_MAGIC;
	state.size = sizeof(state);
	state.major = VG_PUB_MAJOR;
	state.minor = VG_PUB_MINOR;
	state.pid = GetCurrentProcessId();
	state.creation = vg_pub_creation(GetCurrentProcess());
	state.instance = vg_pub_qpc() ^ ((uint64_t)state.pid << 32);
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	state.frequency = freq.QuadPart;
	wchar_t name[128];
	vg_pub_name(name, state.pid, state.creation);
	mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(vg_pub_shared), name);
	if (mapping && GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(mapping);
		mapping = nullptr;
	}
	if (mapping)
		shared = (vg_pub_shared *)MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(vg_pub_shared));
	wchar_t gate_name[256];
	DWORD n = GetEnvironmentVariableW(L"VG_POC_PUBLICATION_GATE", gate_name, 256);
	if (n && n < 256) {
		gate = OpenEventW(SYNCHRONIZE, FALSE, gate_name);
		allowed = false;
		if (!gate) {
			state.capability = VG_CAP_ERROR;
			state.error = HRESULT_FROM_WIN32(GetLastError());
		}
	}
	publish_locked();
	ReleaseSRWLockExclusive(&mutex);
}
extern "C" void vg_pub_dxgi_installed(void)
{
	AcquireSRWLockExclusive(&mutex);
	state.dxgi_installed = 1;
	ReleaseSRWLockExclusive(&mutex);
}
extern "C" void vg_pub_capture_init(void *dev, void *ctx, HWND hwnd, uint32_t map_id)
{
	auto *d = (ID3D11Device *)dev;
	auto *c = (ID3D11DeviceContext *)ctx;
	ComPtr<ID3D11Device5> d5;
	ComPtr<ID3D11DeviceContext4> c4;
	ComPtr<ID3D11Fence> next;
	HRESULT hr = E_NOINTERFACE;
	UINT flags = d->GetCreationFlags();
	if (!(flags & D3D11_CREATE_DEVICE_SINGLETHREADED)) {
		hr = d->QueryInterface(IID_PPV_ARGS(&d5));
		if (SUCCEEDED(hr))
			hr = c->QueryInterface(IID_PPV_ARGS(&c4));
		if (SUCCEEDED(hr))
			hr = d5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&next));
	}
	AcquireSRWLockExclusive(&mutex);
	++state.generation;
	state.api = 11;
	state.transport = 1;
	state.hwnd = (uintptr_t)hwnd;
	state.map_id = map_id;
	state.native_initialized = 1;
	state.device_flags = flags;
	state.error = hr;
	state.submitted = state.completed = 0;
	state.first_submitted_qpc = state.first_completed_qpc = state.last_completed_qpc = 0;
	state.signal_ticks = state.signal_max_ticks = 0;
	state.initialized_qpc = vg_pub_qpc();
	state.capability = SUCCEEDED(hr) ? VG_CAP_SUPPORTED : VG_CAP_UNSUPPORTED;
	fence = next;
	immediate = c4;
	const auto generation = state.generation;
	const auto capability = state.capability;
	ReleaseSRWLockExclusive(&mutex);
	hlog("[VG_PUB] initialized generation=%llu submitted=0 completed=0 capability=%u hr=0x%08lx",
	     (unsigned long long)generation, capability, (unsigned long)hr);
}
extern "C" bool vg_pub_copy_allowed(void)
{
	AcquireSRWLockShared(&mutex);
	bool value = allowed;
	ReleaseSRWLockShared(&mutex);
	return value;
}
extern "C" void vg_pub_submitted(void)
{
	AcquireSRWLockExclusive(&mutex);
	if (state.capability == VG_CAP_SUPPORTED && fence && immediate) {
		uint64_t next = state.submitted + 1, start = vg_pub_qpc();
		HRESULT hr = immediate->Signal(fence.Get(), next);
		uint64_t cost = vg_pub_qpc() - start;
		if (SUCCEEDED(hr)) {
			state.submitted = next;
			state.signal_ticks += cost;
			if (cost > state.signal_max_ticks)
				state.signal_max_ticks = cost;
			if (!state.first_submitted_qpc)
				state.first_submitted_qpc = start;
		} else {
			state.capability = VG_CAP_ERROR;
			state.error = hr;
		}
	}
	ReleaseSRWLockExclusive(&mutex);
}
extern "C" void vg_pub_capture_free(void)
{
	AcquireSRWLockExclusive(&mutex);
	++state.generation;
	state.native_initialized = 0;
	state.submitted = state.completed = 0;
	state.first_submitted_qpc = state.first_completed_qpc = state.last_completed_qpc = 0;
	fence.Reset();
	immediate.Reset();
	ReleaseSRWLockExclusive(&mutex);
}
extern "C" void vg_pub_tick(void)
{
	ComPtr<ID3D11Fence> observed;
	uint64_t generation = 0;
	AcquireSRWLockExclusive(&mutex);
	if (gate && !allowed && WaitForSingleObject(gate, 0) == WAIT_OBJECT_0)
		allowed = true;
	if (state.native_initialized && state.capability == VG_CAP_SUPPORTED) {
		observed = fence;
		generation = state.generation;
	}
	ReleaseSRWLockExclusive(&mutex);
	uint64_t completed = observed ? observed->GetCompletedValue() : 0, now = vg_pub_qpc();
	AcquireSRWLockExclusive(&mutex);
	if (observed && generation == state.generation) {
		if (completed > state.submitted) {
			state.capability = VG_CAP_ERROR;
			state.error = DXGI_ERROR_DEVICE_REMOVED;
		} else if (completed > state.completed) {
			state.completed = completed;
			state.last_completed_qpc = now;
			if (!state.first_completed_qpc)
				state.first_completed_qpc = now;
		}
	}
	state.heartbeat_qpc = now;
	publish_locked();
	ReleaseSRWLockExclusive(&mutex);
}
extern "C" void vg_pub_shutdown(void)
{
	vg_pub_capture_free();
	vg_pub_tick();
	AcquireSRWLockExclusive(&mutex);
	if (shared) {
		UnmapViewOfFile(shared);
		shared = nullptr;
	}
	if (mapping) {
		CloseHandle(mapping);
		mapping = nullptr;
	}
	if (gate) {
		CloseHandle(gate);
		gate = nullptr;
	}
	ReleaseSRWLockExclusive(&mutex);
}
