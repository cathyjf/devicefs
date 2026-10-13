// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.coherent_vector;

import std;
import devicefs.terminal.scope_exit;

// A collection for concurrent readers and writers.
// Reading does not block on concurrent writing but instead is guaranteed to
// return a coherent snapshot of the collection as it existed at some point.
export
template <class T>
class CoherentVector {
public:
    auto PushBack(auto &&value) -> void {
        Update([&value](auto &next) {
            next.push_back(std::forward<decltype(value)>(value));
        });
    }

    auto Erase(const T &value) -> void {
        Update([&value](auto &next) { std::erase(next, value); });
    }

    [[nodiscard]] auto Read() const {
        ++readers_;
        const auto finished = devicefs::terminal::ScopeExit{[this] {
            --readers_;
            readers_.notify_one();
        }};
        const auto current = current_.load();
        return current ? *current : std::vector<T>{};
    }

private:
    auto Update(const auto &change) -> void {
        const auto lock = std::lock_guard{writer_mutex_};
        auto next = Read();
        std::invoke(change, next);
        if (!versions_.empty() && !reclaimer_.joinable()) {
            reclaimer_ = std::jthread{[this](const std::stop_token stop) {
                Reclaim(stop);
            }};
        }
        versions_.push_front(std::move(next));
        current_ = &versions_.front();
        version_changed_.notify_one();
    }

    auto Reclaim(const std::stop_token stop) -> void {
        while (!stop.stop_requested()) {
            auto lock = std::unique_lock{writer_mutex_};
            const auto retained = versions_.begin();
            lock.unlock();
            for (auto count = readers_.load(); count != 0;
                count = readers_.load()) {
                readers_.wait(count);
            }
            // Writers can publish more versions while we wait. Delete only
            // nodes older than `retained`, because a new reader may already
            // be copying a version published during the wait.
            lock.lock();
            versions_.erase_after(retained, versions_.end());
            std::ignore = version_changed_.wait(lock, stop, [this, retained] {
                return current_.load() != &*retained;
            });
        }
    }

    std::mutex writer_mutex_;
    std::forward_list<std::vector<T>> versions_;
    // `Read` increments `readers_` before loading `current_`. Sequential
    // consistency prevents the worker from missing a reader that chose an old
    // version and has not yet finished copying it.
    // https://eel.is/c++draft/atomics.order
    std::atomic<const std::vector<T> *> current_{};
    mutable std::atomic<unsigned> readers_{};
    std::condition_variable_any version_changed_;
    // `reclaimer_` is destroyed first, so the worker stops before the members
    // it accesses are destroyed.
    std::jthread reclaimer_;
};
