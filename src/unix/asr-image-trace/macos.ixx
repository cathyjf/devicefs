// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <fuse.h>
#include <cerrno>
#include <mach-o/dyld.h>
#include <sys/snapshot.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

export module devicefs.asr_trace.macos;

import std;
import devicefs.terminal.safecast;
import devicefs.asr_trace.filesystem;
import devicefs.asr_trace.trace;
import devicefs.asr_trace.synthetic;

namespace devicefs::asr_trace {

class FuseImage final {
public:
    FuseImage(const std::filesystem::path &seed, const std::filesystem::path &trace)
        : filesystem_{seed}, recording_{filesystem_, trace} {
        if (::stat(seed.c_str(), &attributes_) < 0) {
            throw std::system_error{errno, std::generic_category(),
                std::format("inspect initial image fixture '{}'", seed.string())};
        }
    }

    auto Fail() noexcept -> void { recording_.Fail(); }

    auto Attributes(const std::string_view path, struct stat &output) const -> int {
        output = attributes_;
        if (path == "/") {
            output.st_mode = S_IFDIR | 0700;
            output.st_nlink = 2;
            output.st_size = 0;
            output.st_blocks = 0;
        } else if (path == "/marker") {
            output.st_mode = S_IFREG | 0200;
            output.st_nlink = 1;
            output.st_size = 0;
            output.st_blocks = 0;
        } else if (path == "/image.raw") {
            output.st_mode = S_IFREG | 0600;
        } else {
            return -ENOENT;
        }
        return 0;
    }

    auto Statistics(struct statvfs &output) const -> int {
        output = {};
        output.f_bsize = output.f_frsize = 4096;
        output.f_blocks = filesystem_.Size / output.f_frsize +
            (filesystem_.Size % output.f_frsize != 0);
        output.f_bfree = output.f_bavail = output.f_blocks;
        output.f_files = 3;
        output.f_namemax = std::string_view{"image.raw"}.size();
        return 0;
    }

    auto Read(const off_t offset, const std::span<char> buffer) -> int {
        return Transfer([&recording = recording_, offset, buffer] {
            return recording.Read(offset, buffer);
        });
    }

    auto Write(const off_t offset, const std::span<const char> data) -> int {
        return Transfer([&recording = recording_, offset, data] {
            return recording.Write(offset, data.size(), data);
        });
    }

    auto Synchronize() -> int { return -recording_.Synchronize(); }

    auto Truncate(const std::string_view path, const off_t size) -> int {
        if (path == "/image.raw") {
            recording_.Truncate(size);
        }
        return 0;
    }

    auto Mark(std::string_view token) -> int {
        const auto length = token.size();
        if (token.ends_with('\n')) {
            token.remove_suffix(1);
        }
        const auto lock = std::lock_guard{marker_mutex_};
        if ((token == "restore-begin") && (phase_ == Phase::Setup)) {
            phase_ = Phase::Restore;
        } else if (((token == "producer-success") || (token == "producer-failure")) &&
            (phase_ == Phase::Restore)) {
            phase_ = Phase::Finalize;
            producer_succeeded_ = token == "producer-success";
        } else if ((token == "detached") && (phase_ == Phase::Finalize)) {
            phase_ = Phase::Detached;
        } else if (token == "failure") {
            Fail();
            return length;
        } else {
            return -EINVAL;
        }
        recording_.SetPhase(phase_);
        return length;
    }

    auto Finish(const int fuse_result) -> int {
        if (fuse_result != 0) {
            Fail();
        }
        const auto report = recording_.Finish(producer_succeeded_);
        std::println("Trace {}. Restore and finalization have {} nonforward write requests.",
            report.complete ? "complete" : "incomplete", report.NonforwardRequests());
        std::println("Reads after setup: {} requests, {} bytes.",
            report.ReadRequestsAfterSetup(), report.ReadBytesAfterSetup());
        return report.complete ? 0 : 1;
    }

private:
    template <typename Operation>
    auto Transfer(Operation operation) -> int {
        const auto result = operation();
        return result.error != 0 ? -result.error :
            devicefs::terminal::FailFastCast<int>(std::min(result.transferred,
                std::size_t{std::numeric_limits<int>::max()}));
    }

