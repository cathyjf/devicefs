// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.coherent_vector;

import std;

// A collection for concurrent readers and writers. The `Read` method returns
// a coherent snapshot of the collection as it existed at some point.
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

    [[nodiscard]] auto Read() const noexcept {
        return current_.load();
    }

private:
    auto Update(const auto &change) -> void {
        const auto lock = std::lock_guard{writer_mutex_};
        const auto current = Read();
        auto next = current ? *current : std::vector<T>{};
        std::invoke(change, next);
        current_.store(
            std::make_shared<const std::vector<T>>(std::move(next)));
    }

    std::mutex writer_mutex_;
    std::atomic<std::shared_ptr<const std::vector<T>>> current_;
};
