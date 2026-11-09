#include "../voidglass-injector-owner.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <future>
#include <thread>
using namespace vg_injector;
static unsigned checks = 0;
static void check(bool v, const char *why)
{
	++checks;
	if (!v)
		throw std::runtime_error(why);
}
struct Fake : Ops {
	struct Process {
		uint32_t pid, code = 0, error = 0;
		bool exited = false;
		unsigned closed = 0, kills = 0;
	};
	struct Watch {
		uint64_t id;
		Process *p = nullptr;
		uint32_t due = 0;
		bool retiring = false, parked = false;
	};
	std::vector<std::unique_ptr<Process>> children;
	std::vector<std::unique_ptr<Watch>> waits;
	unsigned launches = 0, prepares = 0, reports = 0, closed = 0;
	bool prepare_fail = false;
	uint32_t kill_error = 0;
	Owner owner{*this};
	Child launch()
	{
		auto p = std::make_unique<Process>();
		p->pid = ++launches;
		Child result{p.get(), p->pid, 0};
		children.push_back(std::move(p));
		return result;
	}
	Handle prepare(uint64_t id) override
	{
		++prepares;
		if (prepare_fail)
			return nullptr;
		auto w = std::make_unique<Watch>();
		w->id = id;
		auto result = w.get();
		waits.push_back(std::move(w));
		return result;
	}
	void bind(Handle w, Handle p) override { static_cast<Watch *>(w)->p = static_cast<Process *>(p); }
	void arm(Handle w, uint32_t due) override
	{
		auto &r = *static_cast<Watch *>(w);
		check(!r.retiring, "arm retired wait");
		r.due = due;
		r.parked = false;
	}
	void park(Handle w) override { static_cast<Watch *>(w)->parked = true; }
	Poll poll(Handle p) override
	{
		auto &r = *static_cast<Process *>(p);
		check(!r.closed, "poll closed handle");
		return {r.exited, r.exited, r.code, r.error};
	}
	uint32_t terminate(Handle p) override
	{
		auto &r = *static_cast<Process *>(p);
		check(!r.closed, "kill closed handle");
		++r.kills;
		return kill_error;
	}
	void retire(Handle w) override
	{
		auto &r = *static_cast<Watch *>(w);
		check(!r.retiring, "double retire");
		r.retiring = true;
	}
	void report(const Status &) override { ++reports; }
	uint32_t due(uint64_t id)
	{
		for (auto &w : waits)
			if (w->id == id)
				return w->due;
		throw std::runtime_error("missing wait");
	}
	void exit(unsigned n, uint32_t code = 0)
	{
		children.at(n)->code = code;
		children.at(n)->exited = true;
	}
	void reap()
	{
		for (auto i = waits.begin(); i != waits.end();) {
			auto &w = **i;
			if (!w.retiring) {
				++i;
				continue;
			}
			check(!w.p || w.p->exited, "closed a live child");
			if (w.p) {
				check(!w.p->closed, "double close");
				++w.p->closed;
				++closed;
			}
			auto id = w.id;
			i = waits.erase(i);
			owner.reaped(id);
		}
	}
	bool start(uint64_t id, bool compat = true)
	{
		return owner.start(id, compat, [&] { return launch(); });
	}
};
int main()
{
	try {
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			check(f.start(a), "start slow");
			auto b = f.owner.acquire({10, 100});
			check(a == b, "share target");
			check(!f.start(b), "double source injected");
			check(f.launches == 1 && f.closed == 0, "slow still owned");
			f.owner.release(a);
			check(f.children[0]->kills == 0, "cancelled another owner");
			check(!f.owner.can_start(b), "async reconnection allowed reinjection");
			f.owner.release(b);
			check(f.children[0]->kills == 1 && f.closed == 0, "terminal STOP must retain live handle");
			check(f.due(a) == Owner::drain_ms, "STOP drain deadline");
			f.exit(0);
			f.owner.wake(a, false);
			check(f.owner.pending() == 1, "ownership lost before reaper");
			f.reap();
			check(f.owner.pending() == 0 && f.closed == 1, "no-tick destruction cleanup");
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			check(f.start(a), "natural start");
			f.exit(0);
			f.owner.wake(a, false);
			auto s = f.owner.status(a);
			check(s.state == State::exited && s.code_known && s.exit_code == 0, "natural result");
			check(f.children[0]->kills == 0, "natural exit killed");
			f.owner.release(a);
			f.reap();
			auto b = f.owner.acquire({10, 100});
			check(!f.start(b), "success without Hook must not cause blind retry");
			f.owner.release(b);
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			f.start(a);
			f.exit(0, 17);
			f.owner.wake(a, false);
			f.reap();
			check(f.owner.status(a).exit_code == 17, "error result");
			f.owner.release(a);
			auto b = f.owner.acquire({10, 100});
			check(!f.start(b) && f.launches == 1, "failure retry loop");
			f.owner.release(b);
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			f.start(a);
			check(f.due(a) == 15000, "watchdog armed independently of tick");
			f.owner.wake(a, true);
			check(f.children[0]->kills == 1 && f.due(a) == 5000, "watchdog cancellation");
			f.owner.wake(a, true);
			check(f.owner.status(a).state == State::blocked && f.due(a) == 0, "drain timeout quarantined");
			check(f.closed == 0 && !f.start(a) && f.children[0]->kills == 1,
			      "no busy retry or lost handle");
			f.owner.release(a);
			f.exit(0, 1223);
			f.owner.wake(a, false);
			f.reap();
			check(f.closed == 1, "late termination cleanup");
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			f.start(a, false);
			f.owner.release(a);
			check(f.children[0]->kills == 0, "direct killed on STOP");
			f.owner.wake(a, true);
			check(f.children[0]->kills == 0 && f.due(a) == 0 && f.reports == 1,
			      "direct stall not reported");
			auto b = f.owner.acquire({10, 100});
			check(!f.start(b, false), "direct duplicate after owner destruction");
			f.exit(0);
			f.owner.wake(a, false);
			f.reap();
			f.owner.release(b);
			check(f.closed == 1, "direct natural cleanup");
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			f.start(a);
			f.owner.release(a);
			auto b = f.owner.acquire({10, 200});
			check(b != a && f.start(b), "PID reused creation must be separate");
			f.exit(1, 19);
			f.owner.wake(b, false);
			f.reap();
			f.owner.release(b);
			f.exit(0);
			f.owner.wake(a, false);
			f.reap();
			f.owner.wake(a, true);
			auto c = f.owner.acquire({10, 200});
			check(f.owner.status(c).exit_code == 19 && !f.start(c), "old completion modified new identity");
			f.owner.release(c);
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			f.start(a);
			auto b = f.owner.acquire({11, 100});
			check(f.start(b), "different PID blocked");
			f.owner.shutdown();
			check(f.children[0]->kills == 1 && f.children[1]->kills == 1,
			      "shutdown cancel exact owned children");
			f.owner.release(a);
			f.owner.release(b);
			f.exit(0);
			f.exit(1);
			f.owner.wake(a, false);
			f.owner.wake(b, false);
			f.reap();
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			check(f.owner.start(a, true,
					    [&] {
						    f.owner.release(a);
						    return f.launch();
					    }),
			      "STOP during creation");
			check(f.children[0]->kills == 1, "creation raced STOP silently");
			f.exit(0);
			f.owner.wake(a, false);
			f.reap();
			check(f.owner.pending() == 0, "raced creation retained");
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			f.prepare_fail = true;
			check(!f.start(a) && f.launches == 0, "wait allocation failure created unowned child");
			f.owner.release(a);
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			check(!f.owner.start(a, true, [] { return Child{nullptr, 0, 5}; }), "creation failure");
			f.reap();
			check(f.owner.status(a).error == 5 && !f.start(a), "creation failed retry loop");
			f.owner.release(a);
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			f.start(a);
			f.exit(0);
			f.owner.release(a);
			f.owner.wake(a, true);
			f.reap();
			check(f.children[0]->kills == 0 && f.closed == 1, "STOP/timeout raced exit");
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			f.start(a);
			f.kill_error = 5;
			f.owner.release(a);
			f.owner.wake(a, true);
			check(f.owner.status(a).state == State::blocked && f.closed == 0,
			      "failed kill falsely completed");
			f.exit(0);
			f.owner.wake(a, false);
			f.reap();
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			check(f.owner.claim_direct(a), "upstream no-helper direct reservation");
			auto b = f.owner.acquire({10, 100});
			check(!f.start(b), "helper concurrent with direct reservation");
			f.owner.release(a);
			f.owner.release(b);
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			std::promise<void> reserved, proceed;
			auto go = proceed.get_future();
			bool started = false;
			std::thread launch([&] {
				started = f.owner.start(a, true, [&] {
					reserved.set_value();
					go.wait();
					return f.launch();
				});
			});
			reserved.get_future().wait();
			auto b = f.owner.acquire({10, 100});
			check(!f.start(b), "concurrent source bypassed reservation");
			f.owner.release(a);
			f.owner.release(b);
			proceed.set_value();
			launch.join();
			check(started && f.launches == 1 && f.children[0]->kills == 1,
			      "concurrent terminal STOP lost child");
			f.exit(0);
			f.owner.wake(a, false);
			f.reap();
			check(f.closed == 1, "concurrent launch cleanup");
		}
		{
			Fake f;
			auto a = f.owner.acquire({10, 100});
			f.start(a);
			f.children[0]->error = 6;
			f.owner.wake(a, true);
			check(f.owner.status(a).state == State::blocked && f.children[0]->kills == 0 &&
				      f.waits[0]->parked,
			      "invalid observation must fail closed");
			f.children[0]->error = 0;
			f.exit(0);
			f.owner.wake(a, false);
			f.reap();
			f.owner.release(a);
		}
		std::printf(
			"injector lifecycle: %u checks PASS (real owner, simulated process/wait APIs; no injection)\n",
			checks);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
