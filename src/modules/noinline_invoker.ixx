// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.noinline_invoker;

import std;

namespace devicefs::noinline_invoker_detail {

// The definition of `InvokeOutOfLineErased` is compiled without whole-program
// optimization because MSVC has been observed to inline functions despite
// their `noinline` annotations.
[[msvc::noinline]]
auto InvokeOutOfLineErased(
    const volatile void *, void *,
    auto (&)(const volatile void *, void *) -> void) -> void;

} // namespace devicefs::noinline_invoker_detail

export namespace devicefs {

// Invoke `function` synchronously, keeping its body out of the caller.
template <class Function>
    requires std::invocable<Function &>
[[msvc::forceinline]]
auto InvokeOutOfLine(Function &&function) {
    using Result = std::invoke_result_t<Function &>;
    // A function reference needs a pointer object because the erased context
    // carries an object address. Object callables are passed by reference.
    decltype(auto) callable = [&function]() -> decltype(auto) {
        if constexpr (std::is_function_v<std::remove_reference_t<Function>>) {
            return std::addressof(function);
        } else {
            return (function);
        }
    }();
    using Callable = std::remove_reference_t<decltype(callable)>;
    using Trampoline = decltype([](
        const volatile void *const context, void *const output) static {
        [[gsl::suppress("26492", justification:
            "The `const_cast` removes only qualifiers added by the conversion "
            "to `const volatile void *`. Meanwhile, `Callable` includes the "
            "original object's qualifiers, so that the callable can be "
            "accessed with its original qualifications.")]]
        auto &target = *const_cast<Callable *>(
            static_cast<const volatile Callable *>(context));
        if constexpr (std::is_void_v<Result>) {
            std::ignore = output;
            std::invoke(target);
        } else {
            ::new (output) Result(std::invoke(target));
        }
    });

    using Storage = std::conditional_t<std::is_void_v<Result>, char, Result>;
    /* [[indeterminate]] */ alignas(Storage) char storage[sizeof(Storage)];
    void *const address = std::is_void_v<Result> ?
        nullptr : std::addressof(storage);
    noinline_invoker_detail::InvokeOutOfLineErased(
        std::addressof(callable), address, Trampoline::operator());

    if constexpr (!std::is_void_v<Result>) {
        // The owner is created after construction has succeeded, so that an
        // exception from the invocation of `function` cannot cause destruction
        // of an unconstructed `Result`.
        using DestroyResult = decltype([](Result *const object) static {
            std::destroy_at(object);
        });
        return std::move(*std::unique_ptr<Result, DestroyResult>{
            std::launder(static_cast<Result *>(address))});
    }
}

} // namespace devicefs
