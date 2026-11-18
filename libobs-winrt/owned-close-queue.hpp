#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace wgc {

// Capacity covers live generations AND pending/in-progress closes. Reserving
// before resource creation guarantees retirement never allocates or falls back
// to calling an OS Close on the producer/graphics thread.
template<class Payload, class Policy, size_t Capacity> class OwnedCloseQueue {
	static_assert(Capacity > 0);
	static_assert(std::is_nothrow_default_constructible_v<Payload>);
	static_assert(std::is_nothrow_move_assignable_v<Payload>);
	static_assert(noexcept(std::declval<Policy &>().Close(std::declval<Payload &>())));
	enum class State { Free, Live, Pending, Closing };
	struct Slot {
		Payload payload{};
		State state = State::Free;
		uint64_t ticket = 0;
	};
	std::array<Slot, Capacity> slots_{};
	Policy policy_;
	std::mutex mutex_;
	std::condition_variable ready_, work_, drained_;
	std::thread thread_;
	bool initialized_ = false, init_done_ = false, stopping_ = false, exited_ = true, joining_ = false;
	uint64_t next_ticket_ = 0;

	bool EmptyLocked() const noexcept
	{
		for (const auto &slot : slots_)
			if (slot.state != State::Free)
				return false;
		return true;
	}
	void Run() noexcept
	{
		const bool initialized = policy_.Initialize();
		{
			std::lock_guard lock(mutex_);
			initialized_ = initialized;
			init_done_ = true;
			if (!initialized)
				exited_ = true;
		}
		ready_.notify_all();
		if (!initialized)
			return;
		for (;;) {
			size_t index = Capacity;
			{
				std::unique_lock lock(mutex_);
				work_.wait(lock, [&] {
					for (const auto &slot : slots_)
						if (slot.state == State::Pending)
							return true;
					return stopping_ && EmptyLocked();
				});
				for (size_t i = 0; i < Capacity; ++i)
					if (slots_[i].state == State::Pending &&
					    (index == Capacity || slots_[i].ticket < slots_[index].ticket))
						index = i;
				if (index == Capacity)
					break;
				slots_[index].state = State::Closing;
			}
			// No queue mutex, capture pointer, OBS graphics operation or producer
			// reference is involved in the potentially unbounded OS call.
			policy_.Close(slots_[index].payload);
			slots_[index].payload = Payload{}; // Last references released on this worker, still MTA.
			{
				std::lock_guard lock(mutex_);
				slots_[index].state = State::Free;
			}
			drained_.notify_all();
		}
		policy_.Uninitialize();
		{
			std::lock_guard lock(mutex_);
			initialized_ = false;
			exited_ = true;
		}
		drained_.notify_all();
	}

public:
	struct Lease {
		size_t index = Capacity;
		uint64_t ticket = 0;
		Lease() noexcept = default;
		Lease(size_t i, uint64_t t) noexcept : index(i), ticket(t) {}
		Lease(const Lease &) = delete;
		Lease &operator=(const Lease &) = delete;
		Lease(Lease &&other) noexcept : index(std::exchange(other.index, Capacity)), ticket(other.ticket) {}
		Lease &operator=(Lease &&other) noexcept
		{
			if (index != Capacity) // Replacing a live reservation would lose ownership.
				std::terminate();
			index = std::exchange(other.index, Capacity);
			ticket = other.ticket;
			return *this;
		}
		explicit operator bool() const noexcept { return index != Capacity; }
	};
	struct Counts { size_t live = 0, pending = 0, closing = 0; bool running = false; };
	explicit OwnedCloseQueue(Policy policy = {}) : policy_(std::move(policy)) {}
	OwnedCloseQueue(const OwnedCloseQueue &) = delete;
	OwnedCloseQueue &operator=(const OwnedCloseQueue &) = delete;
	~OwnedCloseQueue()
	{
		// Lifecycle must join before module unload. Neither detach nor a
		// successful-looking process exit is a valid substitute.
		if (thread_.joinable())
			std::terminate();
	}
	bool Start()
	{
		std::unique_lock lock(mutex_);
		if (joining_ || thread_.joinable() || !EmptyLocked())
			return false;
		initialized_ = init_done_ = stopping_ = false;
		exited_ = false;
		try {
			thread_ = std::thread([this] { Run(); });
		} catch (...) {
			exited_ = true;
			return false;
		}
		ready_.wait(lock, [&] { return init_done_; });
		if (initialized_)
			return true;
		joining_ = true;
		lock.unlock();
		thread_.join();
		lock.lock();
		joining_ = false;
		ready_.notify_all();
		return false;
	}
	Lease Reserve() noexcept
	{
		std::lock_guard lock(mutex_);
		if (!initialized_ || stopping_)
			return {};
		for (size_t i = 0; i < Capacity; ++i)
			if (slots_[i].state == State::Free) {
				slots_[i].state = State::Live;
				slots_[i].ticket = ++next_ticket_;
				return {i, slots_[i].ticket};
			}
		return {};
	}
	template<class Transfer> void Retire(Lease &lease, Transfer transfer) noexcept
	{
		static_assert(std::is_nothrow_invocable_v<Transfer, Payload &>);
		{
			std::lock_guard lock(mutex_);
			if (!lease || lease.index >= Capacity || slots_[lease.index].state != State::Live ||
			    slots_[lease.index].ticket != lease.ticket)
				std::terminate();
			auto &slot = slots_[lease.index];
			transfer(slot.payload); // Preallocated, empty destination; moves only.
			slot.state = State::Pending;
			lease.index = Capacity;
		}
		work_.notify_one();
	}
	Counts Snapshot()
	{
		std::lock_guard lock(mutex_);
		Counts counts;
		counts.running = initialized_ && !stopping_;
		for (const auto &slot : slots_) {
			counts.live += slot.state == State::Live;
			counts.pending += slot.state == State::Pending;
			counts.closing += slot.state == State::Closing;
		}
		return counts;
	}
	// Called outside graphics, before dispatcher/apartment/module destruction.
	// A timeout reports INCOMPLETE, but MUST NOT authorize unload or detach.
	template<class Incomplete> bool StopAndJoin(std::chrono::milliseconds deadline, Incomplete incomplete)
	{
		std::unique_lock lock(mutex_);
		ready_.wait(lock, [&] { return !joining_; });
		if (!thread_.joinable())
			return true;
		stopping_ = true;
		work_.notify_all();
		const bool within_deadline = drained_.wait_for(lock, deadline, [&] { return exited_; });
		joining_ = true;
		lock.unlock();
		if (!within_deadline)
			incomplete();
		thread_.join(); // Owned even if OS Close is infinite: external Job must report FAIL.
		lock.lock();
		joining_ = false;
		ready_.notify_all();
		return within_deadline;
	}
};
} // namespace wgc