    SeedFilesystem filesystem_;
    RecordingFilesystem<SeedFilesystem> recording_;
    struct stat attributes_ {};
    std::mutex marker_mutex_;
    Phase phase_ = Phase::Setup;
    bool producer_succeeded_ = false;
};

// fuse_main stops callback dispatch before returning. Its stack-owned image
// consequently outlives every callback, including exceptions translated here.
template <typename Function>
auto FuseCallback(Function function) noexcept -> int {
    auto &image = *static_cast<FuseImage *>(fuse_get_context()->private_data);
    try {
        return function(image);
    } catch (const std::exception &error) {
        std::println(std::cerr, "macFUSE callback: {}", error.what());
    } catch (...) {
        std::println(std::cerr, "macFUSE callback: unexpected exception");
    }
    image.Fail();
    return -EIO;
}

const auto kFuseOperations = [] {
    auto operations = fuse_operations{};
    operations.getattr = [](const char *path, struct stat *output) -> int {
        return FuseCallback([path = std::string_view{path}, output](auto &image) {
            return image.Attributes(path, *output);
        });
    };
    operations.truncate = [](const char *path, const off_t size) -> int {
        return FuseCallback([path = std::string_view{path}, size](auto &image) {
            return image.Truncate(path, size);
        });
    };
    operations.setattr_x = [](const char *path, struct setattr_x *attributes) -> int {
        return FuseCallback([path = std::string_view{path}, attributes](auto &image) {
            std::println(std::cerr, "Acknowledging metadata update for '{}' (fields {:#x}).",
                path, attributes->valid);
            return SETATTR_WANTS_SIZE(attributes) ?
                image.Truncate(path, attributes->size) : 0;
        });
    };
    operations.open = [](const char *path, fuse_file_info *information) -> int {
        if (std::string_view{path} == "/marker") {
            information->direct_io = 1;
            return 0;
        }
        return std::string_view{path} == "/image.raw" ? 0 : -ENOENT;
    };
    operations.read = [](const char *path, char *buffer, const std::size_t size,
        const off_t offset, fuse_file_info *) -> int {
        return FuseCallback([is_image = std::string_view{path} == "/image.raw",
            buffer, size, offset](auto &image) {
            return is_image ? image.Read(offset, std::span{buffer, size}) : -EACCES;
        });
    };
    operations.write = [](const char *path, const char *buffer, const std::size_t size,
        const off_t offset, fuse_file_info *) -> int {
        return FuseCallback([path = std::string_view{path}, buffer,
            size, offset](auto &image) {
            if (path == "/marker") {
                return image.Mark({buffer, size});
            }
            return path == "/image.raw" ? image.Write(offset, {buffer, size}) : -ENOENT;
        });
    };
    operations.statfs = [](const char *, struct statvfs *output) -> int {
        return FuseCallback([output](auto &image) { return image.Statistics(*output); });
    };
    operations.fsync = [](const char *path, int, fuse_file_info *) -> int {
        return FuseCallback([is_image = std::string_view{path} == "/image.raw"](auto &image) {
            return is_image ? image.Synchronize() : 0;
        });
    };
    operations.readdir = [](const char *path, void *buffer, fuse_fill_dir_t fill,
        off_t, fuse_file_info *) -> int {
        if (std::string_view{path} != "/") {
            return -ENOTDIR;
        }
        for (const auto *const name : {".", "..", "image.raw", "marker"}) {
            if (fill(buffer, name, nullptr, 0) != 0) {
                break;
            }
        }
        return 0;
    };
    return operations;
}();

auto MountImage(std::string executable, const std::filesystem::path &seed,
    const std::filesystem::path &trace, const std::filesystem::path &mount) -> int {
    auto image = FuseImage{seed, trace};
    auto mount_text = mount.string();
    auto foreground = std::string{"-f"};
    auto option = std::string{"-o"};
    // noubc disables macFUSE's unified buffer cache. The trace still observes
    // callbacks after any buffering or scheduling performed by DiskImages/asr.
    // https://github.com/macfuse/macfuse/wiki/Mount-Options#noubc
    auto options = std::string{"default_permissions,noubc,debug"};
    auto arguments = std::to_array({executable.data(), foreground.data(), option.data(),
        options.data(), mount_text.data(), {}});
    return image.Finish(fuse_main(arguments.size() - 1,
        arguments.data(), &kFuseOperations, &image));
}

auto CreateSnapshot(const std::filesystem::path &volume, const std::string &name) -> int {
    const auto directory = OpenFile(volume, std::ios::in);
    if (::fs_snapshot_create(directory.native_handle(), name.c_str(), 0) < 0) {
        throw std::system_error{errno, std::generic_category(),
            std::format("create APFS snapshot '{}' on '{}'", name, volume.string())};
    }
    return 0;
}

auto NativeExecutable() -> std::filesystem::path {
    auto length = std::uint32_t{};
    _NSGetExecutablePath(nullptr, &length);
    auto buffer = std::vector<char>(length);
    if (_NSGetExecutablePath(buffer.data(), &length) != 0) {
        throw std::runtime_error{"could not obtain native executable path"};
    }
    return std::filesystem::canonical(buffer.data());
}

template <WritingPolicy Policy>
auto RunMountedSynthetic(const std::filesystem::path &image_path) -> int {
    auto filesystem = MountedImageFilesystem{image_path};
    auto producer = SyntheticProducer<Policy>{};
    producer.Produce(filesystem);
    return 0;
}

}

