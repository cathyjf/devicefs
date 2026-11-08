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
concept IsAtomicFlagType = std::is_same_v<
    std::remove_cvref_t<T>, std::atomic_flag>;

using AtomicReadinessFlag = std::conditional_t<
    std::atomic<bool>::is_always_lock_free,
        std::atomic<bool>, std::atomic_flag>;

template <class T>
auto ReadFlag(const T &flag, const std::memory_order order) noexcept {
    if constexpr (IsAtomicFlagType<T>) {
        return flag.test(order);
    } else {
        return flag.load(order);
    }
}

template <class T>
auto SetFlag(T &flag, const std::memory_order order) noexcept {
    if constexpr (IsAtomicFlagType<T>) {
        flag.test_and_set(order);
    } else {
        flag.store(true, order);
    }
}

struct VoidSentinel {};

struct EmptyError {
    explicit constexpr operator bool() const noexcept {
        return false;
    }
};

class EmptyDiscriminator {
public:
    constexpr auto SetSuccessfulResult() const noexcept {}
    constexpr auto SetErrorResult() const noexcept {}
    constexpr auto HasError() const noexcept { return false; }
};

template <bool NeedsErrorStorage>
class CombinedDiscriminator {
    static auto constexpr kInitialValue = 0;
    static auto constexpr kHasResult = 1;
    static auto constexpr kHasError = 2;
public:
    constexpr auto SetSuccessfulResult() noexcept {
        value_ = kHasResult;
    }
    constexpr auto SetErrorResult() noexcept {
        if constexpr (NeedsErrorStorage) {
            value_ = kHasError;
        }
    }
    constexpr auto HasResult(auto) const noexcept {
        return value_ != kInitialValue;
    }
    constexpr auto HasError() const noexcept {
        if constexpr (NeedsErrorStorage) {
            return value_ == kHasError;
        } else {
            return false;
        }
    }
private:
    [[msvc::no_unique_address]] std::uint8_t value_ = kInitialValue;
};

template <bool NeedsErrorStorage>
class AtomicDiscriminator {
public:
    auto SetSuccessfulResult() noexcept {
        SetFlag(has_result_, std::memory_order_release);
    }
    auto SetErrorResult() noexcept {
        has_error_ = true;
        SetSuccessfulResult();
    }
    auto HasResult(const std::memory_order order) const noexcept {
        return ReadFlag(has_result_, order);
    }
    constexpr auto HasError() const noexcept {
        return has_error_;
    }
private:
    [[msvc::no_unique_address]] AtomicReadinessFlag has_result_{};
    [[msvc::no_unique_address]] std::conditional_t<
        NeedsErrorStorage, bool, EmptyError> has_error_{};
};

template <class Context, class Result, bool AtomicReadiness,
    bool NeedsErrorStorage>
using Discriminator = std::conditional_t<
    AtomicReadiness,
    AtomicDiscriminator<NeedsErrorStorage>,
    std::conditional_t<
        NeedsErrorStorage ||
            (!std::is_same_v<Result, VoidSentinel> &&
                !(std::is_trivially_destructible_v<Context> &&
                    std::is_trivially_destructible_v<Result>)),
        CombinedDiscriminator<NeedsErrorStorage>,
        EmptyDiscriminator>>;

template <class Context, class Result, bool AtomicReadiness,
    bool NeedsErrorStorage>
class FutureState {
    using Error = std::conditional_t<NeedsErrorStorage,
        std::exception_ptr, EmptyError>;
public:
    FutureState(Context &&context) noexcept(noexcept(Construct(
            state_.context, std::forward<Context>(context)))) {
        Construct(state_.context, std::forward<Context>(context));
    }

    auto SetResult() noexcept {
        flag_.SetSuccessfulResult();
    }

    template <bool IsError, class T>
    auto SetResult(T &&result) noexcept(noexcept(ClearContext())) {
        ClearContext();
        if constexpr (!IsError) {
            static_assert(std::is_nothrow_constructible_v<Result, T>);
            Construct(state_.result, std::forward<T>(result));
            flag_.SetSuccessfulResult();
        } else {
            static_assert(NeedsErrorStorage);
            Construct(state_.error, std::forward<T>(result));
            flag_.SetErrorResult();
        }
    }

    auto GetContext() noexcept -> decltype(auto) {
        return (state_.context);
    }

    template <bool TestForError>
    auto GetResult() noexcept(!NeedsErrorStorage || !TestForError)
        -> decltype(auto) {
        if constexpr (NeedsErrorStorage && TestForError) {
            if (flag_.HasError()) {
                std::rethrow_exception(state_.error);
            }
        }
        if constexpr (!std::is_same_v<Result, VoidSentinel>) {
            return (state_.result);
        }
    }

    bool HasResult() const noexcept {
        return flag_.HasResult(std::memory_order_acquire);
    }

    FutureState(const FutureState &) = delete;
    FutureState(FutureState &&) = delete;
    auto operator=(const FutureState &) -> FutureState & = delete;
    auto operator=(FutureState &&) -> FutureState & = delete;

