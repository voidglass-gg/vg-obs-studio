#include "../owned-close-queue.hpp"
#include "../border-access.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
static unsigned checks = 0;
static void Check(bool value, const char *message)
{
	++checks;
	if (!value)
		throw std::runtime_error(message);
}
struct State {
	std::mutex mutex;
	std::condition_variable changed;
	bool release = false;
	unsigned blocked_generation = 1;
	std::atomic<bool> init_ok{true}, mta_live{false}, affinity_error{false};
	std::atomic<unsigned> initialized{0}, uninitialized{0}, made{0}, released{0}, entered{0}, errors{0};
	std::thread::id worker;
	void Release()
	{
		{ std::lock_guard lock(mutex); release = true; }
		changed.notify_all();
	}
	bool WaitEntered()
	{
		std::unique_lock lock(mutex);
		return changed.wait_for(lock, 2s, [&] { return entered.load() > 0; });
	}
};
struct Resource {
	State *state;
	unsigned generation;
	Resource(State &owner, unsigned gen) : state(&owner), generation(gen) { ++state->made; }
	~Resource()
	{
		if (!state->mta_live || state->worker != std::this_thread::get_id())
			state->affinity_error = true;
		++state->released;
		state->changed.notify_all();
	}
};
struct Payload {
	std::shared_ptr<int> session_item;
	std::unique_ptr<Resource> session, pool, partial_reference;
	bool close_error = false;
};
struct Policy {
	State *state;
	bool Initialize() noexcept
	{
		++state->initialized;
		if (!state->init_ok)
			return false;
		state->worker = std::this_thread::get_id();
		state->mta_live = true;
		return true;
	}
	void Close(Payload &payload) noexcept
	{
		if (!state->mta_live || state->worker != std::this_thread::get_id())
			state->affinity_error = true;
		++state->entered;
		state->changed.notify_all();
		if (payload.session && payload.session->generation == state->blocked_generation) {
			std::unique_lock lock(state->mutex);
			state->changed.wait(lock, [&] { return state->release; });
		}
		// Match the production catch-and-release contract for a failed OS Close.
		if (payload.close_error)
			++state->errors;
	}
	void Uninitialize() noexcept
	{
		if (state->made != state->released)
			state->affinity_error = true;
		state->mta_live = false;
		++state->uninitialized;
	}
};
using Queue = wgc::OwnedCloseQueue<Payload, Policy, 32>;
struct Fixture {
	State state;
	Queue queue{Policy{&state}};
	struct Capture { Queue::Lease lease; Payload payload; };
	std::vector<Capture> captures;
	Fixture() { captures.reserve(80); }
	bool Create(unsigned generation, bool partial = false, bool close_error = false)
	{
		auto lease = queue.Reserve();
		if (!lease)
			return false; // Refusal precedes ALL resource construction.
		Payload payload;
		if (partial)
			payload.partial_reference = std::make_unique<Resource>(state, generation);
		else {
			payload.session = std::make_unique<Resource>(state, generation);
			payload.pool = std::make_unique<Resource>(state, generation);
		}
		payload.close_error = close_error;
		captures.push_back({std::move(lease), std::move(payload)});
		return true;
	}
	void Retire(size_t index)
	{
		auto &capture = captures.at(index);
		if (capture.lease)
			queue.Retire(capture.lease, [&](Payload &owned) noexcept { owned = std::move(capture.payload); });
	}
	void RetireAll() { for (size_t i = 0; i < captures.size(); ++i) Retire(i); }
	~Fixture()
	{
		state.Release(); RetireAll();
		queue.StopAndJoin(2s, [] {});
	}
};

