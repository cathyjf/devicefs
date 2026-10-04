// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.publisher.base_process;

import std;

export namespace devicefs::publisher {

struct ProcessOutput {
    std::string output;
    std::string diagnostic;
    std::optional<int> exit_code;
};

// Native adapters supply pipe reads and process completion. This common owner
// retains stdout separately from stderr and waits for both pipe EOFs before
// reporting completion, so exit does not discard the final buffered output.
class BaseProcess {
public:
    explicit BaseProcess(const std::span<const std::string> arguments) {
        if ((arguments.empty()) || (arguments.front().empty())) {
            throw std::invalid_argument(
                "a child command must name an executable");
        }
    }

    [[nodiscard]] auto Poll(this auto &self) -> ProcessOutput {
        auto update = ProcessOutput{
            .output = self.output_ ?
                self.ReadAvailable(self.output_) : std::string{},
            .diagnostic = self.diagnostic_ ?
                self.ReadAvailable(self.diagnostic_) : std::string{},
            .exit_code = std::nullopt,
        };
        self.contents_.output += update.output;
        self.contents_.diagnostic += update.diagnostic;
        if (!self.contents_.exit_code) {
            self.contents_.exit_code = self.ReadExitCode();
        }
        if (self.contents_.exit_code && !self.output_ && !self.diagnostic_) {
            if (self.input_writer_.valid()) {
                self.input_writer_.get();
            }
            update.exit_code = self.contents_.exit_code;
        }
        return update;
    }

    [[nodiscard]] auto Result() const noexcept -> const ProcessOutput & {
        return contents_;
    }

    BaseProcess(const BaseProcess &) = delete;
    auto operator=(const BaseProcess &) -> BaseProcess & = delete;
    BaseProcess(BaseProcess &&) = delete;
    auto operator=(BaseProcess &&) -> BaseProcess & = delete;

protected:
    std::future<void> input_writer_;

private:
    ProcessOutput contents_;
};

}
