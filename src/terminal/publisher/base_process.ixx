// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.publisher.base_process;

import std;
import devicefs.terminal.scope_exit;

export namespace devicefs::publisher {

struct ProcessOutput {
    std::string output;
    std::string diagnostic;
    int exit_code = {};
};

// Blocking readers drain both output pipes while the caller waits for the
// child. Waiting for exit before beginning those reads could deadlock a child
// whose output exceeds a pipe's capacity.
class BaseProcess {
public:
    explicit BaseProcess(const std::span<const std::string> arguments) {
        if ((arguments.empty()) || (arguments.front().empty())) {
            throw std::invalid_argument(
                "a child command must name an executable");
        }
    }

    // Wait for exit, both output EOFs, and stdin delivery. Call once per child;
    // the returned result includes the final unterminated output, and any
    // reader or writer failure is propagated to the caller.
    [[nodiscard]] auto Wait(this auto &self) -> ProcessOutput {
        auto stop = devicefs::terminal::ScopeExit{
            [&self] { self.Terminate(); }};
        auto exit = self.StartWorker([&self] { return self.WaitForExit(); });
        for (auto remaining = self.worker_count_; remaining != 0;
            --remaining) {
            self.completion_.acquire();
            const auto error = [&self] {
                const auto lock = std::lock_guard{self.error_mutex_};
                return self.error_;
            }();
            if (error) {
                self.Terminate();
                self.JoinIo();
                exit.wait();
                std::rethrow_exception(error);
            }
        }
        const auto exit_code = exit.get();
        auto output = self.output_reader_.get();
        auto diagnostic = self.diagnostic_reader_.get();
        if (self.input_writer_.valid()) {
            self.input_writer_.get();
        }
        stop.release();
        return {.output = std::move(output),
            .diagnostic = std::move(diagnostic), .exit_code = exit_code};
    }

    BaseProcess(const BaseProcess &) = delete;
    auto operator=(const BaseProcess &) -> BaseProcess & = delete;
    BaseProcess(BaseProcess &&) = delete;
    auto operator=(BaseProcess &&) -> BaseProcess & = delete;

protected:
    // Start the readers before launching the child, so thread-creation failure
    // cannot leave a running child without its output consumers. The optional
    // callback receives chunks from both reader threads and must synchronize
    // access to any shared state.
    auto StartReaders(this auto &self, auto output, auto diagnostic,
        const std::function<void(std::string_view)> &write_output) -> void {
        using NativeProcess = std::remove_reference_t<decltype(self)>;
        self.output_reader_ = self.ReadOutput(
            std::move(output), &NativeProcess::Read, write_output);
        self.diagnostic_reader_ = self.ReadOutput(
            std::move(diagnostic), &NativeProcess::Read, write_output);
    }

    // Every worker reports failure to the waiter, rather than leaving an
    // exception hidden in a future until the child happens to exit.
    [[nodiscard]] auto StartWorker(auto operation, auto...argument) {
        ++worker_count_;
        try {
            return std::async(std::launch::async,
                [this](auto operation, auto...argument) {
                    const auto finished = devicefs::terminal::ScopeExit{
                        [this] { completion_.release(); }};
                    try {
                        return std::invoke(std::move(operation),
                            std::move(argument)...);
                    } catch (...) {
                        const auto lock = std::lock_guard{error_mutex_};
                        if (!error_) {
                            error_ = std::current_exception();
                        }
                        throw;
                    }
                }, std::move(operation), std::move(argument)...);
        } catch (...) {
            --worker_count_;
            throw;
        }
    }

    auto JoinIo() noexcept -> void {
        // Cleanup joins the I/O workers without propagating their exceptions.
        // Assigning an empty future releases its previous shared state.
        // Each future is unshared and comes from `std::async` with
        // `std::launch::async`, so that release waits for its worker to finish.
        // The move assignment is `noexcept` and does not retrieve the result or
        // rethrow an exception stored by the worker.
        output_reader_ = {};
        diagnostic_reader_ = {};
        input_writer_ = {};
    }

private:
    [[nodiscard]] auto ReadOutput(auto source, const auto read,
        const std::function<void(std::string_view)> &write_output)
        -> std::future<std::string> {
        // Passing the pipe as an invocation argument closes it when the reader
        // returns or throws, even if the future retains its callable. Otherwise
        // a failed reader could leave the child blocked on an unconsumed pipe.
        return StartWorker(
            [](const decltype(source) source, const decltype(read) read,
                const std::function<void(std::string_view)> write_output) {
                auto contents = std::string{};
                for (;;) {
                    const auto chunk = read(source);
                    if (chunk.empty()) {
                        return contents;
                    }
                    contents += chunk;
                    if (write_output) {
                        write_output(chunk);
                    }
                }
            }, std::move(source), read, write_output);
    }

    // Workers can still access this state while their futures are destroyed.
    std::mutex error_mutex_;
    std::counting_semaphore<> completion_{0};
    // Only the launching thread counts workers; they report completion through
    // the semaphore without accessing this count.
    std::size_t worker_count_ = 0;
    std::exception_ptr error_;
    std::future<std::string> output_reader_;
    std::future<std::string> diagnostic_reader_;

protected:
    std::future<void> input_writer_;
};

}