    ~FutureState() noexcept(noexcept(ClearContext()) &&
            std::is_nothrow_destructible_v<Result>) {
        if (NeedsErrorStorage && flag_.HasError()) {
            std::destroy_at(std::addressof(state_.error));
        } else if constexpr (std::is_same_v<Result, VoidSentinel>) {
            ClearContext();
        } else if constexpr (requires {
            flag_.HasResult(std::memory_order_relaxed);
        }) {
            if (!flag_.HasResult(std::memory_order_relaxed)) {
                ClearContext();
            } else {
                std::destroy_at(std::addressof(state_.result));
            }
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
    [[msvc::no_unique_address]] Discriminator<
        Context, Result, AtomicReadiness, NeedsErrorStorage> flag_;
};

export enum class FuturePolicy {
    Default = 0,
    AllowSynchronousCallback = 1,
    CancelOnDestruction = 2,
    ProvideAtomicReadiness = 4,
    AllowMultipleRetrieval = 8
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
    static_assert(!std::is_same_v<ResultType, VoidSentinel>);
    static_assert(std::is_void_v<ResultType> ||
        std::is_nothrow_destructible_v<Context>);

    // The `AllowSynchronousCallback` and `AllowMultipleRetrieval` policies are
    // not supported at the same time as each other.
    static_assert(!(Policy & FuturePolicy::AllowSynchronousCallback) ||
        !(Policy & FuturePolicy::AllowMultipleRetrieval));

    // Precondition: if an `error` callable is supplied, invoking it must not
    // return normally.
    LightweightFuture(Context &&context, const bool async = true,
        auto &&...error) requires (sizeof...(error) <= 1) :
        state_{std::forward<Context>(context)},
        work_{[this, async, ...error_ = std::forward<decltype(error)>(error)]
            -> UniqueWork {
            if constexpr (Policy & FuturePolicy::AllowSynchronousCallback) {
                if (!async) {
                    return nullptr;
                }
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
    // Precondition with `AllowMultipleRetrieval`:
    //   `this->Get()` is not executing on any other thread.
    //
    // If any parameters are supplied, they are set to `true` the first time
    // that a state change is observed.
    //
    // If `AllowStateChange` is `false` (which is allowed only with
    // `AllowMultipleRetrieval`), the function returns `std::nullopt` if the
    // result has not been previously collected, and returns an `std::optional`
    // containing a copy of the result if it has been previously collected.
    template <bool AllowStateChange = true>
        requires (AllowStateChange ||
            (Policy & FuturePolicy::AllowMultipleRetrieval))
    auto Get(auto &&...observers)
        noexcept(noexcept(RunCallback()) &&
            (AllowStateChange ?
                (noexcept(observers = true) && ...) :
                std::is_nothrow_constructible_v<
                    ResultType, std::add_lvalue_reference_t<ResultType>>))
        -> decltype(auto) {
        using OptionalReturn = std::optional<ResultType>;
        if (!(Policy & (FuturePolicy::AllowSynchronousCallback |
                FuturePolicy::AllowMultipleRetrieval)) || work_) {
            if constexpr (AllowStateChange) {
                std::ignore = wil::unique_threadpool_work_nocancel{
                    work_.release()};
                ((observers = true), ...);
                return state_.template GetResult<true>();
            } else {
                return OptionalReturn{};
            }
        }
        if constexpr (Policy & FuturePolicy::AllowMultipleRetrieval) {
            if constexpr (AllowStateChange) {
                return state_.template GetResult<true>();
            } else {
                return OptionalReturn{state_.template GetResult<true>()};
            }
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

    static constexpr auto kCallbackExecutionIsNonthrowing =
        std::is_nothrow_invocable_v<Callable, Context &>;

    auto RunCallback() noexcept(kCallbackExecutionIsNonthrowing) {
        auto &context = state_.GetContext();
        if constexpr (std::is_void_v<ResultType>) {
            Callable::operator()(context);
            state_.SetResult();
        } else {
            state_.template SetResult<false>(Callable::operator()(context));
        }
    }

    auto TryRunCallback() noexcept {
        if constexpr (kCallbackExecutionIsNonthrowing) {
            RunCallback();
        } else {
            try {
                RunCallback();
            } catch (...) {
                state_.template SetResult<true>(std::current_exception());
            }
        }
    }

    [[msvc::no_unique_address]] FutureState<Context,
        std::conditional_t<std::is_void_v<ResultType>,
            VoidSentinel, ResultType>,
        Policy & FuturePolicy::ProvideAtomicReadiness,
        !kCallbackExecutionIsNonthrowing> state_;
    [[msvc::no_unique_address]] UniqueWork work_;
};

export template <FuturePolicy Policy = FuturePolicy::Default, class Callable>
auto async(Callable callback) {
    // Note: If we later add support for returning a reference from the
    // callback, the return type of this lambda should be changed to
    // `decltype(auto)` and the noexcept expression should be changed to
    // `noexcept(std::is_nothrow_invocable_v<Callable &>)`.
    using FutureCallback = decltype([](Callable &callback_) static
        noexcept(std::is_nothrow_invocable_r_v<
            std::decay_t<std::invoke_result_t<Callable &>>, Callable &>) {
        return callback_();
    });
    return LightweightFuture<FutureCallback, Callable, Policy>{
        std::move(callback)};
}

export template <class Callable, FuturePolicy Policy = FuturePolicy::Default>
using AsyncFuture = decltype(
    devicefs::async<Policy>(std::declval<Callable>()));

} // namespace devicefs
