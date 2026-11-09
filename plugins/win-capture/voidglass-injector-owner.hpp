#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <tuple>

namespace vg_injector {
using Handle = void *;
struct Key {
	uint32_t pid;
	uint64_t creation;
	bool operator<(const Key &b) const { return std::tie(pid, creation) < std::tie(b.pid, b.creation); }
	bool operator==(const Key &b) const { return pid == b.pid && creation == b.creation; }
};
struct Poll {
	bool exited = false, code_known = false;
	uint32_t code = 0, error = 0;
};
struct Child {
	Handle handle = nullptr;
	uint32_t pid = 0, error = 0;
};
enum class State { idle, launching, running, cancelling, exited, blocked, failed };
struct Status {
	uint64_t id = 0;
	Key key{};
	State state = State::idle;
	uint32_t helper_pid = 0, exit_code = 0, error = 0;
	bool compatibility = false, code_known = false, cancelled = false;
};
struct Ops {
	virtual ~Ops() = default;
	virtual Handle prepare(uint64_t) = 0; // reserve wait AND deferred cleanup before launch
	virtual void bind(Handle, Handle) = 0;
	virtual void arm(Handle, uint32_t) = 0; // 0=infinite; never invokes callback inline
	virtual Poll poll(Handle) = 0;
	virtual uint32_t terminate(Handle) = 0;
	virtual void park(Handle) = 0;
	virtual void retire(Handle) = 0; // disarm, drain callbacks OFF caller thread, close owned child
	virtual void report(const Status &) = 0;
};
// Same code in production and deterministic tests. No source pointers, clock polling or threads.
class Owner {
	struct Entry {
		Status s;
		unsigned clients = 0;
		Handle child = nullptr, watch = nullptr;
		bool issued = false, launching = false, retiring = false, kill_sent = false;
	};
	Ops &ops;
	std::mutex mutex;
	uint64_t serial = 0;
	std::map<uint64_t, std::unique_ptr<Entry>> entries;
	std::map<uint32_t, Status> issued; // latest process identity per PID; metadata only
	void remember(const Entry &e)
	{
		auto i = issued.find(e.s.key.pid);
		if (i == issued.end() || i->second.key == e.s.key)
			issued[e.s.key.pid] = e.s;
	}
	void retire(Entry &e)
	{
		if (!e.retiring) {
			e.retiring = true;
			ops.retire(e.watch);
		}
	}
	void finish(Entry &e, const Poll &p)
	{
		e.s.state = State::exited;
		e.s.exit_code = p.code;
		e.s.code_known = p.code_known;
		e.s.error = p.error;
		remember(e);
		retire(e);
	}
	void cancel(Entry &e)
	{
		e.s.cancelled = true;
		if (!e.child || e.retiring || !e.s.compatibility || e.kill_sent)
			return;
		auto p = ops.poll(e.child);
		if (p.exited) {
			finish(e, p);
			return;
		}
		if (p.error) {
			e.s.state = State::blocked;
			e.s.error = p.error;
			ops.park(e.watch);
			ops.report(e.s);
			return;
		}
		e.kill_sent = true;
		e.s.state = State::cancelling;
		e.s.error = ops.terminate(e.child); // only the exact owned helper HANDLE
		p = ops.poll(e.child);
		if (p.exited) {
			finish(e, p);
			return;
		}
		ops.arm(e.watch, drain_ms);
		ops.report(e.s);
	}

public:
	// Compatibility helper sleeps 8*500 ms; allow 11 s extra startup/API slack.
	// This is a watchdog, not proof that every device finishes in this interval.
	static constexpr uint32_t watchdog_ms = 15000, drain_ms = 5000;
	explicit Owner(Ops &o) : ops(o) {}
	uint64_t acquire(Key key)
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (auto &row : entries)
			if (row.second->s.key == key) {
				++row.second->clients;
				return row.first;
			}
		auto e = std::make_unique<Entry>();
		e->s.id = ++serial;
		e->s.key = key;
		e->clients = 1;
		auto prior = issued.find(key.pid);
		if (prior != issued.end() && prior->second.key == key && prior->second.state != State::idle) {
			e->issued = true;
			e->s = prior->second;
			e->s.id = serial;
		}
		if (prior == issued.end() || !(prior->second.key == key))
			issued[key.pid] = e->s;
		auto id = e->s.id;
		entries[id] = std::move(e);
		return id;
	}
	Status status(uint64_t id)
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto i = entries.find(id);
		return i == entries.end() ? Status{} : i->second->s;
	}
	bool can_start(uint64_t id)
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto i = entries.find(id);
		return i != entries.end() && !i->second->issued && !i->second->s.cancelled;
	}
	template<class Launch> bool start(uint64_t id, bool compatibility, Launch launch)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			auto i = entries.find(id);
			if (i == entries.end() || i->second->issued || i->second->s.cancelled)
				return false;
			auto &e = *i->second;
			e.issued = true;
			e.launching = true;
			e.s.compatibility = compatibility;
			e.s.state = State::launching;
			remember(e);
			e.watch = ops.prepare(id);
			if (!e.watch) {
				e.launching = false;
				e.s.state = State::failed;
				e.s.error = 8;
				remember(e);
				ops.report(e.s);
				return false;
			}
		}
		// Creation/security checks need not hold the shared exclusion mutex. Reservation remains.
		Child child = launch();
		std::lock_guard<std::mutex> lock(mutex);
		auto &e = *entries.at(id);
		e.launching = false;
		e.child = child.handle;
		e.s.helper_pid = child.pid;
		e.s.error = child.error;
		ops.bind(e.watch, e.child);
		if (!e.child) {
			e.s.state = State::failed;
			remember(e);
			retire(e);
			return false;
		}
		e.s.state = State::running;
		ops.arm(e.watch, watchdog_ms);
		if (e.s.cancelled)
			cancel(e);
		return true;
	}
	// Synchronous upstream direct injection has no child. Still respects helper exclusion/latch.
	bool claim_direct(uint64_t id)
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto i = entries.find(id);
		if (i == entries.end() || i->second->issued || i->second->s.cancelled)
			return false;
		auto &e = *i->second;
		e.issued = true;
		e.s.state = State::exited;
		remember(e);
		return true;
	}
	void release(uint64_t id)
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto i = entries.find(id);
		if (i == entries.end())
			return;
		auto &e = *i->second;
		if (!e.clients || --e.clients)
			return;
		cancel(e);
		if (!e.child && !e.launching && !e.retiring)
			entries.erase(i);
	}
	void wake(uint64_t id, bool timeout)
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto i = entries.find(id);
		if (i == entries.end())
			return;
		auto &e = *i->second;
		if (!e.child || e.retiring)
			return;
		auto p = ops.poll(e.child);
		if (p.exited) {
			finish(e, p);
			return;
		}
		if (!timeout || p.error) {
			e.s.state = State::blocked;
			e.s.error = p.error;
			ops.park(e.watch);
			ops.report(e.s);
			return;
		}
		if (e.s.compatibility && !e.kill_sent) {
			cancel(e);
			return;
		}
		e.s.state = State::blocked;
		ops.arm(e.watch, 0);
		ops.report(e.s);
	}
	void reaped(uint64_t id)
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto i = entries.find(id);
		if (i == entries.end())
			return;
		auto &e = *i->second;
		e.child = nullptr;
		e.watch = nullptr;
		e.retiring = false;
		if (!e.clients)
			entries.erase(i);
	}
	void shutdown()
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (auto &row : entries)
			cancel(*row.second);
	}
	size_t pending()
	{
		std::lock_guard<std::mutex> lock(mutex);
		size_t n = 0;
		for (auto &row : entries)
			if (row.second->child || row.second->launching || row.second->retiring)
				++n;
		return n;
	}
};
} // namespace vg_injector
