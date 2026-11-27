#pragma once

#include <utility>

namespace wgc {
// Polled by the graphics owner. There is no completion handler referencing a
// capture or session. The operation follows its session into retirement.
template<typename Operation> class BorderAccess {
	Operation operation{nullptr};
	bool pending = false;

public:
	template<typename Request> void Begin(Request request) noexcept
	{
		try {
			operation = request();
			pending = static_cast<bool>(operation);
		} catch (...) {
			pending = false;
		}
	}

	template<typename Session> void Poll(const Session &session) noexcept
	{
		if (!pending)
			return;
		try {
			const auto status = operation.Status();
			if (status == decltype(status)::Started)
				return;
			pending = false;
			if (status == decltype(status)::Completed &&
			    operation.GetResults() == decltype(operation.GetResults())::Allowed)
				session.IsBorderRequired(false);
		} catch (...) {
			pending = false;
		}
	}

	Operation Retire() noexcept
	{
		pending = false;
		return std::exchange(operation, nullptr);
	}
	explicit operator bool() const noexcept { return static_cast<bool>(operation); }
};
}
