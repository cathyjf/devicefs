// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.scope_exit;

import std;

export namespace devicefs::terminal {

// ScopeExit owns a cleanup action until destruction or an explicit release.
// Moving the guard transfers that responsibility to the destination, allowing
// functions to return guards whose cleanup belongs to their callers' scopes.
template <typename Cleanup>
    requires std::invocable<Cleanup &>
class ScopeExit {
public:
    explicit ScopeExit(Cleanup cleanup)
        noexcept(std::is_nothrow_move_constructible_v<Cleanup>)
        : cleanup_{std::move(cleanup)} {}

    ScopeExit(const ScopeExit &) = delete;
    auto operator=(const ScopeExit &) -> ScopeExit & = delete;
    auto operator=(ScopeExit &&) -> ScopeExit & = delete;

    ScopeExit(ScopeExit &&other)
        noexcept(std::is_nothrow_move_constructible_v<Cleanup>)
        : cleanup_{std::move(other.cleanup_)},
          active_{std::exchange(other.active_, false)} {}

    ~ScopeExit() noexcept {
        if (active_) {
            std::invoke(cleanup_);
        }
    }

    auto release() noexcept -> void {
        active_ = false;
    }

private:
    Cleanup cleanup_;
    bool active_ = true;
};

}