export namespace devicefs::asr_trace {

auto RunMac(const std::span<const char *const> arguments) -> int {
    if ((arguments.size() == 2) && (std::string_view{arguments[1]} == "--help")) {
        std::println("Usage: asr-image-trace --output-root DIRECTORY "
            "[--producer asr|serial|jump]\n"
            "       asr-image-trace --output-root DIRECTORY "
            "--source-volume PATH --snapshot NAME_OR_UUID\n"
            "Create the disposable fixture and trace writes through a macFUSE discard sink.");
        return 0;
    }
    if ((arguments.size() == 4) && (std::string_view{arguments[1]} == "--create-snapshot")) {
        return CreateSnapshot(arguments[2], arguments[3]);
    }
    if ((arguments.size() == 5) && (std::string_view{arguments[1]} == "--mount")) {
        return MountImage(arguments[0], arguments[2], arguments[3], arguments[4]);
    }
    if ((arguments.size() == 4) && (std::string_view{arguments[1]} == "--synthetic")) {
        const auto policy = std::string_view{arguments[2]};
        if (policy == "serial") {
            return RunMountedSynthetic<SerialWriting>(arguments[3]);
        }
        if (policy == "jump") {
            return RunMountedSynthetic<JumpAroundWriting>(arguments[3]);
        }
        throw std::invalid_argument{"synthetic policy must be serial or jump"};
    }
    auto root = std::filesystem::path{};
    auto producer = std::string{"asr"};
    auto source = std::string{};
    auto snapshot = std::string{};
    for (auto index = 1uz; index < arguments.size(); ++index) {
        const auto option = std::string_view{arguments[index]};
        if (((option != "--output-root") && (option != "--producer") &&
                (option != "--source-volume") && (option != "--snapshot")) ||
            (++index == arguments.size())) {
            throw std::invalid_argument{
                std::format("invalid or incomplete option '{}'", option)};
        }
        if (option == "--output-root") {
            root = arguments[index];
        } else if (option == "--producer") {
            producer = arguments[index];
        } else if (option == "--source-volume") {
            source = arguments[index];
        } else {
            snapshot = arguments[index];
        }
    }
    if (root.empty()) {
        throw std::invalid_argument{"--output-root is required"};
    }
    if ((producer != "asr") && (producer != "serial") && (producer != "jump")) {
        throw std::invalid_argument{"--producer must be asr, serial or jump"};
    }
    if (source.empty() != snapshot.empty()) {
        throw std::invalid_argument{
            "--source-volume and --snapshot must be supplied together"};
    }
    if (!source.empty() && (producer != "asr")) {
        throw std::invalid_argument{"--source-volume applies only to the ASR producer"};
    }
    const auto executable_path = NativeExecutable();
    const auto script = executable_path.parent_path().parent_path() / "Resources/macos.fish";
    const auto executable = executable_path.string();
    const auto output = std::filesystem::absolute(root).string();
    // Replacing this entry process gives Fish sole ownership of child waiting,
    // signals, primary failure, and cleanup; there is no second supervisor.
    ::execl(DEVICEFS_ASR_FISH_EXECUTABLE, "fish", "--no-config", script.c_str(), "--",
        executable.c_str(), output.c_str(), producer.c_str(), source.c_str(),
        snapshot.c_str(), DEVICEFS_ASR_JQ_EXECUTABLE, nullptr);
    throw std::system_error{errno, std::generic_category(),
        "start Fish experiment controller"};
}

}
