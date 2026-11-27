#include <windows.h>
#include <cstdio>
#include <new>
#include "voidglass-injector.h"
#include "voidglass-injector-owner.hpp"
using namespace vg_injector;
namespace {
Owner &owner();
struct Watch {
	PTP_WAIT wait = nullptr;
	PTP_WORK cleanup = nullptr;
	HANDLE child = nullptr;
	uint64_t id;
};
void CALLBACK wait_callback(PTP_CALLBACK_INSTANCE, void *p, PTP_WAIT, TP_WAIT_RESULT reason)
{
	auto *w = static_cast<Watch *>(p);
	owner().wake(w->id, reason == WAIT_TIMEOUT);
}
void CALLBACK cleanup_callback(PTP_CALLBACK_INSTANCE, void *p, PTP_WORK)
{
	auto *w = static_cast<Watch *>(p);
	// Never on the graphics thread and never waiting for the helper itself.
	WaitForThreadpoolWaitCallbacks(w->wait, TRUE);
	CloseThreadpoolWait(w->wait);
	if (w->child)
		CloseHandle(w->child); // child is observed exited, or creation failed
	auto id = w->id;
	CloseThreadpoolWork(w->cleanup);
	delete w;
	owner().reaped(id);
}
struct WindowsOps final : Ops {
	Handle prepare(uint64_t id) override
	{
		auto *w = new (std::nothrow) Watch;
		if (!w)
			return nullptr;
		w->id = id;
		w->wait = CreateThreadpoolWait(wait_callback, w, nullptr);
		if (w->wait)
			w->cleanup = CreateThreadpoolWork(cleanup_callback, w, nullptr);
		if (!w->wait || !w->cleanup) {
			if (w->wait)
				CloseThreadpoolWait(w->wait);
			delete w;
			return nullptr;
		}
		return w;
	}
	void bind(Handle w, Handle child) override { static_cast<Watch *>(w)->child = child; }
	std::chrono::steady_clock::time_point now() override { return std::chrono::steady_clock::now(); }
	void arm(Handle handle, uint32_t milliseconds) override
	{
		auto *w = static_cast<Watch *>(handle);
		FILETIME due{};
		if (milliseconds) {
			ULARGE_INTEGER t{};
			t.QuadPart = uint64_t(-int64_t(milliseconds) * 10000);
			due.dwLowDateTime = t.LowPart;
			due.dwHighDateTime = t.HighPart;
		}
		SetThreadpoolWait(w->wait, w->child, milliseconds ? &due : nullptr);
	}
	Poll poll(Handle child) override
	{
		DWORD wait = WaitForSingleObject(child, 0);
		Poll p;
		if (wait == WAIT_OBJECT_0) {
			p.exited = true;
			DWORD code = 0;
			p.code_known = !!GetExitCodeProcess(child, &code);
			p.code = code;
			if (!p.code_known)
				p.error = GetLastError();
		} else if (wait == WAIT_FAILED)
			p.error = GetLastError();
		return p;
	}
	uint32_t terminate(Handle child) override
	{
		return TerminateProcess(child, ERROR_CANCELLED) ? 0 : GetLastError();
	}
	void park(Handle handle) override { SetThreadpoolWait(static_cast<Watch *>(handle)->wait, nullptr, nullptr); }
	void retire(Handle handle) override
	{
		auto *w = static_cast<Watch *>(handle);
		SetThreadpoolWait(w->wait, nullptr, nullptr);
		// Work was preallocated before CreateProcess; no allocation failure can lose the child.
		SubmitThreadpoolWork(w->cleanup);
	}
	void report(const Status &s) override
	{
		char text[256];
		std::snprintf(
			text, sizeof(text),
			"VoidGlass injector: target=%lu creation=%llu attempt=%llu helper=%lu state=%u compatibility=%u error=%lu cancelled=%u\n",
			(unsigned long)s.key.pid, (unsigned long long)s.key.creation, (unsigned long long)s.id,
			(unsigned long)s.helper_pid, (unsigned)s.state, (unsigned)s.compatibility,
			(unsigned long)s.error, (unsigned)s.cancelled);
		OutputDebugStringA(text); // independent of obs_shutdown and user log-handler lifetime
	}
};
Owner &owner()
{ // OS waits can outlive a source and obs_shutdown. No source/OBS pointers in callbacks.
	static auto *ops = new WindowsOps;
	static auto *instance = new Owner(*ops);
	return *instance;
}
} // namespace
extern "C" {
uint64_t vg_injector_acquire(uint32_t pid, uint64_t creation)
{
	try {
		return owner().acquire({pid, creation});
	} catch (...) {
		OutputDebugStringA("VoidGlass injector: could not allocate target ownership; refusing injection\n");
		return 0;
	}
}
bool vg_injector_can_start(uint64_t lease)
{
	return owner().can_start(lease);
}
bool vg_injector_start(uint64_t lease, bool compatibility, vg_injector_launch launch, void *context)
{
	return owner().start(lease, compatibility, [&] {
		auto c = launch(context);
		return Child{c.handle, c.pid, c.error};
	});
}
bool vg_injector_claim_direct(uint64_t lease)
{
	return owner().claim_direct(lease);
}
void vg_injector_release(uint64_t lease)
{
	if (lease)
		owner().release(lease);
}
vg_injector_status vg_injector_get_status(uint64_t lease)
{
	auto s = owner().status(lease);
	return {s.id,        s.key.creation, s.key.pid,       s.helper_pid, (uint32_t)s.state,
		s.exit_code, s.error,        s.compatibility, s.code_known, s.cancelled};
}
bool vg_injector_shutdown(void)
{
	owner().shutdown();
	return owner().drain(Owner::drain_ms);
}
}
