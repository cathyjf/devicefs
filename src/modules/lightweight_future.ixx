// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

export module devicefs.lightweight_future;

import std;
import <devicefs/windows_imports.h>;

namespace devicefs {

export template <class Callable, bool CancelOnDestruction, class Context>
class LightweightFuture {
    using UniqueWork = std::conditional_t<CancelOnDestruction,
        wil::unique_threadpool_work, wil::unique_threadpool_work_nocancel>;
public:
    using ResultType = std::invoke_result_t<Callable, Context &>;

    LightweightFuture(Context &&context, const bool async = true,
        auto &&...error) requires (sizeof...(error) <= 1) :
        result_{std::forward<Context>(context)},
        work_{[this, async, ...error_ = std::forward<decltype(error)>(error)]
            -> UniqueWork {
            if (!async) {
                return nullptr;
            }
            if (const auto work =
                    CreateThreadpoolWork(WorkCallback, this, nullptr); work) {
                SubmitThreadpoolWork(work);
                return UniqueWork{work};
            }
            if constexpr (sizeof...(error_) == 0) {
                throw std::bad_alloc{};
            } else {
                (error_(), ...);
            }
        }()} {}

    // Precondition: `Get` has never previously been called on this object.
    auto Get() -> decltype(auto) {
        if (work_) {
            std::ignore =
                wil::unique_threadpool_work_nocancel{work_.release()};
            if (std::holds_alternative<std::exception_ptr>(result_)) {
                std::rethrow_exception(std::get<std::exception_ptr>(result_));
            }
        } else {
            RunCallback();
        }
        if constexpr (!std::is_void_v<ResultType>) {
            return std::get<ResultType>(result_);
        }
    }

private:
    LightweightFuture(LightweightFuture &&) = delete;

    [[gsl::suppress("26429", justification:
        "The function prototype must match the requirements of the relevant "
        "Windows APIs, including the use of the nullable `context_`.")]]
    static auto CALLBACK WorkCallback(_Inout_ PTP_CALLBACK_INSTANCE,
        _Inout_opt_ PVOID context_, _Inout_ PTP_WORK) noexcept -> void {
        static_cast<LightweightFuture *>(context_)->TryRunCallback();
    }

    auto RunCallback() {
        auto &context = std::get<Context>(result_);
        if constexpr (std::is_void_v<ResultType>) {
            Callable::operator()(context);
        } else {
            result_ = Callable::operator()(context);
        }
    }

    auto TryRunCallback() noexcept {
        try {
            RunCallback();
        } catch (...) {
            result_ = std::current_exception();
        }
    }

    std::variant<Context,
        std::conditional_t<std::is_void_v<ResultType>,
            std::monostate, ResultType>,
        std::exception_ptr> result_;
    UniqueWork work_;
};

export template <class Callable>
auto async(Callable callback) {
    using FutureCallback = decltype([](Callable &callback_) static {
        return callback_();
    });
    return LightweightFuture<FutureCallback, false, Callable>{
        std::move(callback)};
}

export template <class Callable>
using AsyncFuture = decltype(devicefs::async(std::declval<Callable>()));

} // namespace devicefs
