// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <cerrno>
#include <stdlib.h>
// `unistd.h` is required for `mkdtemp` on macOS.
#include <unistd.h>

export module devicefs.asr_trace.experiment;
import std;
import devicefs.asr_trace.filesystem;
import devicefs.asr_trace.trace;

export namespace devicefs::asr_trace {

template <typename Producer, typename Filesystem>
concept ImageProducerFor = ImageFilesystem<Filesystem> &&
    requires(Producer &producer, Filesystem &filesystem) {
        { producer.Produce(filesystem) } -> std::same_as<void>;
    };

auto CreateRunDirectory(const std::filesystem::path &root) -> std::filesystem::path {
    std::filesystem::create_directories(root);
    auto pattern = (std::filesystem::absolute(root) / "run-XXXXXX").string();
    if (!mkdtemp(pattern.data())) {
        throw std::system_error{errno, std::generic_category(),
            std::format("could not create run directory under '{}'", root.string())};
    }
    return pattern;
}

template <ImageFilesystem Filesystem,
    ImageProducerFor<RecordingFilesystem<Filesystem>> Producer>
auto RunExperiment(Filesystem &filesystem, Producer &producer,
    const std::filesystem::path &trace_path) -> Report {
    auto recording = RecordingFilesystem{filesystem, trace_path};
    auto error = std::exception_ptr{};
    recording.SetPhase(Phase::Restore);
    try {
        producer.Produce(recording);
    } catch (...) {
        error = std::current_exception();
    }
    const auto report = [&recording, &error = std::as_const(error), &trace_path] {
        try {
            recording.SetPhase(Phase::Finalize);
            recording.SetPhase(Phase::Detached);
            return recording.Finish(!error);
        } catch (const std::exception &finalization_error) {
            if (!error) {
                throw;
            }
            std::println(std::cerr, "could not finalize trace '{}': {}",
                trace_path.c_str(), finalization_error.what());
            std::rethrow_exception(error);
        }
    }();
    if (error) {
        std::rethrow_exception(error);
    }
    return report;
}

} // namespace devicefs::asr_trace
