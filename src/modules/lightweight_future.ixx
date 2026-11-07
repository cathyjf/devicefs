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

template <class T>
auto ReadFlag(const T &flag, const std::memory_order order) noexcept {
    if constexpr (std::is_same_v<std::remove_cvref_t<T>, bool>) {
        std::ignore = order;
        return flag;
    } else {
        return flag.test(order);
    }
}

template <class T>
auto SetFlag(T &flag, const std::memory_order order) noexcept {
    if constexpr (std::is_same_v<std::remove_cvref_t<T>, bool>) {
        std::ignore = order;
        flag = true;
    } else {
        flag.test_and_set(order);
    }
}

template <class Context, class Result, bool AtomicReadiness>
class FutureState {
    using Error = std::exception_ptr;
public:
    FutureState(Context &&context) noexcept(noexcept(Construct(
            state_.context, std::forward<Context>(context)))) {
        Construct(state_.context, std::forward<Context>(context));
    }

    auto SetResult() noexcept {
        SetFlag(has_result_, std::memory_order_release);
    }

    template <class T>
    auto SetResult(T &&result) noexcept(noexcept(ClearContext())) {
        ClearContext();
        if constexpr (std::is_same_v<std::remove_cvref_t<T>, Result>) {
            static_assert(std::is_nothrow_constructible_v<Result, T>);
            Construct(state_.result, std::forward<T>(result));
        } else {
            Construct(state_.error, std::forward<T>(result));
            has_error_ = true;
        }
        SetResult();
    }

    auto GetContext() noexcept -> decltype(auto) {
        return (state_.context);
    }

    template <bool TestForError>
    auto GetResult() noexcept(!TestForError) -> decltype(auto) {
        if constexpr (TestForError) {
            if (has_error_) {
                std::rethrow_exception(state_.error);
            }
        }
        if constexpr (!std::is_same_v<Result, std::monostate>) {
            return (state_.result);
        }
    }

    bool HasResult() const noexcept {
        return ReadFlag(has_result_, std::memory_order_acquire);
    }

    FutureState(const FutureState &) = delete;
    FutureState(FutureState &&) = delete;
    auto operator=(const FutureState &) -> FutureState & = delete;
    auto operator=(FutureState &&) -> FutureState & = delete;

    ~FutureState() noexcept(noexcept(ClearContext()) &&
            std::is_nothrow_destructible_v<Result>) {
        if (has_error_) {
            std::destroy_at(std::addressof(state_.error));
        } else if constexpr (std::is_same_v<Result, std::monostate>) {
            ClearContext();
        } else if (!ReadFlag(has_result_, std::memory_order_relaxed)) {
            ClearContext();
        } else {
            std::destroy_at(std::addressof(state_.result));
        }
    }

private:
    template <class T>
    static auto Construct(T &buffer, auto &&...arguments)
        noexcept(std::is_nothrow_constructible_v<T, decltype(arguments)...>) {
        ::new (std::addressof(buffer)) T{
            std::forward<decltype(arguments)>(arguments)...};
    }

    auto ClearContext() noexcept(std::is_nothrow_destructible_v<Context>) {
        std::destroy_at(std::addressof(state_.context));
    }

    __pragma(warning(push))
    __pragma(warning(disable : 26495, justification:
        "Construction and destruction of the union members is handled by "
        "the enclosing class."))
    [[msvc::no_unique_address]] union StateUnion {
        Context context;
        Result result;
        Error error;

        StateUnion() noexcept {}
        StateUnion(const StateUnion &) = delete;
        StateUnion(StateUnion &&) = delete;
        auto operator=(const StateUnion &) -> StateUnion & = delete;
        auto operator=(StateUnion &&) -> StateUnion & = delete;
        ~StateUnion() {}
    } state_;
    __pragma(warning(pop))
    [[msvc::no_unique_address]] std::conditional_t<
        AtomicReadiness, std::atomic_flag, bool> has_result_{};
    [[msvc::no_unique_address]] bool has_error_ = false;
};

export enum class FuturePolicy {
    Default = 0,
    CancelOnDestruction = 1,
    ProvideAtomicReadiness = 2,
    AllowMultipleRetrieval = 4
};

export constexpr auto operator&(const FuturePolicy policy,
    const FuturePolicy flag) noexcept {
    return (std::to_underlying(policy) & std::to_underlying(flag)) != 0;
}

export constexpr auto operator|(const FuturePolicy left,
    const FuturePolicy right) noexcept {
    return static_cast<FuturePolicy>(
        std::to_underlying(left) | std::to_underlying(right));
}

export template <class Callable, class Context,
    FuturePolicy Policy = FuturePolicy::CancelOnDestruction>
class LightweightFuture {
    using UniqueWork = std::conditional_t<
        Policy & FuturePolicy::CancelOnDestruction,
            wil::unique_threadpool_work,
            wil::unique_threadpool_work_nocancel>;
public:
    using ResultType = std::invoke_result_t<Callable, Context &>;

    LightweightFuture(Context &&context, const bool async = true,
        auto &&...error) requires (sizeof...(error) <= 1) :
        state_{std::forward<Context>(context)},
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

    // Precondition without `AllowMultipleRetrieval`:
    //   `Get` has never previously been called on this object.
    //
    // Preconditions with `AllowMultipleRetrieval`:
    //   (1) The constructor used to create this object must have been called
    //       with the `async` parameter set to `true`; and
    //
    //   (2) No other thread is currently executing `Get`.
    //
    // If any parameters are supplied, they are set to `true` the first time
    // that a state change is observed.
    auto Get(auto &&...observers) -> decltype(auto) {
        if (work_) {
            std::ignore = wil::unique_threadpool_work_nocancel{
                work_.release()};
            ((observers = true), ...);
            return state_.template GetResult<true>();
        }
        if constexpr (Policy & FuturePolicy::AllowMultipleRetrieval) {
            return state_.template GetResult<true>();
        } else {
            RunCallback();
            ((observers = true), ...);
            return state_.template GetResult<false>();
        }
    }

    auto IsReady() const noexcept
        requires (Policy & FuturePolicy::ProvideAtomicReadiness) {
        return state_.HasResult();
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
        auto &context = state_.GetContext();
        if constexpr (std::is_void_v<ResultType>) {
            Callable::operator()(context);
            state_.SetResult();
        } else {
            state_.SetResult(Callable::operator()(context));
        }
    }

    auto TryRunCallback() noexcept {
        try {
            RunCallback();
        } catch (...) {
            state_.SetResult(std::current_exception());
        }
    }

    [[msvc::no_unique_address]] FutureState<Context,
        std::conditional_t<std::is_void_v<ResultType>,
            std::monostate, ResultType>,
        Policy & FuturePolicy::ProvideAtomicReadiness> state_;
    [[msvc::no_unique_address]] UniqueWork work_;
};

export template <FuturePolicy Policy = FuturePolicy::Default, class Callable>
auto async(Callable callback) {
    using FutureCallback = decltype([](Callable &callback_) static {
        return callback_();
    });
    return LightweightFuture<FutureCallback, Callable, Policy>{
        std::move(callback)};
}

export template <class Callable, FuturePolicy Policy = FuturePolicy::Default>
using AsyncFuture = decltype(
    devicefs::async<Policy>(std::declval<Callable>()));

} // namespace devicefs
