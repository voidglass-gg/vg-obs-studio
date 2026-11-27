// Deterministic checks of the actual producer implementation. No GPU, OBS
// startup or injection.
#include <dxgi.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <voidglass-publication.h>
#include <windows.h>
static void driver_unlocked();
namespace {
unsigned made = 0, disposed = 0, signals = 0, queries = 0, factories = 0, lock_violations = 0;
HRESULT factory_result = S_OK, signal_result = S_OK;
uint64_t queried = 0;
std::function<void()> on_factory, on_signal, on_query, on_destroy;
static void callback(std::function<void()> &fn)
{
	auto once = std::move(fn);
	fn = nullptr;
	if (once)
		once();
}
struct publication_backend {
	publication_backend() { ++made; }
	~publication_backend()
	{
		driver_unlocked();
		++disposed;
		callback(on_destroy);
	}
	HRESULT signal(uint64_t value)
	{
		driver_unlocked();
		++signals;
		queried = value;
		callback(on_signal);
		return signal_result;
	}
	uint64_t completed()
	{
		driver_unlocked();
		++queries;
		callback(on_query);
		return queried;
	}
};
publication_backend *make_backend(void *, void *, UINT &flags, HRESULT &hr)
{
	driver_unlocked();
	++factories;
	flags = 0;
	hr = factory_result;
	callback(on_factory);
	return SUCCEEDED(hr) ? new (std::nothrow) publication_backend : nullptr;
}
} // namespace
#define VG_PUBLICATION_TEST_BACKEND
#include "voidglass-publication.cpp"
static void driver_unlocked()
{
	if (TryAcquireSRWLockExclusive(&mutex))
		ReleaseSRWLockExclusive(&mutex);
	else
		++lock_violations;
}
static void check(bool ok, const char *message)
{
	if (!ok)
		throw std::runtime_error(message);
}
static vg_pub_payload read()
{
	vg_pub_payload out{};
	check(vg_pub_read(shared, sizeof(vg_pub_shared), &out) == VG_PUB_VALID, "coherent published snapshot");
	return out;
}
static void init()
{
	vg_pub_capture_init(nullptr, nullptr, (HWND)42, 7);
}
int main()
{
	try {
		vg_pub_start();
		init();
		check(state.capability == VG_CAP_SUPPORTED, "supported backend");
		vg_pub_submitted();
		vg_pub_tick();
		check(read().completed == 1, "first GPU completion");
		const auto generation = state.generation;
		vg_pub_capture_free();
		auto invalidated = read();
		check(invalidated.generation > generation && !invalidated.native_initialized &&
			      invalidated.completed == 0 && !current,
		      "immediate invalidation resets all generation measurements");
		factory_result = E_NOINTERFACE;
		init();
		const auto before = signals;
		vg_pub_submitted();
		check(state.capability == VG_CAP_UNSUPPORTED && signals == before && !current,
		      "unsupported API: no Signal, ordinary caller continues");
		factory_result = E_OUTOFMEMORY;
		init();
		vg_pub_submitted();
		check(state.capability == VG_CAP_ERROR && !current, "allocation failure stays optional");
		factory_result = S_OK;
		init();
		signal_result = E_FAIL;
		vg_pub_submitted();
		check(state.capability == VG_CAP_ERROR && !current && state.submitted == 0,
		      "Signal failure retires telemetry");
		signal_result = S_OK;
		init();
		on_signal = [] {
			vg_pub_tick();
			check(state.capability == VG_CAP_SUPPORTED && state.completed == 0,
			      "early completion cannot precede committed Signal count");
		};
		vg_pub_submitted();
		vg_pub_tick();
		check(read().completed == 1, "early completion becomes visible after commit");
		queried = UINT64_MAX;
		vg_pub_tick();
		check(state.capability == VG_CAP_ERROR && state.error == DXGI_ERROR_DEVICE_REMOVED && !current,
		      "device removed sentinel");
		init();
		queried = 5;
		vg_pub_tick();
		check(state.capability == VG_CAP_ERROR && state.error == E_UNEXPECTED && !current,
		      "impossible completion is not mislabelled device loss");
		init();
		vg_pub_submitted();
		vg_pub_tick();
		queried = 0;
		vg_pub_tick();
		check(state.capability == VG_CAP_ERROR && !current, "regressing completion rejected");
		init();
		const auto retired_generation = state.generation;
		on_signal = [] {
			auto count = disposed;
			vg_pub_capture_free();
			check(disposed == count, "Signal lease retains backend across free");
			init();
		};
		vg_pub_submitted();
		check(state.generation > retired_generation && state.submitted == 0 && state.completed == 0 && current,
		      "late Signal cannot revive old generation");
		on_query = [] {
			auto count = disposed;
			vg_pub_capture_free();
			check(disposed == count, "observer lease retains backend across free");
			init();
		};
		queried = 1;
		vg_pub_tick();
		check(state.capability == VG_CAP_SUPPORTED && state.completed == 0,
		      "late observation discarded after rebuild");
		const auto prior_signals = signals;
		on_signal = [] {
			vg_pub_submitted();
		};
		vg_pub_submitted();
		check(signals == prior_signals + 1 && state.capability == VG_CAP_ERROR && !current,
		      "overlap/reentrant Signal rejected without context serialization");
		on_factory = [] {
			vg_pub_capture_free();
		};
		init();
		check(!current && !state.native_initialized, "free during factory cannot resurrect a generation");
		init();
		on_destroy = [] {
			vg_pub_capture_free();
		};
		vg_pub_capture_free();
		check(!current, "final COM-like release may reenter free without a lock");
		init();
		on_signal = [] {
			vg_pub_shutdown();
		};
		vg_pub_submitted();
		check(!running && !current && !shared, "shutdown during Signal cannot publish late results");
		vg_pub_start();
		init();
		HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr),
		       leave = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		check(entered && leave, "concurrency control events");
		bool blocked_timeout = false;
		on_signal = [&] {
			SetEvent(entered);
			blocked_timeout = WaitForSingleObject(leave, 1000) != WAIT_OBJECT_0;
		};
		std::thread producer([] { vg_pub_submitted(); });
		check(WaitForSingleObject(entered, 1000) == WAIT_OBJECT_0, "Signal reached controlled pause");
		vg_pub_capture_free();
		init();
		SetEvent(leave);
		producer.join();
		CloseHandle(entered);
		CloseHandle(leave);
		check(!blocked_timeout && state.capability == VG_CAP_SUPPORTED && state.submitted == 0,
		      "free/rebuild progresses while old Signal is in flight on another "
		      "thread");
		vg_pub_shutdown();
		const auto prior_factories = factories;
		wchar_t name[128];
		vg_pub_name(name, GetCurrentProcessId(), vg_pub_creation(GetCurrentProcess()));
		HANDLE collision = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 8, name);
		check(collision != nullptr, "mapping collision control");
		vg_pub_start();
		init();
		check(!shared && factories == prior_factories && !current, "missing channel never calls driver");
		vg_pub_shutdown();
		CloseHandle(collision);
		check(lock_violations == 0 && made == disposed,
		      "all backend calls/releases outside SRWLOCK; no leaked leases");
		std::cout << "PASS publication safety; backends=" << made << " released=" << disposed
			  << " lock_violations=" << lock_violations << std::endl;
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "FAIL " << e.what() << std::endl;
		return 1;
	}
}