static void BlockedCloseKeepsProducerAndControlResponsive()
{
	Fixture f;
	Check(!f.queue.Reserve(), "No admission before the worker MTA is initialized");
	Check(f.queue.Start(), "Worker MTA start confirmed");
	Check(f.Create(1), "First generation admitted");
	const auto first_ticket = f.captures[0].lease.ticket;
	f.Retire(0);
	Check(f.state.WaitEntered(), "Fake OS Close reached its controlled blocking point");
	auto graphics = std::async(std::launch::async, [&] {
		for (unsigned gen = 2; gen <= 32; ++gen)
			if (!f.Create(gen)) return false;
		return true;
	});
	auto control = std::async(std::launch::async, [&] {
		for (unsigned poll = 0; poll < 1000; ++poll)
			(void)f.queue.Snapshot();
		return true;
	});
	const bool graphics_ready = graphics.wait_for(2s) == std::future_status::ready;
	const bool control_ready = control.wait_for(2s) == std::future_status::ready;
	if (!graphics_ready || !control_ready) f.state.Release();
	Check(graphics.get() && graphics_ready, "Producer remains responsive while OS Close is blocked");
	Check(control.get() && control_ready, "Control polling remains responsive while OS Close is blocked");
	const auto counts = f.queue.Snapshot();
	Check(counts.live == 31 && counts.closing == 1 && counts.pending == 0, "Capacity counts live plus in-progress generations");
	const auto made = f.state.made.load();
	for (unsigned attempt = 0; attempt < 8; ++attempt)
		Check(!f.Create(100), "Burst overload refuses new generations without stale/live admission");
	Check(f.state.made == made, "No resource is constructed after capacity refusal");
	Check(f.state.released == 0, "No final reference is released on producer/control while Close is blocked");
	f.state.Release();
	{
		std::unique_lock lock(f.state.mutex);
		Check(f.state.changed.wait_for(lock, 2s, [&] { return f.state.released.load() >= 2; }), "Released Close returns its generation slot");
	}
	// Resource releases precede slot publication, so wait for actual admission.
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while (!f.Create(100) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	Check(f.captures.size() == 33 && f.captures.back().lease.ticket > first_ticket, "A fresh generation uses a new reservation, never retired proof");
	f.RetireAll();
	Check(f.queue.StopAndJoin(2s, [] {}), "Unblocked queue drains and joins within test bound");
	Check(!f.state.affinity_error && f.state.made == f.state.released && f.state.uninitialized == 1,
		"All final releases occur on initialized worker before MTA uninitialization");
}

static void WarningThresholdCannotAuthorizeUnload()
{
	Fixture f;
	Check(f.queue.Start() && f.Create(1), "Blocked-drain fixture starts");
	f.RetireAll(); Check(f.state.WaitEntered(), "Blocked drain entered");
	std::atomic<bool> warning{false}, returned{false};
	auto stopping = std::async(std::launch::async, [&] {
		const bool within = f.queue.StopAndJoin(30ms, [&] { warning = true; });
		returned = true; return within;
	});
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while (!warning && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	const bool warned = warning;
	const bool stayed_owned = !returned && stopping.wait_for(0ms) != std::future_status::ready;
	const bool refused_restart = !f.queue.Start() && !f.queue.Reserve();
	f.state.Release();
	const bool within = stopping.get();
	Check(warned && stayed_owned, "Threshold callback does not return, unload, detach or report joined while Close is blocked");
	Check(refused_restart, "Stopping worker refuses creation/restart until the old worker is joined");
	Check(!within && returned && f.state.uninitialized == 1 && !f.state.affinity_error,
		"After release the owned join completes, with warning-threshold-exceeded reported honestly");
	Check(f.queue.Start(), "Video reset can start a new worker after the old join");
	Check(f.Create(2), "Rebuild admits a fresh generation after video reset");
	f.RetireAll(); Check(f.queue.StopAndJoin(2s, [] {}), "Second worker drains and joins");
	Check(f.state.initialized == 2 && f.state.uninitialized == 2 && f.state.made == f.state.released,
		"Repeated lifecycle has no stranded worker or reference");
	f.RetireAll(); // Residual source destruction after video stop must be inert.
	Check(!f.queue.Reserve(), "Residual cleanup never silently restarts a stopped worker");
}

static void FailedInitializationAndPartialCreation()
{
	Fixture f; f.state.init_ok = false;
	Check(!f.queue.Start() && !f.Create(1) && f.state.made == 0, "MTA initialization failure refuses admission before any resource");
	f.state.init_ok = true; f.state.blocked_generation = 999;
	Check(f.queue.Start(), "Failed initialization was joined and permits a clean retry");
	Check(f.Create(3, true), "A partially created generation retains its reserved slot");
	Check(f.Create(4, false, true), "OS Close-error fixture admitted");
	f.RetireAll(); Check(f.queue.StopAndJoin(2s, [] {}), "Partial creation and Close error both drain");
	Check(f.state.released == 3 && f.state.errors == 1 && !f.state.affinity_error,
		"Partial references and Close-error final releases stay on the worker, with no leaked reservation");
}

static void CaptureItemOutlivesWorkerClose()
{
	State state;
	wgc::OwnedCloseQueue<Payload, Policy, 1, std::shared_ptr<int>> queue{Policy{&state}};
	const auto owner = std::this_thread::get_id();
	std::atomic<unsigned> releases{0};
	std::atomic<bool> wrong_thread{false};
	auto new_item = [&] {
		return std::shared_ptr<int>(new int{}, [&](int *item) {
			delete item;
			if (std::this_thread::get_id() != owner)
				wrong_thread = true;
			(void)queue.Snapshot(); // Final release must run outside the queue mutex.
			++releases;
		});
	};
	Check(queue.Start(), "Worker starts for item ownership test");
	auto first = queue.Reserve();
	auto &first_item = queue.ResetOwnerReference(first);
	first_item = new_item();
	auto session = std::make_unique<Resource>(state, 1);
	queue.Retire(first, [&](Payload &p) noexcept {
		p.session = std::move(session);
		p.session_item = first_item;
	});
	const bool close_started = state.WaitEntered();
	queue.CollectOwnerReferences();
	const bool retained_while_closing = releases == 0;
	state.Release();
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	auto second = queue.Reserve();
	while (!second && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
		second = queue.Reserve();
	}
	const bool reused_before_tick = static_cast<bool>(second);
	const bool retained_after_close = releases == 0;
	if (second) {
		auto &second_item = queue.ResetOwnerReference(second);
		second_item = new_item();
		queue.CollectOwnerReferences();
		queue.Retire(second, [](Payload &) noexcept {}); // Partial start, no session.
	}
	const bool released_on_reuse = releases == 1;
	const bool joined = queue.StopAndJoin(2s, [] {});
	const bool held_at_join = releases == 1;
	queue.CollectOwnerReferences();
	queue.CollectOwnerReferences();
	Check(close_started && retained_while_closing && retained_after_close,
	      "Session Close cannot perform the final release of its dispatcher-owned item");
	Check(reused_before_tick && released_on_reuse, "A reused slot releases only its completed predecessor");
	Check(joined && held_at_join && releases == 2 && !wrong_thread,
	      "Tick or shutdown releases each item exactly once on its creating dispatcher");
}

struct ConsentReply {
	enum class Status { Started, Completed, Canceled, Error } status = Status::Started;
	enum class Access { Denied, Allowed } access = Access::Denied;
	unsigned reads = 0;
	bool fail_results = false;
};
struct ConsentOperation {
	ConsentReply *reply;
	ConsentOperation(ConsentReply *value) : reply(value) {}
	explicit operator bool() const { return reply != nullptr; }
	auto Status() const { return reply->status; }
	auto GetResults() const
	{
		++reply->reads;
		if (reply->fail_results) throw std::runtime_error("results");
		return reply->access;
	}
};
struct BorderSession {
	mutable unsigned hides = 0;
	bool fail = false;
	void IsBorderRequired(bool required) const
	{
		if (fail || required) throw std::runtime_error("border");
		++hides;
	}
};

static void OptionalBorderConsent()
{
	wgc::BorderAccess<ConsentOperation> consent;
	BorderSession first, replacement;
	ConsentReply old, current;
	consent.Begin([&] { return &old; });
	consent.Poll(first);
	Check(!first.hides && !old.reads, "pending consent leaves the started session bordered");
	auto retired = consent.Retire();
	old.status = ConsentReply::Status::Completed;
	old.access = ConsentReply::Access::Allowed;
	consent.Poll(first);
	consent.Begin([&] { return &current; });
	consent.Poll(replacement);
	Check(retired && !old.reads && !first.hides && !replacement.hides, "late grant touches neither session");
	current.status = ConsentReply::Status::Completed;
	consent.Poll(replacement);
	Check(current.reads == 1 && !replacement.hides, "denial keeps capture bordered");
	for (auto status : {ConsentReply::Status::Error, ConsentReply::Status::Canceled}) {
		current.status = status;
		consent.Begin([&] { return &current; });
		consent.Poll(replacement);
	}
	consent.Begin([]() -> ConsentOperation { throw std::runtime_error("unavailable"); });
	consent.Poll(replacement);
	Check(current.reads == 1 && !replacement.hides, "unavailable, error and canceled consent are optional");
	current.status = ConsentReply::Status::Completed;
	current.access = ConsentReply::Access::Allowed;
	current.fail_results = true;
	consent.Begin([&] { return &current; });
	consent.Poll(replacement);
	Check(!replacement.hides, "failed result leaves border enabled");
	current.fail_results = false;
	replacement.fail = true;
	consent.Begin([&] { return &current; });
	consent.Poll(replacement);
	replacement.fail = false;
	consent.Poll(replacement);
	Check(!replacement.hides, "failed property update is contained");
	consent.Begin([&] { return &current; });
	consent.Poll(replacement);
	consent.Poll(replacement);
	Check(replacement.hides == 1 && !first.hides, "allowed live session hides border once");
}

int main()
{
	try {
		OptionalBorderConsent();
		BlockedCloseKeepsProducerAndControlResponsive();
		WarningThresholdCannotAuthorizeUnload();
		FailedInitializationAndPartialCreation();
		CaptureItemOutlivesWorkerClose();
		std::cout << "Owned WGC Close: " << checks << " CPU checks PASS; no OBS, WinRT, GPU, window or capture\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL: " << error.what() << '\n'; return 1;
	}
}
