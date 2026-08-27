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

module;

#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <virtdisk.h>

#include <devicefs/strsafe_compat.h>

export module devicefs.supervisor.native_backup:filesystem_verifier;

import std;
import <devicefs/windows_imports.h>;
import <wil/filesystem.h>;
import :devicefs_process;
import :internal;
import :privileges;
import devicefs.common;
import devicefs.stream_writer;

export struct FilesystemVerificationVolume {
    GUID volume_identifier;
    GUID payload_snapshot_identifier;
    std::wstring filename;
};

namespace {

using namespace std::chrono_literals;

// Each volume owns its worker pools. Four workers keep independent operations
// in flight without allowing one volume to consume another drive's workers.
constexpr auto kVerificationWorkerCount = 4uz;
constexpr auto kReadChunkSize = 4uz * 1024 * 1024;
constexpr auto kProgressReportInterval = 1min;
constexpr auto kReporterPollInterval = 100ms;
constexpr auto kShareMode =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

enum class VerificationEndpoint {
    Synthetic,
    Real,
};

enum class VerificationPhase {
    AttachingSynthetic,
    AttachingReal,
    Traversing,
    Planning,
    Comparing,
    Detaching,
    Complete,
};

enum class ObjectKind {
    File,
    Directory,
    FileReparsePoint,
    DirectoryReparsePoint,
};

enum class MismatchKind {
    ObjectMissingFromSynthetic,
    ObjectMissingFromReal,
    ObjectType,
    StreamMissingFromSynthetic,
    StreamMissingFromReal,
    StreamLength,
    StreamContents,
};

enum class FilesystemOperation {
    QueryDirectory,
    OpenObject,
    QueryStreams,
    OpenStream,
    SeekStream,
    ReadStream,
};

[[nodiscard]] constexpr auto IsVssExcludedPath(
    const std::wstring_view path) noexcept {
    constexpr auto directory =
        std::wstring_view{L"System Volume Information"};
    return (path == directory) ||
        (path.starts_with(L"System Volume Information\\"));
}

using FileIdentity = std::uint64_t;

struct StreamRecord {
    std::wstring name;
    std::int64_t length;

    bool operator==(const StreamRecord &) const = default;
};

struct ObjectRecord {
    std::wstring relative_path;
    ObjectKind kind;
    FileIdentity identity;
    std::vector<StreamRecord> streams;
    bool streams_complete = false;
};

struct OperationFailure {
    DWORD error;
    DWORD transferred = 0;

    auto operator<=>(const OperationFailure &) const = default;
};

struct TraversalTask {
    std::wstring relative_path;
    ObjectKind kind;
    std::optional<FileIdentity> identity;
};

struct OperationIssueKey {
    std::wstring path;
    std::wstring stream;
    FilesystemOperation operation;
    std::int64_t offset = 0;

    auto operator<=>(const OperationIssueKey &) const = default;
};

struct OperationComparison {
    OperationIssueKey key;
    DWORD requested = 0;
    std::optional<OperationFailure> synthetic;
    std::optional<OperationFailure> real;

    auto operator<=>(const OperationComparison &) const = default;
};

struct InventoryIssue {
    OperationIssueKey key;
    OperationFailure failure;

    auto operator<=>(const InventoryIssue &) const = default;
};

[[nodiscard]] auto FailuresMatch(
    const std::optional<OperationFailure> &synthetic,
    const std::optional<OperationFailure> &real) noexcept {
    return synthetic && real && (*synthetic == *real);
}

[[nodiscard]] auto IsMatchedError(
    const OperationComparison &comparison) noexcept {
    return FailuresMatch(comparison.synthetic, comparison.real);
}

[[nodiscard]] auto IsOperationMismatch(
    const OperationComparison &comparison) noexcept {
    return !IsVssExcludedPath(comparison.key.path) &&
        !IsMatchedError(comparison);
}

[[nodiscard]] auto IsContentOperationMismatch(
    const OperationComparison &comparison) noexcept {
    return IsOperationMismatch(comparison) &&
        ((comparison.key.operation == FilesystemOperation::OpenStream) ||
            (comparison.key.operation == FilesystemOperation::SeekStream) ||
            (comparison.key.operation == FilesystemOperation::ReadStream));
}

[[nodiscard]] auto IsVssExcludedDifference(
    const OperationComparison &comparison) noexcept {
    return !IsMatchedError(comparison) &&
        IsVssExcludedPath(comparison.key.path);
}

struct FilesystemInventory {
    std::filesystem::path root;
    std::vector<ObjectRecord> objects;
    std::vector<InventoryIssue> issues;
    std::vector<std::wstring> incomplete_directories;
};

struct ReadTask {
    std::size_t synthetic_object;
    std::size_t real_object;
    std::size_t synthetic_stream;
    std::size_t real_stream;
    std::int64_t offset;
    std::size_t length;
};

struct ComparisonPlan {
    std::uint64_t available_bytes;
    std::uint64_t selected_bytes;
    std::vector<ReadTask> tasks;
};

struct PairedFileIdentity {
    FileIdentity synthetic;
    FileIdentity real;

    auto operator<=>(const PairedFileIdentity &) const = default;
};

struct MismatchKey {
    std::wstring path;
    std::wstring stream;

    auto operator<=>(const MismatchKey &) const = default;
};

struct MismatchDetail {
    MismatchKind kind;
    std::wstring path;
    std::wstring stream;
    ObjectKind synthetic_kind = ObjectKind::File;
    ObjectKind real_kind = ObjectKind::File;
    std::int64_t synthetic_length = 0;
    std::int64_t real_length = 0;
    std::int64_t first_offset = 0;
    unsigned char synthetic_byte = 0;
    unsigned char real_byte = 0;
    std::uint64_t differing_bytes = 0;
    std::uint64_t differing_chunks = 0;
};

struct InventoryObservation {
    std::uint64_t tasks_discovered = 0;
    std::uint64_t tasks_completed = 0;
    std::uint64_t directories = 0;
    std::uint64_t objects = 0;
    std::uint64_t streams = 0;
    std::uint64_t stream_bytes = 0;
    std::uint64_t failures = 0;
};

[[nodiscard]] constexpr auto SaturatingSum(
    const std::uint64_t left,
    const std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max()
        : left + right;
}

[[nodiscard]] constexpr auto SaturationPrefix(
    const std::uint64_t value) noexcept {
    return value == std::numeric_limits<std::uint64_t>::max()
        ? std::string_view{"at least "}
        : std::string_view{};
}

struct VerificationObservation {
    VerificationPhase phase = VerificationPhase::AttachingSynthetic;
    InventoryObservation synthetic;
    InventoryObservation real;
    std::uint64_t available_bytes = 0;
    std::uint64_t selected_bytes = 0;
    std::uint64_t compared_bytes = 0;
    std::optional<std::chrono::steady_clock::time_point>
        traversal_started;
    std::optional<std::chrono::steady_clock::time_point>
        comparison_started;
    std::size_t mismatch_count = 0;
    std::size_t exclusion_count = 0;
    std::size_t matched_error_count = 0;
    bool traversal_complete = false;
    bool plan_ready = false;
    bool comparison_complete = false;
    bool failed = false;
    std::string failure;
    std::vector<std::string> cleanup_failures;
};

class VerificationFailure final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] auto IsCancellationError(
    const std::system_error &error) noexcept {
    return (error.code() == std::error_code{
        ERROR_CANCELLED, std::system_category()}) ||
        (error.code() == std::error_code{
            ERROR_OPERATION_ABORTED, std::system_category()});
}

[[nodiscard]] constexpr auto IsCancellationError(
    const DWORD error) noexcept {
    return (error == ERROR_CANCELLED) ||
        (error == ERROR_OPERATION_ABORTED);
}

class SynchronousIoCancellation {
  public:
    class Registration {
      public:
        Registration(
            SynchronousIoCancellation &owner,
            wil::unique_handle thread)
            : owner_{owner}, thread_{std::move(thread)} {
            const auto lock = owner_.lock_.lock_exclusive();
            owner_.threads_.push_back(thread_.get());
        }

        Registration(const Registration &) = delete;
        auto operator=(const Registration &)
            -> Registration & = delete;
        Registration(Registration &&) = delete;
        auto operator=(Registration &&)
            -> Registration & = delete;

        ~Registration() {
            Unregister();
        }

        [[gsl::suppress("26447",
            justification:
                "Erasing a HANDLE performs only nonthrowing HANDLE "
                "comparisons, assignments, and destruction.")]]
        auto Unregister() noexcept -> void {
            if (!thread_) {
                return;
            }
            const auto lock = owner_.lock_.lock_exclusive();
            std::erase(owner_.threads_, thread_.get());
            thread_.reset();
        }

      private:
        SynchronousIoCancellation &owner_;
        wil::unique_handle thread_;
    };

    [[nodiscard]] auto RegisterCurrentThread() {
        // CancelSynchronousIo requires a thread handle with
        // THREAD_TERMINATE access.
        auto thread = wil::unique_handle{OpenThread(
            THREAD_TERMINATE, FALSE, GetCurrentThreadId())};
        if (!thread) {
            WinError(
                "could not register filesystem-verification I/O");
        }
        return Registration{*this, std::move(thread)};
    }

    [[nodiscard]] auto CancelPending() noexcept {
        // Retain the registry lock so a Registration cannot close a thread
        // handle while CancelSynchronousIo is using it.
        const auto lock = lock_.lock_exclusive();
        auto result = DWORD{ERROR_SUCCESS};
        for (const auto thread : threads_) {
            if (CancelSynchronousIo(thread)) {
                continue;
            }
            const auto error = GetLastError();
            if ((error != ERROR_NOT_FOUND) &&
                (result == ERROR_SUCCESS)) {
                result = error;
            }
        }
        return result;
    }

  private:
    wil::srwlock lock_;
    std::vector<HANDLE> threads_;
};

auto RequestPendingIoCancellation(
    SynchronousIoCancellation &io_cancellation,
    DWORD &first_failure) noexcept -> void {
    const auto error = io_cancellation.CancelPending();
    if ((error == ERROR_SUCCESS) ||
        (first_failure != ERROR_SUCCESS)) {
        return;
    }
    first_failure = error;
    devicefs::WriteToStream(
        std::cout,
        "Could not interrupt pending filesystem-verification I/O "
        "(Windows error {}).\n",
        error);
}

class VerificationState {
  public:
    auto BeginTraversal() -> void {
        {
            const auto lock = std::scoped_lock{mutex_};
            traversal_started_ = std::chrono::steady_clock::now();
        }
        SetPhase(VerificationPhase::Traversing);
    }

    auto RecordTraversalTasks(
        const VerificationEndpoint endpoint,
        const std::size_t count) noexcept -> void {
        Inventory(endpoint).tasks_discovered.fetch_add(
            count, std::memory_order_relaxed);
    }

    auto SetPhase(const VerificationPhase phase) noexcept -> void {
        phase_.store(phase, std::memory_order_release);
    }

    auto RecordInventory(
        const VerificationEndpoint endpoint,
        const std::size_t stream_count,
        const std::uint64_t stream_bytes) noexcept -> void {
        auto &inventory = Inventory(endpoint);
        inventory.objects.fetch_add(1, std::memory_order_relaxed);
        inventory.streams.fetch_add(
            stream_count, std::memory_order_relaxed);
        AddSaturating(inventory.stream_bytes, stream_bytes);
    }

    auto RecordTraversalTaskCompleted(
        const VerificationEndpoint endpoint) noexcept -> void {
        Inventory(endpoint).tasks_completed.fetch_add(
            1, std::memory_order_relaxed);
    }

    auto RecordDirectory(
        const VerificationEndpoint endpoint) noexcept -> void {
        Inventory(endpoint).directories.fetch_add(
            1, std::memory_order_relaxed);
    }

    auto RecordInventoryFailure(
        const VerificationEndpoint endpoint) noexcept -> void {
        Inventory(endpoint).failures.fetch_add(
            1, std::memory_order_relaxed);
    }

    auto CompleteTraversal() noexcept -> void {
        traversal_complete_.store(true, std::memory_order_release);
    }

    auto SetPlan(const ComparisonPlan &plan) noexcept -> void {
        available_bytes_.store(
            plan.available_bytes, std::memory_order_relaxed);
        selected_bytes_.store(
            plan.selected_bytes, std::memory_order_relaxed);
        plan_ready_.store(true, std::memory_order_release);
    }

    auto RecordCompared(const std::size_t bytes) noexcept -> void {
        compared_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }

    auto CompleteComparison() noexcept -> void {
        comparison_complete_.store(true, std::memory_order_release);
    }

    auto BeginComparison() -> void {
        {
            const auto lock = std::scoped_lock{mutex_};
            comparison_started_ = std::chrono::steady_clock::now();
        }
        SetPhase(VerificationPhase::Comparing);
    }

    [[nodiscard]] auto Failed() const noexcept {
        return failed_.load(std::memory_order_acquire);
    }

    auto Fail(std::string failure) -> void {
        const auto lock = std::scoped_lock{mutex_};
        if (failed_.load(std::memory_order_relaxed)) {
            return;
        }
        failure_ = std::move(failure);
        failed_.store(true, std::memory_order_release);
    }

    auto RecordMismatch(MismatchDetail mismatch) -> void {
        const auto lock = std::scoped_lock{mutex_};
        mismatches_.push_back(std::move(mismatch));
    }

    auto RecordOperationComparison(
        OperationIssueKey key,
        const DWORD requested,
        std::optional<OperationFailure> synthetic,
        std::optional<OperationFailure> real) -> void {
        const auto lock = std::scoped_lock{mutex_};
        auto comparison = OperationComparison{
            .key = std::move(key),
            .requested = requested,
            .synthetic = std::move(synthetic),
            .real = std::move(real),
        };
        if (!operation_comparisons_.insert(comparison).second) {
            return;
        }
        operation_comparison_records_.push_back(std::move(comparison));
    }

    auto RecordContentMismatch(
        const std::wstring &path,
        const std::wstring &stream,
        const std::int64_t first_offset,
        const unsigned char synthetic_byte,
        const unsigned char real_byte,
        const std::uint64_t differing_bytes) -> void {
        const auto lock = std::scoped_lock{mutex_};
        const auto key = MismatchKey{path, stream};
        auto found = content_mismatches_.find(key);
        if (found == content_mismatches_.end()) {
            const auto index = mismatches_.size();
            mismatches_.push_back(MismatchDetail{
                .kind = MismatchKind::StreamContents,
                .path = path,
                .stream = stream,
                .first_offset = first_offset,
                .synthetic_byte = synthetic_byte,
                .real_byte = real_byte,
            });
            found = content_mismatches_.emplace(key, index).first;
        }
        auto &mismatch = mismatches_[found->second];
        if (first_offset < mismatch.first_offset) {
            mismatch.first_offset = first_offset;
            mismatch.synthetic_byte = synthetic_byte;
            mismatch.real_byte = real_byte;
        }
        mismatch.differing_bytes += differing_bytes;
        ++mismatch.differing_chunks;
    }

    auto RecordCleanupFailure(std::string failure) -> void {
        const auto lock = std::scoped_lock{mutex_};
        cleanup_failures_.push_back(std::move(failure));
    }

    [[nodiscard]] auto NewMismatches(
        std::size_t &cursor) const -> std::vector<MismatchDetail> {
        const auto lock = std::scoped_lock{mutex_};
        auto result = std::vector<MismatchDetail>{
            mismatches_.begin() +
                wil::safe_cast_failfast<std::ptrdiff_t>(cursor),
            mismatches_.end()};
        cursor = mismatches_.size();
        return result;
    }

    [[nodiscard]] auto NewOperationComparisons(
        std::size_t &cursor) const -> std::vector<OperationComparison> {
        const auto lock = std::scoped_lock{mutex_};
        auto result = std::vector<OperationComparison>{
            operation_comparison_records_.begin() +
                wil::safe_cast_failfast<std::ptrdiff_t>(cursor),
            operation_comparison_records_.end()};
        cursor = operation_comparison_records_.size();
        return result;
    }

    // Inventory and comparison work have finished before either unguarded
    // view is used.
    [[nodiscard]] auto MismatchesAfterJoin() const noexcept
        -> const std::vector<MismatchDetail> & {
        return mismatches_;
    }

    [[nodiscard]] auto OperationComparisonsAfterJoin() const noexcept
        -> const std::vector<OperationComparison> & {
        return operation_comparison_records_;
    }

    [[nodiscard]] auto Observe() const -> VerificationObservation {
        const auto plan_ready =
            plan_ready_.load(std::memory_order_acquire);
        const auto comparison_complete =
            comparison_complete_.load(std::memory_order_acquire);
        auto result = VerificationObservation{
            .phase = phase_.load(std::memory_order_acquire),
            .synthetic = ObserveInventory(synthetic_inventory_),
            .real = ObserveInventory(real_inventory_),
            .available_bytes =
                available_bytes_.load(std::memory_order_relaxed),
            .selected_bytes =
                selected_bytes_.load(std::memory_order_relaxed),
            .compared_bytes =
                compared_bytes_.load(std::memory_order_relaxed),
            .traversal_complete = traversal_complete_.load(
                std::memory_order_acquire),
            .plan_ready = plan_ready,
            .comparison_complete = comparison_complete,
        };
        const auto lock = std::scoped_lock{mutex_};
        result.traversal_started = traversal_started_;
        result.comparison_started = comparison_started_;
        result.mismatch_count = std::ranges::count_if(
            mismatches_, [](const MismatchDetail &mismatch) {
                return !IsVssExcludedPath(mismatch.path);
            }) +
            std::ranges::count_if(operation_comparison_records_,
                IsOperationMismatch);
        result.exclusion_count = std::ranges::count_if(
            mismatches_, [](const MismatchDetail &mismatch) {
                return IsVssExcludedPath(mismatch.path);
            }) +
            std::ranges::count_if(operation_comparison_records_,
                IsVssExcludedDifference);
        result.matched_error_count = std::ranges::count_if(
            operation_comparison_records_, IsMatchedError);
        result.failed = failed_.load(std::memory_order_acquire);
        result.failure = failure_;
        result.cleanup_failures = cleanup_failures_;
        return result;
    }

  private:
    static auto AddSaturating(
        std::atomic<std::uint64_t> &value,
        const std::uint64_t addend) noexcept -> void {
        if (addend == 0) {
            return;
        }
        auto current = value.load(std::memory_order_relaxed);
        while ((current != std::numeric_limits<std::uint64_t>::max()) &&
            !value.compare_exchange_weak(current,
                SaturatingSum(current, addend),
                std::memory_order_relaxed)) {}
    }

    struct InventoryCounters {
        std::atomic<std::uint64_t> tasks_discovered{};
        std::atomic<std::uint64_t> tasks_completed{};
        std::atomic<std::uint64_t> directories{};
        std::atomic<std::uint64_t> objects{};
        std::atomic<std::uint64_t> streams{};
        std::atomic<std::uint64_t> stream_bytes{};
        std::atomic<std::uint64_t> failures{};
    };

    [[nodiscard]] auto Inventory(
        const VerificationEndpoint endpoint) noexcept
        -> InventoryCounters & {
        return endpoint == VerificationEndpoint::Synthetic
            ? synthetic_inventory_ : real_inventory_;
    }

    [[nodiscard]] static auto ObserveInventory(
        const InventoryCounters &inventory) noexcept {
        return InventoryObservation{
            .tasks_discovered = inventory.tasks_discovered.load(
                std::memory_order_relaxed),
            .tasks_completed = inventory.tasks_completed.load(
                std::memory_order_relaxed),
            .directories = inventory.directories.load(
                std::memory_order_relaxed),
            .objects = inventory.objects.load(
                std::memory_order_relaxed),
            .streams = inventory.streams.load(
                std::memory_order_relaxed),
            .stream_bytes = inventory.stream_bytes.load(
                std::memory_order_relaxed),
            .failures = inventory.failures.load(
                std::memory_order_relaxed),
        };
    }

    std::atomic<VerificationPhase> phase_{
        VerificationPhase::AttachingSynthetic};
    InventoryCounters synthetic_inventory_;
    InventoryCounters real_inventory_;
    std::atomic<std::uint64_t> available_bytes_{};
    std::atomic<std::uint64_t> selected_bytes_{};
    std::atomic<std::uint64_t> compared_bytes_{};
    std::atomic<bool> traversal_complete_{};
    std::atomic<bool> plan_ready_{};
    std::atomic<bool> comparison_complete_{};
    std::atomic<bool> failed_{};
    mutable std::mutex mutex_;
    std::string failure_;
    std::optional<std::chrono::steady_clock::time_point>
        traversal_started_;
    std::optional<std::chrono::steady_clock::time_point>
        comparison_started_;
    std::vector<std::string> cleanup_failures_;
    std::vector<MismatchDetail> mismatches_;
    std::map<MismatchKey, std::size_t> content_mismatches_;
    std::set<OperationComparison> operation_comparisons_;
    std::vector<OperationComparison> operation_comparison_records_;
};

[[nodiscard]] auto QueryPhysicalDiskPath(const HANDLE disk) {
    auto bytes = ULONG{};
    const auto query = GetVirtualDiskPhysicalPath(
        disk, &bytes, nullptr);
    if (query != ERROR_INSUFFICIENT_BUFFER) {
        WinError("could not size the VHDX physical path",
            ExplicitWin32Error{query});
    }
    [[gsl::suppress("26493",
        justification:
            "Braced initialization proves this construction safe at compile time.")]]
    auto path = std::vector<wchar_t>(
        (std::size_t{bytes} + sizeof(wchar_t) - 1) /
            sizeof(wchar_t));
    const auto status = GetVirtualDiskPhysicalPath(
        disk, &bytes, path.data());
    if (status != ERROR_SUCCESS) {
        WinError("could not obtain the VHDX physical path",
            ExplicitWin32Error{status});
    }
    return std::wstring{path.data()};
}

[[nodiscard]] auto OpenPhysicalDisk(const std::wstring &path) {
    auto result = wil::unique_hfile{CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!result) {
        WinError("could not open the attached VHDX physical disk");
    }
    return result;
}

[[nodiscard]] auto QueryDiskNumber(const HANDLE disk) {
    auto number = STORAGE_DEVICE_NUMBER{};
    auto returned = DWORD{};
    if (!DeviceIoControl(disk, IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr, 0, &number, sizeof(number), &returned, nullptr)) {
        WinError("could not identify the attached VHDX physical disk");
    }
    return number.DeviceNumber;
}

[[nodiscard]] auto QueryVolumeRoot(
    const DWORD disk_number,
    const HANDLE cancellation_event) {
    // VhdxViewer presents exactly one GPT partition. Open that known root and
    // ask Windows for the volume-GUID path of the filesystem mounted there.
    const auto partition_root = std::format(
        LR"(\\?\GLOBALROOT\Device\Harddisk{}\Partition1\)",
        disk_number);
    const auto partition_root_name = wil::zwstring_view{partition_root};
    devicefs::WriteToStream(
        std::cout,
        L"  Partition root: {}\n"
        L"  Opening its filesystem root.\n",
        partition_root);
    auto partition = [&] {
        constexpr auto retry_interval = 100ms;
        constexpr auto retry_period = 10s;
        const auto retry_deadline =
            std::chrono::steady_clock::now() + retry_period;
        while (true) {
            auto result = wil::unique_hfile{CreateFileW(
                partition_root_name.c_str(), 0,
                kShareMode, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
            if (result) {
                return result;
            }
            const auto error = GetLastError();
            if (std::chrono::steady_clock::now() >= retry_deadline) {
                WinError("could not open attached VHDX Partition1 root {}",
                    std::filesystem::path{partition_root}.string(),
                    ExplicitWin32Error{error});
            }
            const auto wait = WaitForSingleObject(
                cancellation_event,
                wil::safe_cast_failfast<DWORD>(
                    retry_interval.count()));
            if (wait == WAIT_FAILED) {
                WinError("could not wait for the attached filesystem");
            }
            if (wait == WAIT_OBJECT_0) {
                WinError("waiting for the attached filesystem was cancelled",
                    ExplicitWin32Error{ERROR_CANCELLED});
            }
        }
    }();

    devicefs::WriteToStream(std::cout,
        "  Filesystem root opened.\n"
        "  Querying its volume-GUID name.\n");
    auto volume_root = wil::unique_cotaskmem_string{};
    const auto error = wil::GetFinalPathNameByHandleW(
        partition.get(), volume_root, wil::VolumePrefix::VolumeGuid);
    if (FAILED(error)) {
        WinError("could not obtain the attached VHDX volume name",
            ExplicitWin32Error::FromHresult(error));
    }
    return std::wstring{volume_root.get()};
}

[[nodiscard]] auto AttachVirtualDiskCancellable(
    const HANDLE disk,
    const HANDLE cancellation_event) {
    // Use the virtual-disk API's overlapped form so Ctrl+C can cancel a
    // pending attachment without waiting for its ordinary completion. The
    // OVERLAPPED and disk handle must remain alive until cancellation itself
    // completes, so the completion event is still awaited after CancelIoEx.
    auto completion_event = wil::unique_event_nothrow{};
    if (!completion_event.try_create(
            wil::EventOptions::ManualReset, nullptr)) {
        WinError("could not create the VHDX attachment event");
    }
    auto operation = OVERLAPPED{
        .hEvent = completion_event.get(),
    };
    auto parameters = ATTACH_VIRTUAL_DISK_PARAMETERS{
        .Version = ATTACH_VIRTUAL_DISK_VERSION_1,
    };
    const auto status = AttachVirtualDisk(
        disk, nullptr,
        ATTACH_VIRTUAL_DISK_FLAG{
            ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY |
            ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER},
        0, &parameters, &operation);
    if (status != ERROR_IO_PENDING) {
        return status;
    }

    const auto events = std::array{
        completion_event.get(), cancellation_event,
    };
    const auto wait = WaitForMultipleObjects(
        wil::safe_cast_failfast<DWORD>(events.size()),
        events.data(), FALSE, INFINITE);
    if (wait == WAIT_FAILED) {
        WinError("could not wait for VHDX attachment");
    }
    const auto cancelled = wait == (WAIT_OBJECT_0 + 1);
    if (cancelled) {
        devicefs::WriteToStream(
            std::cout,
            "Cancellation requested while VHDX attachment was pending; "
            "requesting cancellation of that attachment.\n");
        if (!CancelIoEx(disk, &operation)) {
            const auto error = GetLastError();
            if (error != ERROR_NOT_FOUND) {
                devicefs::WriteToStream(std::cout,
                    "Could not cancel the pending VHDX attachment "
                    "(Windows error {}); waiting for it to finish.\n",
                    error);
            }
        }
        if (WaitForSingleObject(completion_event.get(), INFINITE) ==
            WAIT_FAILED) {
            WinError("could not wait for VHDX attachment cancellation");
        }
    }

    auto progress = VIRTUAL_DISK_PROGRESS{};
    const auto progress_status = GetVirtualDiskOperationProgress(
        disk, &operation, &progress);
    if (progress_status != ERROR_SUCCESS) {
        return progress_status;
    }
    if (cancelled &&
        (progress.OperationStatus != ERROR_SUCCESS)) {
        return DWORD{ERROR_CANCELLED};
    }
    return progress.OperationStatus;
}

class AttachedVhdx {
  public:
    [[nodiscard]] static auto Attach(
        const std::filesystem::path &path,
        const std::string_view name,
        const HANDLE cancellation_event,
        wil::srwlock &virtual_disk_lock,
        SynchronousIoCancellation &io_cancellation) {
        try {
            const auto lock = virtual_disk_lock.lock_exclusive();
            if (internal::CancellationRequested(cancellation_event)) {
                WinError("VHDX attachment was cancelled",
                    ExplicitWin32Error{ERROR_CANCELLED});
            }
            devicefs::WriteToStream(
                std::cout,
                "\nPreparing the {} VHDX attachment:\n", name);
            devicefs::WriteToStream(
                std::cout,
                L"  File: {}\n"
                L"  Opening the VHDX.\n",
                path.native());
            auto storage_type = VIRTUAL_STORAGE_TYPE{
                .DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX,
                .VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT,
            };
            auto parameters = OPEN_VIRTUAL_DISK_PARAMETERS{
                .Version = OPEN_VIRTUAL_DISK_VERSION_2,
                .Version2 = {
                    .ReadOnly = TRUE,
                },
            };
            auto disk = wil::unique_handle{};
            auto registration =
                io_cancellation.RegisterCurrentThread();
            const auto open_status = OpenVirtualDisk(
                &storage_type, path.c_str(), VIRTUAL_DISK_ACCESS_NONE,
                OPEN_VIRTUAL_DISK_FLAG{
                    OPEN_VIRTUAL_DISK_FLAG_CACHED_IO |
                    OPEN_VIRTUAL_DISK_FLAG_SUPPORT_COMPRESSED_VOLUMES |
                    OPEN_VIRTUAL_DISK_FLAG_SUPPORT_SPARSE_FILES_ANY_FS |
                    OPEN_VIRTUAL_DISK_FLAG_SUPPORT_ENCRYPTED_FILES},
                &parameters, disk.addressof());
            if (open_status != ERROR_SUCCESS) {
                WinError("could not open filesystem-verification VHDX {}",
                    path.string(), ExplicitWin32Error{open_status});
            }
            devicefs::WriteToStream(
                std::cout, "  VHDX opened.\n  Attaching the VHDX.\n");
            if (internal::CancellationRequested(cancellation_event)) {
                WinError("VHDX attachment was cancelled",
                    ExplicitWin32Error{ERROR_CANCELLED});
            }
            const auto attach_status = AttachVirtualDiskCancellable(
                disk.get(), cancellation_event);
            if (attach_status != ERROR_SUCCESS) {
                WinError("could not attach a filesystem-verification VHDX",
                    ExplicitWin32Error{attach_status});
            }
            if (internal::CancellationRequested(cancellation_event)) {
                // Cancellation may interrupt preparation, but must not
                // interrupt the cleanup it caused.
                registration.Unregister();
                const auto detach_status = DetachVirtualDisk(
                    disk.get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
                if (detach_status != ERROR_SUCCESS) {
                    devicefs::WriteToStream(std::cout,
                        "  Cancellation arrived as the VHDX attachment "
                        "completed, but detaching it failed with Windows "
                        "error {}. Closing its nonpermanent attachment "
                        "handle.\n",
                        detach_status);
                }
                disk.reset();
                WinError("VHDX attachment was cancelled",
                    ExplicitWin32Error{ERROR_CANCELLED});
            }
            devicefs::WriteToStream(std::cout,
                "  VHDX attached.\n"
                "  Querying its physical-disk path.\n");
            const auto physical_path = QueryPhysicalDiskPath(disk.get());
            devicefs::WriteToStream(std::cout,
                L"  Attached physical disk: {}\n"
                L"  Opening that physical disk.\n",
                physical_path);
            auto physical_disk = OpenPhysicalDisk(physical_path);
            devicefs::WriteToStream(std::cout,
                "  Physical disk opened.\n"
                "  Querying its disk number.\n");
            const auto disk_number = QueryDiskNumber(physical_disk.get());
            devicefs::WriteToStream(
                std::cout,
                "  Disk number: {}\n",
                disk_number);
            auto root = QueryVolumeRoot(
                disk_number, cancellation_event);
            devicefs::WriteToStream(
                std::cout, L"  Attached volume: {}\n", root);
            return AttachedVhdx{
                std::move(disk), std::move(root), virtual_disk_lock};
        } catch (const VerificationFailure &error) {
            throw VerificationFailure(std::format(
                "could not prepare the {} VHDX view: {}",
                name, error.what()));
        } catch (const std::system_error &error) {
            if (IsCancellationError(error)) {
                throw;
            }
            throw VerificationFailure(std::format(
                "could not prepare the {} VHDX view: {}",
                name, error.what()));
        }
    }

    AttachedVhdx(const AttachedVhdx &) = delete;
    auto operator=(const AttachedVhdx &) -> AttachedVhdx & = delete;
    AttachedVhdx(AttachedVhdx &&) = delete;
    auto operator=(AttachedVhdx &&) -> AttachedVhdx & = delete;

    ~AttachedVhdx() {
        if (disk_) {
            const auto lock = virtual_disk_lock_.lock_exclusive();
            static_cast<void>(DetachLocked());
            // The attachment does not use PERMANENT_LIFETIME. If this explicit
            // attempt failed, closing disk_ is the fallback detach and must
            // remain serialized with every other virtual-disk operation.
            disk_.reset();
        }
    }

    [[nodiscard]] auto Root() const noexcept
        -> std::wstring_view {
        return root_;
    }

    [[nodiscard]] auto Detach() noexcept {
        const auto lock = virtual_disk_lock_.lock_exclusive();
        return DetachLocked();
    }

  private:
    [[nodiscard]] auto DetachLocked() noexcept -> DWORD {
        const auto status = DetachVirtualDisk(
            disk_.get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        if (status == ERROR_SUCCESS) {
            disk_.reset();
        }
        return status;
    }

    AttachedVhdx(
        wil::unique_handle disk,
        std::wstring root,
        wil::srwlock &virtual_disk_lock) noexcept
        : disk_{std::move(disk)}, root_{std::move(root)},
          virtual_disk_lock_{virtual_disk_lock} {}

    wil::unique_handle disk_;
    std::wstring root_;
    wil::srwlock &virtual_disk_lock_;
};

auto DetachView(
    AttachedVhdx &view,
    const std::string_view name,
    VerificationState &state) noexcept -> void {
    try {
        devicefs::WriteToStream(
            std::cout, "Detaching the {} VHDX view.\n", name);
        devicefs::WriteToStream(
            std::cout, L"  Volume: {}\n", view.Root());
    } catch (const std::exception &) {
        // Diagnostic output cannot replace or prevent VHDX cleanup.
    }
    const auto status = view.Detach();
    if (status != ERROR_SUCCESS) {
        try {
            state.RecordCleanupFailure(std::format(
                "could not detach the {} VHDX view (Windows error {})",
                name, status));
        } catch (const std::bad_alloc &) {
            // Cleanup reporting must not replace the verification result.
        }
        return;
    }
    try {
        devicefs::WriteToStream(
            std::cout, "The {} VHDX view was detached.\n", name);
        devicefs::WriteToStream(
            std::cout, L"  Volume: {}\n", view.Root());
    } catch (const std::exception &) {
        // Diagnostic output cannot replace completed VHDX cleanup.
    }
}

[[nodiscard]] auto ObjectPath(
    const FilesystemInventory &inventory,
    const std::wstring_view relative_path) {
    return relative_path.empty()
        ? inventory.root
        : inventory.root / relative_path;
}

[[nodiscard]] auto StreamPath(
    const FilesystemInventory &inventory,
    const ObjectRecord &object,
    const StreamRecord &stream) {
    auto path = ObjectPath(inventory, object.relative_path);
    if (!stream.name.empty()) {
        path = std::filesystem::path{
            std::format(L"{}{}", path.native(), stream.name)};
    }
    return path;
}

[[nodiscard]] auto TryOpenFilesystemObject(
    const std::filesystem::path &path) noexcept
    -> std::expected<wil::unique_hfile, OperationFailure> {
    // Preserve the exact spelling supplied by the namespace inventory when a
    // case-sensitive directory contains names that differ only by case.
    auto result = wil::unique_hfile{CreateFileW(
        path.c_str(), GENERIC_READ, kShareMode, nullptr, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION |
            FILE_FLAG_POSIX_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_BACKUP_SEMANTICS,
        nullptr)};
    if (!result) {
        return std::unexpected{OperationFailure{GetLastError()}};
    }
    return result;
}

[[nodiscard]] auto ClassifyObject(const DWORD attributes) noexcept {
    const auto directory =
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const auto reparse =
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (directory && reparse) {
        return ObjectKind::DirectoryReparsePoint;
    }
    if (directory) {
        return ObjectKind::Directory;
    }
    if (reparse) {
        return ObjectKind::FileReparsePoint;
    }
    return ObjectKind::File;
}

[[nodiscard]] auto QueryObjectStreams(const HANDLE object)
    -> std::expected<std::vector<StreamRecord>, OperationFailure> {
    auto streams = wistd::unique_ptr<FILE_STREAM_INFO>{};
    const auto query = wil::GetFileInfoNoThrow<FileStreamInfo>(
        object, streams);
    if (FAILED(query)) {
        return std::unexpected{OperationFailure{
            ExplicitWin32Error::FromHresult(query).value}};
    }

    auto result = std::vector<StreamRecord>{};
    for (const auto &stream :
        wil::create_next_entry_offset_iterator(streams.get())) {
        auto name = std::wstring{
            stream.StreamName,
            stream.StreamNameLength /
                sizeof(std::wstring::value_type),
        };
        if (name == L"::$DATA") {
            name.clear();
        }
        result.push_back({
            .name = std::move(name),
            .length = stream.StreamSize.QuadPart,
        });
    }
    std::ranges::sort(result, {}, &StreamRecord::name);
    return result;
}

[[nodiscard]] auto ChildPath(
    const std::wstring_view parent,
    const std::wstring_view name) {
    auto result = std::wstring{};
    result.reserve(parent.size() + !parent.empty() + name.size());
    result.append(parent);
    if (!parent.empty()) {
        result.push_back(L'\\');
    }
    result.append(name);
    return result;
}

class FilesystemTraverser {
  public:
    FilesystemTraverser(
        const std::wstring_view root,
        const VerificationEndpoint endpoint,
        const HANDLE cancellation_event,
        SynchronousIoCancellation &io_cancellation,
        VerificationState &state)
        : endpoint_{endpoint}, cancellation_event_{cancellation_event},
          io_cancellation_{io_cancellation}, state_{state},
          inventory_{.root = std::filesystem::path{root}} {}

    [[nodiscard]] auto Run()
        -> std::optional<FilesystemInventory> {
        auto initial = std::vector{
            TraversalTask{.kind = ObjectKind::Directory},
        };
        Queue(std::move(initial));

        auto workers = std::vector<std::future<void>>{};
        workers.reserve(kVerificationWorkerCount);
        try {
            for (auto index = 0uz;
                index < kVerificationWorkerCount; ++index) {
                workers.push_back(std::async(
                    std::launch::async,
                    [this] { Worker(); }));
            }
        } catch (...) {
            Fail(std::current_exception());
        }
        for (auto &worker : workers) {
            worker.get();
        }

        if (failure_) {
            std::rethrow_exception(failure_);
        }
        if (internal::CancellationRequested(cancellation_event_)) {
            return std::nullopt;
        }

        std::ranges::sort(
            inventory_.objects, {}, &ObjectRecord::relative_path);
        std::ranges::sort(inventory_.issues);
        std::ranges::sort(inventory_.incomplete_directories);
        return std::move(inventory_);
    }

  private:
    [[nodiscard]] auto Cancelled() const {
        return internal::CancellationRequested(cancellation_event_);
    }

    auto Queue(std::vector<TraversalTask> tasks) -> void {
        if (tasks.empty()) {
            return;
        }
        const auto count = tasks.size();
        {
            const auto lock = std::scoped_lock{mutex_};
            if (Cancelled() || failure_) {
                return;
            }
            pending_.insert(
                pending_.end(),
                std::make_move_iterator(tasks.begin()),
                std::make_move_iterator(tasks.end()));
            state_.RecordTraversalTasks(endpoint_, count);
        }
        ready_.notify_all();
    }

    [[nodiscard]] auto Take() -> std::optional<TraversalTask> {
        auto lock = std::unique_lock{mutex_};
        while (true) {
            if (Cancelled() || failure_) {
                return std::nullopt;
            }
            if (!pending_.empty()) {
                auto result = std::move(pending_.front());
                pending_.pop_front();
                ++active_;
                return result;
            }
            if (active_ == 0) {
                return std::nullopt;
            }
            // A 100-ms poll keeps Ctrl+C responsive while workers wait for a
            // directory to discover more work, without busy-waiting.
            constexpr auto cancellation_poll = 100ms;
            ready_.wait_for(lock, cancellation_poll);
        }
    }

    auto Complete() -> void {
        {
            const auto lock = std::scoped_lock{mutex_};
            --active_;
        }
        state_.RecordTraversalTaskCompleted(endpoint_);
        ready_.notify_all();
    }

    auto Fail(std::exception_ptr failure) -> void {
        {
            const auto lock = std::scoped_lock{mutex_};
            if (!failure_) {
                failure_ = std::move(failure);
            }
        }
        ready_.notify_all();
    }

    auto Worker() -> void {
        try {
            const auto registration =
                io_cancellation_.RegisterCurrentThread();
            while (auto task = Take()) {
                if (!Process(std::move(*task))) {
                    return;
                }
                Complete();
            }
        } catch (...) {
            Fail(std::current_exception());
        }
    }

    [[nodiscard]] auto CachedStreams(
        const std::optional<FileIdentity> identity)
        -> std::optional<std::vector<StreamRecord>> {
        if (!identity || (*identity == 0)) {
            return std::nullopt;
        }
        const auto lock = std::scoped_lock{mutex_};
        const auto found = streams_by_identity_.find(*identity);
        if (found == streams_by_identity_.end()) {
            return std::nullopt;
        }
        return inventory_.objects[found->second].streams;
    }

    auto Commit(ObjectRecord object) -> void {
        const auto stream_count = object.streams.size();
        const auto stream_bytes = std::accumulate(
            object.streams.begin(), object.streams.end(), std::uint64_t{},
            [](const std::uint64_t total, const StreamRecord &stream) {
                return stream.length > 0
                    ? SaturatingSum(total,
                        wil::safe_cast_failfast<std::uint64_t>(
                            stream.length))
                    : total;
            });
        {
            const auto lock = std::scoped_lock{mutex_};
            const auto index = inventory_.objects.size();
            inventory_.objects.push_back(std::move(object));
            const auto &committed = inventory_.objects[index];
            if (committed.streams_complete &&
                (committed.kind != ObjectKind::Directory) &&
                (committed.identity != 0)) {
                streams_by_identity_.try_emplace(
                    committed.identity, index);
            }
        }
        state_.RecordInventory(
            endpoint_, stream_count, stream_bytes);
    }

    auto RecordIssue(
        const std::wstring &path,
        const FilesystemOperation operation,
        const OperationFailure failure,
        const bool incomplete_directory = false) -> void {
        {
            const auto lock = std::scoped_lock{mutex_};
            inventory_.issues.push_back({
                .key = {
                    .path = path,
                    .operation = operation,
                },
                .failure = failure,
            });
            if (incomplete_directory) {
                inventory_.incomplete_directories.push_back(path);
            }
        }
        state_.RecordInventoryFailure(endpoint_);
    }

    [[nodiscard]] auto Process(TraversalTask task) -> bool {
        if (Cancelled()) {
            return false;
        }
        auto object = ObjectRecord{
            .relative_path = std::move(task.relative_path),
            .kind = task.kind,
            .identity = task.identity.value_or(0),
        };
        auto handle = TryOpenFilesystemObject(
            ObjectPath(inventory_, object.relative_path));
        if (Cancelled()) {
            return false;
        }
        if (!handle) {
            RecordIssue(object.relative_path,
                FilesystemOperation::OpenObject, handle.error(),
                object.kind == ObjectKind::Directory);
            const auto directory =
                object.kind == ObjectKind::Directory;
            Commit(std::move(object));
            if (directory) {
                state_.RecordDirectory(endpoint_);
            }
            return true;
        }

        if ((object.kind != ObjectKind::Directory) &&
            task.identity) {
            if (auto cached = CachedStreams(task.identity)) {
                object.streams = std::move(*cached);
                object.streams_complete = true;
                Commit(std::move(object));
                return true;
            }
        }

        auto streams = QueryObjectStreams(handle->get());
        if (Cancelled()) {
            return false;
        }
        if (streams) {
            object.streams = std::move(*streams);
            object.streams_complete = true;
        } else {
            RecordIssue(object.relative_path,
                FilesystemOperation::QueryStreams,
                streams.error());
        }
        const auto directory =
            object.kind == ObjectKind::Directory;
        const auto relative_path = object.relative_path;
        Commit(std::move(object));
        if (!directory) {
            return true;
        }
        if (!EnumerateDirectory(handle->get(), relative_path)) {
            return false;
        }
        state_.RecordDirectory(endpoint_);
        return true;
    }

    [[nodiscard]] auto EnumerateDirectory(
        const HANDLE directory,
        const std::wstring &relative_path) -> bool {
        while (true) {
            if (Cancelled()) {
                return false;
            }
            auto entries = wistd::unique_ptr<FILE_ID_BOTH_DIR_INFO>{};
            const auto query =
                wil::GetFileInfoNoThrow<FileIdBothDirectoryInfo>(
                    directory, entries);
            if (Cancelled()) {
                return false;
            }
            if (FAILED(query)) {
                RecordIssue(relative_path,
                    FilesystemOperation::QueryDirectory,
                    OperationFailure{
                        ExplicitWin32Error::FromHresult(query).value},
                    true);
                return true;
            }
            if (!entries) {
                return true;
            }

            auto children = std::vector<TraversalTask>{};
            for (const auto &entry :
                wil::create_next_entry_offset_iterator(entries.get())) {
                if (Cancelled()) {
                    return false;
                }
                const auto name = std::wstring_view{
                    entry.FileName,
                    entry.FileNameLength /
                        sizeof(std::wstring_view::value_type),
                };
                if ((name == L".") || (name == L"..")) {
                    continue;
                }
                static_assert(
                    sizeof(FileIdentity) ==
                    sizeof(entry.FileId.QuadPart));
                children.push_back({
                    .relative_path = ChildPath(relative_path, name),
                    .kind = ClassifyObject(entry.FileAttributes),
                    .identity = std::bit_cast<FileIdentity>(
                        entry.FileId.QuadPart),
                });
            }
            if (Cancelled()) {
                return false;
            }
            Queue(std::move(children));
        }
    }

    VerificationEndpoint endpoint_;
    HANDLE cancellation_event_;
    SynchronousIoCancellation &io_cancellation_;
    VerificationState &state_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<TraversalTask> pending_;
    std::size_t active_ = 0;
    std::exception_ptr failure_;
    FilesystemInventory inventory_;
    std::map<FileIdentity, std::size_t> streams_by_identity_;
};

[[nodiscard]] auto InventoryFilesystem(
    const std::wstring_view root,
    const VerificationEndpoint endpoint,
    const HANDLE cancellation_event,
    SynchronousIoCancellation &io_cancellation,
    VerificationState &state) {
    return FilesystemTraverser{
        root, endpoint, cancellation_event,
        io_cancellation, state}.Run();
}

struct InventoryFailures {
    std::optional<OperationFailure> open;
    std::optional<OperationFailure> streams;
    std::optional<OperationFailure> directory;
};

[[nodiscard]] auto FailuresForObject(
    const std::vector<InventoryIssue> &issues,
    std::size_t &cursor,
    const std::wstring_view path) noexcept {
    while ((cursor < issues.size()) &&
        (issues[cursor].key.path < path)) {
        ++cursor;
    }

    auto result = InventoryFailures{};
    while ((cursor < issues.size()) &&
        (issues[cursor].key.path == path)) {
        const auto &issue = issues[cursor++];
        switch (issue.key.operation) {
        case FilesystemOperation::OpenObject:
            result.open = issue.failure;
            break;
        case FilesystemOperation::QueryStreams:
            result.streams = issue.failure;
            break;
        case FilesystemOperation::QueryDirectory:
            result.directory = issue.failure;
            break;
        case FilesystemOperation::OpenStream:
        case FilesystemOperation::SeekStream:
        case FilesystemOperation::ReadStream:
            std::unreachable();
        }
    }
    return result;
}

auto RecordInventoryOperationComparison(
    VerificationState &state,
    const std::wstring &path,
    const FilesystemOperation operation,
    const std::optional<OperationFailure> synthetic,
    const std::optional<OperationFailure> real) -> void {
    if (!synthetic && !real) {
        return;
    }
    state.RecordOperationComparison({
        .path = path,
        .operation = operation,
    }, 0, synthetic, real);
}

[[nodiscard]] auto IsBelowIncompleteDirectory(
    const FilesystemInventory &inventory,
    const std::wstring_view path) {
    return std::ranges::any_of(
        inventory.incomplete_directories,
        [path](const std::wstring &directory) {
            if (directory.empty()) {
                return !path.empty();
            }
            return (path.size() > directory.size()) &&
                path.starts_with(directory) &&
                (path[directory.size()] == L'\\');
        });
}

[[nodiscard]] auto ComparisonSurfaceIncomplete(
    const FilesystemInventory &inventory) {
    return std::ranges::any_of(
        inventory.incomplete_directories,
        [](const std::wstring &directory) {
            return !IsVssExcludedPath(directory);
        });
}

auto AddObjectMismatch(
    VerificationState &state,
    const MismatchKind kind,
    const ObjectRecord &object) -> void {
    state.RecordMismatch({
        .kind = kind,
        .path = object.relative_path,
    });
}

auto AddTypeMismatch(
    VerificationState &state,
    const ObjectRecord &synthetic,
    const ObjectRecord &real) -> void {
    state.RecordMismatch({
        .kind = MismatchKind::ObjectType,
        .path = synthetic.relative_path,
        .synthetic_kind = synthetic.kind,
        .real_kind = real.kind,
    });
}

auto AddStreamMismatch(
    VerificationState &state,
    const MismatchKind kind,
    const ObjectRecord &object,
    const StreamRecord &stream) -> void {
    state.RecordMismatch({
        .kind = kind,
        .path = object.relative_path,
        .stream = stream.name,
    });
}

auto AddLengthMismatch(
    VerificationState &state,
    const ObjectRecord &object,
    const StreamRecord &synthetic,
    const StreamRecord &real) -> void {
    state.RecordMismatch({
        .kind = MismatchKind::StreamLength,
        .path = object.relative_path,
        .stream = synthetic.name,
        .synthetic_length = synthetic.length,
        .real_length = real.length,
    });
}

auto AppendStreamTasks(
    const std::size_t synthetic_object_index,
    const std::size_t real_object_index,
    const ObjectRecord &synthetic,
    const ObjectRecord &real,
    const bool compare_contents,
    const HANDLE cancellation_event,
    VerificationState &state,
    auto &&add_task) -> bool {
    const auto &synthetic_streams = synthetic.streams;
    const auto &real_streams = real.streams;
    auto synthetic_stream = 0uz;
    auto real_stream = 0uz;
    while ((synthetic_stream < synthetic_streams.size()) ||
        (real_stream < real_streams.size())) {
        if (internal::CancellationRequested(cancellation_event)) {
            return false;
        }
        if (synthetic_stream == synthetic_streams.size()) {
            AddStreamMismatch(state,
                MismatchKind::StreamMissingFromSynthetic,
                real, real_streams[real_stream++]);
            continue;
        }
        if (real_stream == real_streams.size()) {
            AddStreamMismatch(state,
                MismatchKind::StreamMissingFromReal,
                synthetic, synthetic_streams[synthetic_stream++]);
            continue;
        }
        const auto &synthetic_entry =
            synthetic_streams[synthetic_stream];
        const auto &real_entry = real_streams[real_stream];
        if (synthetic_entry.name < real_entry.name) {
            AddStreamMismatch(state,
                MismatchKind::StreamMissingFromReal,
                synthetic, synthetic_entry);
            ++synthetic_stream;
            continue;
        }
        if (real_entry.name < synthetic_entry.name) {
            AddStreamMismatch(state,
                MismatchKind::StreamMissingFromSynthetic,
                real, real_entry);
            ++real_stream;
            continue;
        }
        if (synthetic_entry.length != real_entry.length) {
            AddLengthMismatch(
                state, synthetic, synthetic_entry, real_entry);
        } else if (compare_contents) {
            for (auto offset = std::int64_t{};
                offset < synthetic_entry.length;) {
                if (internal::CancellationRequested(
                        cancellation_event)) {
                    return false;
                }
                const auto remaining =
                    synthetic_entry.length - offset;
                const auto length = std::min(
                    kReadChunkSize,
                    wil::safe_cast_failfast<std::size_t>(remaining));
                add_task(ReadTask{
                    .synthetic_object = synthetic_object_index,
                    .real_object = real_object_index,
                    .synthetic_stream = synthetic_stream,
                    .real_stream = real_stream,
                    .offset = offset,
                    .length = length,
                });
                offset += wil::safe_cast_failfast<std::int64_t>(length);
            }
        }
        ++synthetic_stream;
        ++real_stream;
    }
    return true;
}

[[nodiscard]] auto BuildComparisonPlan(
    const FilesystemInventory &synthetic,
    const FilesystemInventory &real,
    const GUID &seed_identifier,
    const double percentage,
    const HANDLE cancellation_event,
    VerificationState &state) {
    static_assert(sizeof(GUID) == 4 * sizeof(std::uint32_t));
    const auto seed_parts =
        std::bit_cast<std::array<std::uint32_t, 4>>(seed_identifier);
    auto seed = std::seed_seq{seed_parts.begin(), seed_parts.end()};
    auto random = std::mt19937_64{seed};
    // The percentage independently selects ordinary stream-content chunks,
    // with B's snapshot ID making the sample repeatable for this run.
    auto choose = std::bernoulli_distribution{percentage / 100.0};
    auto result = ComparisonPlan{};
    auto fallback = std::optional<ReadTask>{};
    auto task_count = std::size_t{};
    const auto add_task = [&](ReadTask task) {
        result.available_bytes += task.length;
        ++task_count;
        if (percentage >= 100.0) {
            result.selected_bytes += task.length;
            result.tasks.push_back(std::move(task));
            return;
        }
        const auto retain_for_fallback =
            std::uniform_int_distribution<std::size_t>{
                0, task_count - 1}(random) == 0;
        if (retain_for_fallback) {
            fallback = task;
        }
        if (choose(random)) {
            result.selected_bytes += task.length;
            result.tasks.push_back(std::move(task));
        }
    };
    auto compared_identities = std::set<PairedFileIdentity>{};
    auto synthetic_object = 0uz;
    auto real_object = 0uz;
    auto synthetic_issue = 0uz;
    auto real_issue = 0uz;
    while ((synthetic_object < synthetic.objects.size()) ||
        (real_object < real.objects.size())) {
        if (internal::CancellationRequested(cancellation_event)) {
            return std::optional<ComparisonPlan>{};
        }
        if (synthetic_object == synthetic.objects.size()) {
            const auto &entry = real.objects[real_object++];
            if (!IsBelowIncompleteDirectory(
                    synthetic, entry.relative_path)) {
                AddObjectMismatch(state,
                    MismatchKind::ObjectMissingFromSynthetic, entry);
            }
            continue;
        }
        if (real_object == real.objects.size()) {
            const auto &entry = synthetic.objects[synthetic_object++];
            if (!IsBelowIncompleteDirectory(
                    real, entry.relative_path)) {
                AddObjectMismatch(state,
                    MismatchKind::ObjectMissingFromReal, entry);
            }
            continue;
        }
        const auto &synthetic_entry =
            synthetic.objects[synthetic_object];
        const auto &real_entry = real.objects[real_object];
        if (synthetic_entry.relative_path < real_entry.relative_path) {
            if (!IsBelowIncompleteDirectory(
                    real, synthetic_entry.relative_path)) {
                AddObjectMismatch(state,
                    MismatchKind::ObjectMissingFromReal,
                    synthetic_entry);
            }
            ++synthetic_object;
            continue;
        }
        if (real_entry.relative_path < synthetic_entry.relative_path) {
            if (!IsBelowIncompleteDirectory(
                    synthetic, real_entry.relative_path)) {
                AddObjectMismatch(state,
                    MismatchKind::ObjectMissingFromSynthetic,
                    real_entry);
            }
            ++real_object;
            continue;
        }
        const auto synthetic_failures = FailuresForObject(
            synthetic.issues, synthetic_issue,
            synthetic_entry.relative_path);
        const auto real_failures = FailuresForObject(
            real.issues, real_issue, real_entry.relative_path);
        RecordInventoryOperationComparison(state,
            synthetic_entry.relative_path,
            FilesystemOperation::OpenObject,
            synthetic_failures.open, real_failures.open);
        const auto both_opened =
            !synthetic_failures.open && !real_failures.open;
        if (both_opened) {
            RecordInventoryOperationComparison(state,
                synthetic_entry.relative_path,
                FilesystemOperation::QueryStreams,
                synthetic_failures.streams, real_failures.streams);
            if ((synthetic_entry.kind == ObjectKind::Directory) &&
                (real_entry.kind == ObjectKind::Directory)) {
                RecordInventoryOperationComparison(state,
                    synthetic_entry.relative_path,
                    FilesystemOperation::QueryDirectory,
                    synthetic_failures.directory,
                    real_failures.directory);
            }
        }
        if (synthetic_entry.kind != real_entry.kind) {
            AddTypeMismatch(state, synthetic_entry, real_entry);
        } else if (both_opened &&
            synthetic_entry.streams_complete &&
            real_entry.streams_complete) {
            // Hard-link names remain separate namespace entries, but equal
            // paired file identities expose the same streams and need only one
            // content comparison.
            const auto compare_contents =
                (synthetic_entry.identity == 0) ||
                (real_entry.identity == 0) ||
                compared_identities.insert({
                    .synthetic = synthetic_entry.identity,
                    .real = real_entry.identity,
                }).second;
            if (!AppendStreamTasks(
                synthetic_object, real_object,
                synthetic_entry, real_entry,
                compare_contents, cancellation_event,
                state, add_task)) {
                return std::optional<ComparisonPlan>{};
            }
        }
        ++synthetic_object;
        ++real_object;
    }

    if (result.tasks.empty() && fallback) {
        result.selected_bytes = fallback->length;
        result.tasks.push_back(std::move(*fallback));
    }
    return std::optional<ComparisonPlan>{std::move(result)};
}

[[nodiscard]] auto TrySeekStream(
    const HANDLE stream,
    const std::int64_t offset) noexcept -> std::optional<OperationFailure> {
    const auto position = LARGE_INTEGER{.QuadPart = offset};
    if (!SetFilePointerEx(stream, position, nullptr, FILE_BEGIN)) {
        return OperationFailure{GetLastError()};
    }
    return std::nullopt;
}

[[nodiscard]] auto TryReadStreamChunk(
    const HANDLE stream,
    const std::span<unsigned char> destination)
    -> std::optional<OperationFailure> {
    static_assert(kReadChunkSize <= std::numeric_limits<DWORD>::max());
    const auto wanted =
        wil::safe_cast_failfast<DWORD>(destination.size());
    auto transferred = DWORD{};
    if (!ReadFile(stream, destination.data(), wanted,
            &transferred, nullptr)) {
        return OperationFailure{GetLastError()};
    }
    if (transferred != wanted) {
        return OperationFailure{
            .error = ERROR_SUCCESS,
            .transferred = transferred,
        };
    }
    return std::nullopt;
}

class ComparisonWorker {
  public:
    ComparisonWorker(
        const FilesystemInventory &synthetic,
        const FilesystemInventory &real,
        const HANDLE cancellation_event,
        VerificationState &state)
        : synthetic_{synthetic}, real_{real},
          cancellation_event_{cancellation_event}, state_{state},
          synthetic_buffer_(kReadChunkSize),
          real_buffer_(kReadChunkSize) {}

    [[nodiscard]] auto Compare(const ReadTask &task) -> bool {
        if (internal::CancellationRequested(cancellation_event_)) {
            return false;
        }
        const auto key = std::array{
            task.synthetic_object, task.real_object,
            task.synthetic_stream, task.real_stream};
        if (!current_stream_ || (*current_stream_ != key)) {
            current_stream_ = key;
            current_stream_readable_ = OpenStream(task);
        }
        if (internal::CancellationRequested(cancellation_event_)) {
            return false;
        }
        if (!current_stream_readable_) {
            return current_stream_open_failure_accepted_;
        }

        const auto &synthetic_object =
            synthetic_.objects[task.synthetic_object];
        const auto &synthetic_stream =
            synthetic_object.streams[task.synthetic_stream];
        const auto synthetic_seek = TrySeekStream(
            synthetic_handle_.get(), task.offset);
        if (internal::CancellationRequested(cancellation_event_)) {
            return false;
        }
        const auto real_seek = TrySeekStream(
            real_handle_.get(), task.offset);
        if (internal::CancellationRequested(cancellation_event_)) {
            return false;
        }
        if (synthetic_seek || real_seek) {
            RecordOperation(task, synthetic_object, synthetic_stream,
                FilesystemOperation::SeekStream, 0,
                synthetic_seek, real_seek);
            return FailuresMatch(synthetic_seek, real_seek) ||
                IsVssExcludedPath(synthetic_object.relative_path);
        }
        const auto synthetic_chunk =
            std::span{synthetic_buffer_}.first(task.length);
        const auto real_chunk =
            std::span{real_buffer_}.first(task.length);
        const auto synthetic_read = TryReadStreamChunk(
            synthetic_handle_.get(), synthetic_chunk);
        if (internal::CancellationRequested(cancellation_event_)) {
            return false;
        }
        const auto real_read = TryReadStreamChunk(
            real_handle_.get(), real_chunk);
        if (internal::CancellationRequested(cancellation_event_) &&
            ((synthetic_read &&
                IsCancellationError(synthetic_read->error)) ||
                (real_read &&
                    IsCancellationError(real_read->error)))) {
            return false;
        }
        auto compared_length = task.length;
        if (synthetic_read || real_read) {
            RecordOperation(task, synthetic_object, synthetic_stream,
                FilesystemOperation::ReadStream,
                wil::safe_cast_failfast<DWORD>(task.length),
                synthetic_read, real_read);
            if (!synthetic_read || !real_read ||
                (*synthetic_read != *real_read)) {
                return IsVssExcludedPath(
                    synthetic_object.relative_path);
            }
            if (synthetic_read->error != ERROR_SUCCESS) {
                return true;
            }
            compared_length = synthetic_read->transferred;
        }
        const auto synthetic_compared =
            synthetic_chunk.first(compared_length);
        const auto real_compared =
            real_chunk.first(compared_length);
        const auto first = std::ranges::mismatch(
            synthetic_compared, real_compared);
        if (first.in1 != synthetic_compared.end()) {
            const auto first_relative = std::ranges::distance(
                synthetic_compared.begin(), first.in1);
            const auto differing_bytes = std::transform_reduce(
                synthetic_compared.begin(), synthetic_compared.end(),
                real_compared.begin(), std::uint64_t{}, std::plus{},
                std::not_equal_to{});
            state_.RecordContentMismatch(
                synthetic_object.relative_path,
                synthetic_stream.name,
                task.offset + first_relative,
                *first.in1, *first.in2, differing_bytes);
        }
        state_.RecordCompared(compared_length);
        return true;
    }

  private:
    [[nodiscard]] auto OpenStream(const ReadTask &task) -> bool {
        synthetic_handle_.reset();
        real_handle_.reset();
        current_stream_open_failure_accepted_ = false;
        const auto &synthetic_object =
            synthetic_.objects[task.synthetic_object];
        const auto &real_object = real_.objects[task.real_object];
        const auto &synthetic_stream =
            synthetic_object.streams[task.synthetic_stream];
        const auto &real_stream =
            real_object.streams[task.real_stream];
        auto synthetic_handle = TryOpenFilesystemObject(
            StreamPath(synthetic_, synthetic_object, synthetic_stream));
        if (internal::CancellationRequested(cancellation_event_)) {
            return false;
        }
        auto real_handle = TryOpenFilesystemObject(
            StreamPath(real_, real_object, real_stream));
        if (internal::CancellationRequested(cancellation_event_) &&
            ((!synthetic_handle &&
                IsCancellationError(synthetic_handle.error().error)) ||
                (!real_handle &&
                    IsCancellationError(real_handle.error().error)))) {
            return false;
        }
        if (!synthetic_handle || !real_handle) {
            current_stream_open_failure_accepted_ =
                FailuresMatch(
                    synthetic_handle
                        ? std::nullopt
                        : std::optional{synthetic_handle.error()},
                    real_handle
                        ? std::nullopt
                        : std::optional{real_handle.error()}) ||
                IsVssExcludedPath(synthetic_object.relative_path);
            RecordOperation(task, synthetic_object, synthetic_stream,
                FilesystemOperation::OpenStream, 0,
                synthetic_handle
                    ? std::nullopt
                    : std::optional{synthetic_handle.error()},
                real_handle
                    ? std::nullopt
                    : std::optional{real_handle.error()});
            return false;
        }
        synthetic_handle_ = std::move(*synthetic_handle);
        real_handle_ = std::move(*real_handle);
        return true;
    }

    auto RecordOperation(
        const ReadTask &task,
        const ObjectRecord &object,
        const StreamRecord &stream,
        const FilesystemOperation operation,
        const DWORD requested,
        std::optional<OperationFailure> synthetic,
        std::optional<OperationFailure> real) -> void {
        state_.RecordOperationComparison({
            .path = object.relative_path,
            .stream = stream.name,
            .operation = operation,
            .offset = operation == FilesystemOperation::OpenStream
                ? 0 : task.offset,
        }, requested, std::move(synthetic), std::move(real));
    }

    const FilesystemInventory &synthetic_;
    const FilesystemInventory &real_;
    HANDLE cancellation_event_;
    VerificationState &state_;
    std::vector<unsigned char> synthetic_buffer_;
    std::vector<unsigned char> real_buffer_;
    std::optional<std::array<std::size_t, 4>> current_stream_;
    bool current_stream_readable_ = false;
    bool current_stream_open_failure_accepted_ = false;
    wil::unique_hfile synthetic_handle_;
    wil::unique_hfile real_handle_;
};

auto RunComparisonWorkers(
    const ComparisonPlan &plan,
    const FilesystemInventory &synthetic,
    const FilesystemInventory &real,
    const HANDLE cancellation_event,
    SynchronousIoCancellation &io_cancellation,
    VerificationState &state) -> bool {
    auto next_task = std::atomic<std::size_t>{};
    auto completed_tasks = std::atomic<std::size_t>{};
    const auto run = [&] {
        const auto registration =
            io_cancellation.RegisterCurrentThread();
        auto worker = ComparisonWorker{
            synthetic, real, cancellation_event, state};
        while (!state.Failed() &&
            !internal::CancellationRequested(cancellation_event)) {
            const auto index = next_task.fetch_add(
                1, std::memory_order_relaxed);
            if (index >= plan.tasks.size()) {
                return;
            }
            const auto &task = plan.tasks[index];
            if (worker.Compare(task)) {
                completed_tasks.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
    };

    auto workers = std::vector<std::future<void>>{};
    const auto worker_count = std::min(
        kVerificationWorkerCount, plan.tasks.size());
    workers.reserve(worker_count);
    try {
        for (auto index = 0uz; index < worker_count; ++index) {
            workers.push_back(std::async(std::launch::async, run));
        }
    } catch (const std::system_error &error) {
        state.Fail(std::format(
            "could not start a filesystem-verification worker: {}",
            error.what()));
    }
    for (auto &worker : workers) {
        worker.get();
    }
    return !state.Failed() &&
        !internal::CancellationRequested(cancellation_event) &&
        std::ranges::none_of(
            state.OperationComparisonsAfterJoin(),
            IsContentOperationMismatch) &&
        (completed_tasks.load(std::memory_order_relaxed) ==
            plan.tasks.size());
}

auto CompareAttachedFilesystems(
    const FilesystemVerificationVolume &volume,
    const AttachedVhdx &synthetic_attachment,
    const AttachedVhdx &real_attachment,
    const HANDLE cancellation_event,
    const double percentage,
    SynchronousIoCancellation &io_cancellation,
    VerificationState &state) -> void {
    if (internal::CancellationRequested(cancellation_event)) {
        return;
    }
    const auto volume_identifier =
        winrt::to_hstring(volume.volume_identifier);
    state.BeginTraversal();
    devicefs::WriteToStream(
        std::cout,
        L"\nBeginning concurrent filesystem traversal for volume {}.\n"
        L"  Synthetic volume: {}\n"
        L"  Real-B volume: {}\n"
        L"  Traversal workers per view: {}\n",
        std::wstring_view{volume_identifier},
        synthetic_attachment.Root(), real_attachment.Root(),
        kVerificationWorkerCount);

    auto synthetic_operation = std::async(
        std::launch::async,
        [&] {
            return InventoryFilesystem(
                synthetic_attachment.Root(),
                VerificationEndpoint::Synthetic,
                cancellation_event, io_cancellation, state);
        });
    auto real_operation = std::async(
        std::launch::async,
        [&] {
            return InventoryFilesystem(
                real_attachment.Root(), VerificationEndpoint::Real,
                cancellation_event, io_cancellation, state);
        });
    auto synthetic = synthetic_operation.get();
    auto real = real_operation.get();
    if (synthetic && real && !state.Failed() &&
        !internal::CancellationRequested(cancellation_event)) {
        state.CompleteTraversal();
        const auto observation = state.Observe();
        devicefs::WriteToStream(std::cout,
            L"Filesystem traversal finished for volume {}.\n"
            L"  Synthetic: {} directories, {} object(s), {} "
            L"stream(s), {}{} logical stream byte(s) observed, "
            L"{} operation failure(s)\n"
            L"  Real B: {} directories, {} object(s), {} "
            L"stream(s), {}{} logical stream byte(s) observed, "
            L"{} operation failure(s)\n",
            std::wstring_view{volume_identifier},
            observation.synthetic.directories,
            observation.synthetic.objects,
            observation.synthetic.streams,
            observation.synthetic.stream_bytes ==
                std::numeric_limits<std::uint64_t>::max()
                ? std::wstring_view{L"at least "}
                : std::wstring_view{},
            observation.synthetic.stream_bytes,
            observation.synthetic.failures,
            observation.real.directories,
            observation.real.objects,
            observation.real.streams,
            observation.real.stream_bytes ==
                std::numeric_limits<std::uint64_t>::max()
                ? std::wstring_view{L"at least "}
                : std::wstring_view{},
            observation.real.stream_bytes,
            observation.real.failures);
        const auto comparison_surface_incomplete =
            ComparisonSurfaceIncomplete(*synthetic) ||
            ComparisonSurfaceIncomplete(*real);
        state.SetPhase(VerificationPhase::Planning);
        devicefs::WriteToStream(std::cout,
            L"Filesystem inventories are ready for volume {}; "
            L"preparing content comparisons.\n",
            std::wstring_view{volume_identifier});
        auto plan = BuildComparisonPlan(
            *synthetic, *real, volume.payload_snapshot_identifier,
            percentage, cancellation_event, state);
        if (plan) {
            state.SetPlan(*plan);
        }
        if (plan &&
            !internal::CancellationRequested(cancellation_event)) {
            state.BeginComparison();
            if (RunComparisonWorkers(
                    *plan, *synthetic, *real,
                    cancellation_event, io_cancellation, state)) {
                if (comparison_surface_incomplete) {
                    state.Fail(
                        "ordinary filesystem traversal could not observe "
                        "the complete namespace beneath at least one "
                        "directory");
                } else {
                    state.CompleteComparison();
                }
            }
        }
    }
}

auto VerifyVolume(
    const FilesystemVerificationVolume &volume,
    const std::filesystem::path &synthetic_vhdx,
    const std::filesystem::path &real_vhdx,
    const HANDLE cancellation_event,
    const double percentage,
    wil::srwlock &virtual_disk_lock,
    SynchronousIoCancellation &io_cancellation,
    VerificationState &state) -> void {
    if (internal::CancellationRequested(cancellation_event)) {
        return;
    }
    state.SetPhase(VerificationPhase::AttachingSynthetic);
    auto synthetic_attachment = AttachedVhdx::Attach(
        synthetic_vhdx, "synthetic", cancellation_event,
        virtual_disk_lock, io_cancellation);
    if (internal::CancellationRequested(cancellation_event)) {
        state.SetPhase(VerificationPhase::Detaching);
        DetachView(synthetic_attachment, "synthetic", state);
        state.SetPhase(VerificationPhase::Complete);
        return;
    }
    try {
        state.SetPhase(VerificationPhase::AttachingReal);
        auto real_attachment = AttachedVhdx::Attach(
            real_vhdx, "real B", cancellation_event,
            virtual_disk_lock, io_cancellation);
        try {
            CompareAttachedFilesystems(volume,
                synthetic_attachment, real_attachment,
                cancellation_event, percentage,
                io_cancellation, state);
        } catch (...) {
            state.SetPhase(VerificationPhase::Detaching);
            DetachView(real_attachment, "real-B", state);
            throw;
        }
        state.SetPhase(VerificationPhase::Detaching);
        DetachView(real_attachment, "real-B", state);
    } catch (...) {
        state.SetPhase(VerificationPhase::Detaching);
        DetachView(synthetic_attachment, "synthetic", state);
        state.SetPhase(VerificationPhase::Complete);
        throw;
    }

    DetachView(synthetic_attachment, "synthetic", state);
    state.SetPhase(VerificationPhase::Complete);
}

struct VolumeJob {
    GUID volume_identifier;
    double percentage;
    std::unique_ptr<VerificationState> state;
    // The operation is destroyed before the state it borrows.
    std::future<void> operation;
    std::size_t printed_mismatches = 0;
    std::size_t printed_operation_comparisons = 0;
};

[[nodiscard]] auto PhaseName(
    const VerificationPhase phase) noexcept -> std::string_view {
    switch (phase) {
    case VerificationPhase::AttachingSynthetic:
        return "attaching the synthetic VHDX";
    case VerificationPhase::AttachingReal:
        return "attaching the real-B VHDX";
    case VerificationPhase::Traversing:
        return "traversing both filesystems";
    case VerificationPhase::Planning:
        return "preparing content comparisons";
    case VerificationPhase::Comparing:
        return "comparing stream contents";
    case VerificationPhase::Detaching:
        return "detaching the VHDX views";
    case VerificationPhase::Complete:
        return "complete";
    }
    std::unreachable();
}

[[nodiscard]] auto ObjectKindName(
    const ObjectKind kind) noexcept -> std::string_view {
    switch (kind) {
    case ObjectKind::File:
        return "file";
    case ObjectKind::Directory:
        return "directory";
    case ObjectKind::FileReparsePoint:
        return "file reparse point";
    case ObjectKind::DirectoryReparsePoint:
        return "directory reparse point";
    }
    std::unreachable();
}

[[nodiscard]] auto DisplayPath(
    const std::wstring &path) noexcept -> std::wstring_view {
    return path.empty() ? std::wstring_view{L"\\"} : path;
}

[[nodiscard]] auto OperationName(
    const FilesystemOperation operation) noexcept -> std::string_view {
    switch (operation) {
    case FilesystemOperation::QueryDirectory:
        return "enumerate directory";
    case FilesystemOperation::OpenObject:
        return "open object";
    case FilesystemOperation::QueryStreams:
        return "enumerate object streams";
    case FilesystemOperation::OpenStream:
        return "open stream";
    case FilesystemOperation::SeekStream:
        return "seek stream";
    case FilesystemOperation::ReadStream:
        return "read stream";
    }
    std::unreachable();
}

auto PrintOperationOutcome(
    std::ostream &output,
    const std::string_view name,
    const std::optional<OperationFailure> &failure,
    const DWORD requested) -> void {
    if (!failure) {
        devicefs::WriteToStream(output,
            "  {} outcome: succeeded.\n", name);
        return;
    }
    if (failure->error == ERROR_SUCCESS) {
        devicefs::WriteToStream(output,
            "  {} outcome: returned {} of {} requested bytes.\n",
            name, failure->transferred, requested);
        return;
    }
    static_assert(sizeof(DWORD) == sizeof(int));
    const auto message = std::error_code{
        std::bit_cast<int>(failure->error),
        std::system_category()}.message();
    devicefs::WriteToStream(output,
        "  {} outcome: Windows error {} ({}).\n",
        name, std::uint32_t{failure->error}, message);
}

auto PrintOperationComparison(
    std::ostream &output,
    const GUID &volume_identifier,
    const OperationComparison &comparison) -> void {
    const auto matched = IsMatchedError(comparison);
    const auto identifier = winrt::to_hstring(volume_identifier);
    if (matched) {
        devicefs::WriteToStream(
            output, "\nMatched filesystem error:\n");
    } else if (IsVssExcludedDifference(comparison)) {
        devicefs::WriteToStream(output,
            "\nIdentified difference in object known to be excluded "
            "from VSS snapshots:\n");
    } else {
        devicefs::WriteToStream(output,
            "\n*** FILESYSTEM VERIFICATION MISMATCH ***\n");
    }
    devicefs::WriteToStream(output,
        L"  Volume ID: {}\n",
        std::wstring_view{identifier});
    devicefs::WriteToStream(output,
        L"  Object: {}\n", DisplayPath(comparison.key.path));
    if (!comparison.key.stream.empty() ||
        (comparison.key.operation == FilesystemOperation::OpenStream) ||
        (comparison.key.operation == FilesystemOperation::SeekStream) ||
        (comparison.key.operation == FilesystemOperation::ReadStream)) {
        devicefs::WriteToStream(output,
            L"  Stream: {}\n",
            comparison.key.stream.empty()
                ? std::wstring_view{L"(unnamed data stream)"}
                : std::wstring_view{comparison.key.stream});
    }
    devicefs::WriteToStream(output,
        "  Operation: {}\n", OperationName(comparison.key.operation));
    if ((comparison.key.operation == FilesystemOperation::SeekStream) ||
        (comparison.key.operation == FilesystemOperation::ReadStream)) {
        devicefs::WriteToStream(output,
            "  Stream offset: 0x{:x}\n", comparison.key.offset);
    }
    if (matched) {
        PrintOperationOutcome(output, "Both views",
            comparison.synthetic, comparison.requested);
        return;
    }
    PrintOperationOutcome(output, "Synthetic",
        comparison.synthetic, comparison.requested);
    PrintOperationOutcome(output, "Real B",
        comparison.real, comparison.requested);
}

auto PrintMismatch(
    std::ostream &output,
    const GUID &volume_identifier,
    const MismatchDetail &mismatch) -> void {
    const auto identifier = winrt::to_hstring(volume_identifier);
    if (IsVssExcludedPath(mismatch.path)) {
        devicefs::WriteToStream(output,
            "\nIdentified difference in object known to be excluded "
            "from VSS snapshots:\n");
    } else {
        devicefs::WriteToStream(output,
            "\n*** FILESYSTEM VERIFICATION MISMATCH ***\n");
    }
    devicefs::WriteToStream(output,
        L"  Volume ID: {}\n"
        L"  Object: {}\n",
        std::wstring_view{identifier}, DisplayPath(mismatch.path));
    if ((mismatch.kind == MismatchKind::StreamMissingFromSynthetic) ||
        (mismatch.kind == MismatchKind::StreamMissingFromReal) ||
        (mismatch.kind == MismatchKind::StreamLength) ||
        (mismatch.kind == MismatchKind::StreamContents)) {
        devicefs::WriteToStream(output,
            L"  Stream: {}\n",
            mismatch.stream.empty()
                ? std::wstring_view{L"(unnamed data stream)"}
                : std::wstring_view{mismatch.stream});
    }
    switch (mismatch.kind) {
    case MismatchKind::ObjectMissingFromSynthetic:
        devicefs::WriteToStream(output,
            "  Difference: the synthetic backup is missing this object.\n");
        break;
    case MismatchKind::ObjectMissingFromReal:
        devicefs::WriteToStream(output,
            "  Difference: the synthetic backup contains an object that "
            "is absent from real B.\n");
        break;
    case MismatchKind::ObjectType:
        devicefs::WriteToStream(output,
            "  Difference: synthetic type is {}; real-B type is {}.\n",
            ObjectKindName(mismatch.synthetic_kind),
            ObjectKindName(mismatch.real_kind));
        break;
    case MismatchKind::StreamMissingFromSynthetic:
        devicefs::WriteToStream(output,
            "  Difference: the stream is missing from the synthetic "
            "backup.\n");
        break;
    case MismatchKind::StreamMissingFromReal:
        devicefs::WriteToStream(output,
            "  Difference: the stream exists only in the synthetic "
            "backup.\n");
        break;
    case MismatchKind::StreamLength:
        devicefs::WriteToStream(output,
            "  Difference: synthetic length is {} bytes; real-B length "
            "is {} bytes.\n",
            mismatch.synthetic_length, mismatch.real_length);
        break;
    case MismatchKind::StreamContents:
        devicefs::WriteToStream(output,
            "  First differing stream byte: 0x{:x}; "
            "synthetic=0x{:02x}, real-B=0x{:02x}\n"
            "  Observed so far: {} differing byte(s) in {} read chunk(s).\n",
            mismatch.first_offset,
            std::uint32_t{mismatch.synthetic_byte},
            std::uint32_t{mismatch.real_byte},
            mismatch.differing_bytes, mismatch.differing_chunks);
        break;
    }
}

auto PrintNewMismatches(
    std::span<VolumeJob> jobs,
    std::ostream &output) -> void {
    for (auto &job : jobs) {
        for (const auto &mismatch :
            job.state->NewMismatches(job.printed_mismatches)) {
            PrintMismatch(output, job.volume_identifier, mismatch);
        }
        for (const auto &comparison :
            job.state->NewOperationComparisons(
                job.printed_operation_comparisons)) {
            if (!IsMatchedError(comparison)) {
                PrintOperationComparison(
                    output, job.volume_identifier, comparison);
            }
        }
    }
}

[[nodiscard]] constexpr auto Percentage(
    const std::uint64_t completed,
    const std::uint64_t total) noexcept {
    return total == 0 ? 100.0 :
        (static_cast<double>(completed) /
            static_cast<double>(total)) * 100.0;
}

auto PrintTraversalProgress(
    std::ostream &output,
    const std::string_view name,
    const InventoryObservation &inventory,
    const double elapsed_seconds) noexcept -> void {
    const auto not_completed =
        inventory.tasks_discovered > inventory.tasks_completed
        ? inventory.tasks_discovered - inventory.tasks_completed
        : 0;
    const auto stream_bytes_prefix =
        SaturationPrefix(inventory.stream_bytes);
    devicefs::WriteToStream(output,
        "    {}: {} directories, {} object(s), {} stream(s), "
        "{}{} logical stream byte(s) observed; "
        "{} work item(s) not completed; "
        "{} operation failure(s)\n",
        name, inventory.directories, inventory.objects,
        inventory.streams, stream_bytes_prefix,
        inventory.stream_bytes,
        not_completed, inventory.failures);
    if ((elapsed_seconds > 0.0) && (not_completed != 0)) {
        devicefs::WriteToStream(output,
            "      Average traversal rate: {:.2f} directories/s, "
            "{:.2f} object(s)/s, {}{:.2f} logical stream MiB "
            "observed/s\n",
            static_cast<double>(inventory.directories) /
                elapsed_seconds,
            static_cast<double>(inventory.objects) /
                elapsed_seconds,
            stream_bytes_prefix,
            static_cast<double>(inventory.stream_bytes) /
                (1024.0 * 1024.0 * elapsed_seconds));
    }
}

auto PrintProgress(const std::span<VolumeJob> jobs) -> void {
    devicefs::WriteToStream(
        std::cout, "\nFilesystem verification progress:\n");
    for (const auto &job : jobs) {
        const auto observation = job.state->Observe();
        const auto identifier =
            winrt::to_hstring(job.volume_identifier);
        devicefs::WriteToStream(std::cout,
            L"  Volume ID: {}\n",
            std::wstring_view{identifier});
        devicefs::WriteToStream(std::cout,
            "    Phase: {}\n",
            PhaseName(observation.phase));
        if (observation.traversal_started) {
            devicefs::WriteToStream(std::cout,
                "    Namespace traversal:\n");
            const auto traversal_seconds =
                std::chrono::duration<double>(
                std::chrono::steady_clock::now() -
                    *observation.traversal_started).count();
            PrintTraversalProgress(std::cout, "Synthetic",
                observation.synthetic,
                observation.traversal_complete
                    ? 0.0 : traversal_seconds);
            PrintTraversalProgress(std::cout, "Real B",
                observation.real,
                observation.traversal_complete
                    ? 0.0 : traversal_seconds);
            if (observation.traversal_complete) {
                devicefs::WriteToStream(std::cout,
                    "    Namespace traversal completed on both views.\n");
            }
        }
        if (observation.plan_ready) {
            devicefs::WriteToStream(std::cout,
                "    Selected content: {} of {} bytes ({:.2f}% actual)\n"
                "    Compared: {} bytes ({:.2f}% of selection)\n",
                observation.selected_bytes,
                observation.available_bytes,
                Percentage(observation.selected_bytes,
                    observation.available_bytes),
                observation.compared_bytes,
                Percentage(observation.compared_bytes,
                    observation.selected_bytes));
        }
        if (observation.comparison_started) {
            const auto elapsed =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() -
                    *observation.comparison_started).count();
            const auto mebibytes_per_second = elapsed == 0.0
                ? 0.0
                : static_cast<double>(observation.compared_bytes) /
                    (1024.0 * 1024.0 * elapsed);
            devicefs::WriteToStream(std::cout,
                "    Comparison rate: {:.2f} MiB/s\n",
                mebibytes_per_second);
        }
        devicefs::WriteToStream(std::cout,
            "    Mismatches observed: {}\n"
            "    VSS exclusions observed: {}\n"
            "    Matched filesystem errors: {}\n",
            observation.mismatch_count,
            observation.exclusion_count,
            observation.matched_error_count);
        if (observation.failed) {
            devicefs::WriteToStream(std::cout,
                "    Verification failure: {}\n",
                observation.failure);
        }
    }
}

[[nodiscard]] auto JobsReady(
    const std::span<VolumeJob> jobs) {
    return std::ranges::all_of(jobs, [](const VolumeJob &job) {
        return !job.operation.valid() ||
            (job.operation.wait_for(0s) ==
                std::future_status::ready);
    });
}

[[nodiscard]] auto PrintFinalResults(
    const std::span<const VolumeJob> jobs,
    const std::size_t optimization_unavailable,
    const std::size_t preparation_failures) {
    devicefs::WriteToStream(
        std::cout, "\nFilesystem verification results:\n");
    auto verified = 0uz;
    auto sample_verified = 0uz;
    auto mismatched = 0uz;
    auto indeterminate = 0uz;
    auto incomplete = 0uz;
    auto cleanup_failed = 0uz;
    auto matched_error_volumes = 0uz;
    auto exclusion_volumes = 0uz;
    for (const auto &job : jobs) {
        const auto observation = job.state->Observe();
        const auto &mismatches = job.state->MismatchesAfterJoin();
        const auto &operation_comparisons =
            job.state->OperationComparisonsAfterJoin();
        if (observation.matched_error_count != 0) {
            ++matched_error_volumes;
        }
        if (observation.exclusion_count != 0) {
            ++exclusion_volumes;
        }
        const auto identifier =
            winrt::to_hstring(job.volume_identifier);
        devicefs::WriteToStream(std::cout,
            L"\n  Volume ID: {}\n",
            std::wstring_view{identifier});
        if (observation.mismatch_count != 0) {
            ++mismatched;
            if (observation.failed) {
                devicefs::WriteToStream(std::cout,
                    "    Status: MISMATCH; an operational failure also "
                    "prevented complete comparison\n");
                devicefs::WriteToStream(std::cout,
                    "    Failure: {}\n", observation.failure);
            } else if (observation.comparison_complete) {
                devicefs::WriteToStream(
                    std::cout, "    Status: MISMATCH\n");
            } else {
                devicefs::WriteToStream(std::cout,
                    "    Status: MISMATCH; comparison incomplete\n");
            }
        } else if (observation.failed) {
            ++indeterminate;
            devicefs::WriteToStream(std::cout,
                "    Status: INDETERMINATE\n"
                "    Failure: {}\n",
                observation.failure);
        } else if (!observation.comparison_complete) {
            ++incomplete;
            devicefs::WriteToStream(std::cout,
                "    Status: CANCELLED/INCOMPLETE\n");
        } else if (observation.selected_bytes !=
            observation.available_bytes) {
            ++sample_verified;
            devicefs::WriteToStream(std::cout,
                "    Status: SAMPLE VERIFIED ({:.2f}% requested)\n",
                job.percentage);
        } else {
            ++verified;
            devicefs::WriteToStream(std::cout,
                "    Status: VERIFIED\n");
        }
        devicefs::WriteToStream(std::cout,
            "    Synthetic namespace: {} directories, {} object(s), "
            "{} stream(s), {}{} logical stream byte(s) observed, "
            "{} operation failure(s)\n"
            "    Real-B namespace: {} directories, {} object(s), "
            "{} stream(s), {}{} logical stream byte(s) observed, "
            "{} operation failure(s)\n",
            observation.synthetic.directories,
            observation.synthetic.objects,
            observation.synthetic.streams,
            SaturationPrefix(observation.synthetic.stream_bytes),
            observation.synthetic.stream_bytes,
            observation.synthetic.failures,
            observation.real.directories,
            observation.real.objects,
            observation.real.streams,
            SaturationPrefix(observation.real.stream_bytes),
            observation.real.stream_bytes,
            observation.real.failures);
        if (observation.plan_ready) {
            devicefs::WriteToStream(std::cout,
                "    Content available: {} bytes\n"
                "    Content selected: {} bytes ({:.2f}%)\n"
                "    Content compared: {} bytes "
                "({:.2f}% of selection)\n",
                observation.available_bytes,
                observation.selected_bytes,
                Percentage(observation.selected_bytes,
                    observation.available_bytes),
                observation.compared_bytes,
                Percentage(observation.compared_bytes,
                    observation.selected_bytes));
        } else {
            devicefs::WriteToStream(std::cout,
                "    Content comparison was not reached.\n");
        }
        devicefs::WriteToStream(std::cout,
            "    Mismatches: {}\n"
            "    VSS exclusions: {}\n"
            "    Matched filesystem errors: {}\n",
            observation.mismatch_count,
            observation.exclusion_count,
            observation.matched_error_count);
        if (!observation.cleanup_failures.empty()) {
            ++cleanup_failed;
            for (const auto &failure : observation.cleanup_failures) {
                devicefs::WriteToStream(std::cout,
                    "    Cleanup failure: {}\n", failure);
            }
        }
        for (const auto &mismatch : mismatches) {
            PrintMismatch(std::cout, job.volume_identifier, mismatch);
        }
        for (const auto &comparison : operation_comparisons) {
            if (!IsMatchedError(comparison)) {
                PrintOperationComparison(
                    std::cout, job.volume_identifier, comparison);
            }
        }
        for (const auto &comparison : operation_comparisons) {
            if (IsMatchedError(comparison)) {
                PrintOperationComparison(
                    std::cout, job.volume_identifier, comparison);
            }
        }
    }
    devicefs::WriteToStream(std::cout,
        "\nSummary: {} verified, {} sample verified, {} mismatched, "
        "{} indeterminate, {} cancelled/incomplete, "
        "{} volume(s) with VSS exclusions, "
        "{} volume(s) with matched filesystem errors, "
        "{} volume(s) optimization unavailable, "
        "{} view-preparation failure(s), and "
        "{} cleanup-failure volume(s).\n",
        verified, sample_verified, mismatched,
        indeterminate, incomplete, exclusion_volumes,
        matched_error_volumes,
        optimization_unavailable,
        preparation_failures, cleanup_failed);
    if ((indeterminate != 0) || (mismatched != 0) ||
        (preparation_failures != 0)) {
        return 1;
    }
    if (incomplete != 0) {
        return internal::kCancelledExitCode;
    }
    if (cleanup_failed != 0) {
        return 1;
    }
    return 0;
}

} // namespace

[[nodiscard]] auto InventoryVhdx(
    const HANDLE cancellation_event,
    const std::wstring_view device) -> int {
    const auto mount_target = internal::TemporaryDeviceFsViewPath();
    const auto sources = std::array{
        internal::DeviceFsSource{
            .name = L"device.vhdx",
            .source = std::wstring{device},
        },
    };
    auto child = internal::DeviceFsChild{
        internal::StartDeviceFs(
            internal::DeviceFsStartRequest{
                .sources = sources,
                .mount_target = mount_target.native(),
                .vhdx = true,
            })};
    if (!internal::WaitForDeviceFs(
            child.Process(), cancellation_event)) {
        child.Stop();
        return internal::kCancelledExitCode;
    }
    const auto preserve_for_manual_mount =
        [&](const std::string_view error) {
            devicefs::WriteToStream(
                std::cout,
                "\nAutomatic VHDX inventory failed: {}\n",
                error);
            devicefs::WriteToStream(std::cout,
                L"The VHDX remains available for manual mounting:\n"
                L"  VHDX file: {}\n"
                L"After unmounting any manual attachment, press Ctrl+C "
                L"to stop devicefs.\n",
                child.Process().readiness_path.native());
            const auto processes = std::array{&child.Process()};
            const auto result =
                internal::WaitForDeviceFsExitOrCancellation(
                    processes, cancellation_event);
            if (result) {
                devicefs::WriteToStream(std::cout,
                    "devicefs exited with code {}.\n",
                    result->exit_code);
            }
            child.Stop();
            return 1;
        };

    auto state = VerificationState{};
    auto io_cancellation = SynchronousIoCancellation{};
    auto virtual_disk_lock = wil::srwlock{};
    const auto vhdx_path = child.Process().readiness_path;

    auto automatic_inventory_finished = false;
    try {
        constexpr auto privilege_names = std::array{
            wil::zwstring_view{SE_BACKUP_NAME},
            wil::zwstring_view{SE_MANAGE_VOLUME_NAME},
        };
        auto privileges = internal::ProcessPrivilegeEnabler{
            GetCurrentProcess(), privilege_names,
            std::string_view{
                "the backup and volume-management privileges"}};
        auto operation = std::async(std::launch::async,
            [&state, &io_cancellation, &virtual_disk_lock,
                vhdx_path, device, cancellation_event] {
                try {
                    auto view = AttachedVhdx::Attach(
                        vhdx_path, "requested device",
                        cancellation_event,
                        virtual_disk_lock, io_cancellation);
                    state.BeginTraversal();
                    devicefs::WriteToStream(
                        std::cout,
                        L"\nBeginning VHDX filesystem inventory:\n"
                        L"  Source device: {}\n"
                        L"  VHDX file: {}\n"
                        L"  Attached volume: {}\n"
                        L"  Traversal workers: {}\n",
                        device, vhdx_path.native(), view.Root(),
                        kVerificationWorkerCount);
                    const auto root = std::wstring{view.Root()};
                    const auto inventory = InventoryFilesystem(
                        root, VerificationEndpoint::Synthetic,
                        cancellation_event, io_cancellation, state);
                    if (inventory) {
                        state.CompleteTraversal();
                    }
                    if (inventory && !inventory->issues.empty()) {
                        const auto &first = inventory->issues.front();
                        throw VerificationFailure{std::format(
                            "ordinary traversal retained {} filesystem-"
                            "operation failure(s); first: {} for {} "
                            "(Windows error {})",
                            inventory->issues.size(),
                            OperationName(first.key.operation),
                            std::filesystem::path{DisplayPath(
                                first.key.path)}.string(),
                            first.failure.error)};
                    }
                    state.SetPhase(VerificationPhase::Detaching);
                    DetachView(view, "requested device", state);
                    state.SetPhase(VerificationPhase::Complete);
                    return inventory.has_value();
                } catch (const std::system_error &error) {
                    if (IsCancellationError(error) &&
                        internal::CancellationRequested(
                            cancellation_event)) {
                        state.SetPhase(VerificationPhase::Complete);
                        return false;
                    }
                    throw;
                }
            });
        auto next_progress =
            std::chrono::steady_clock::now() +
            kProgressReportInterval;
        auto cancellation_failure = DWORD{ERROR_SUCCESS};
        while (operation.wait_for(0s) !=
            std::future_status::ready) {
            if (internal::CancellationRequested(
                    cancellation_event)) {
                RequestPendingIoCancellation(
                    io_cancellation, cancellation_failure);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_progress) {
                const auto observation = state.Observe();
                devicefs::WriteToStream(std::cout,
                    "\nVHDX inventory progress:\n");
                const auto elapsed = observation.traversal_started
                    ? std::chrono::duration<double>(
                        now - *observation.traversal_started).count()
                    : 0.0;
                PrintTraversalProgress(std::cout,
                    "Requested device", observation.synthetic,
                    elapsed);
                next_progress = now + kProgressReportInterval;
            }
            std::this_thread::sleep_for(kReporterPollInterval);
        }
        const auto inventory_complete = operation.get();
        automatic_inventory_finished = true;
        const auto observation = state.Observe();
        devicefs::WriteToStream(std::cout,
            "\nVHDX inventory {}:\n",
            inventory_complete ? "complete" : "cancelled/incomplete");
        const auto elapsed = observation.traversal_started
            ? std::chrono::duration<double>(
                std::chrono::steady_clock::now() -
                *observation.traversal_started).count()
            : 0.0;
        PrintTraversalProgress(std::cout,
            "Requested device", observation.synthetic, elapsed);
        for (const auto &failure : observation.cleanup_failures) {
            devicefs::WriteToStream(
                std::cout, "  Cleanup failure: {}\n", failure);
        }
        privileges.Restore();
        child.Stop();
        if (!inventory_complete) {
            return internal::kCancelledExitCode;
        }
        if (!observation.cleanup_failures.empty()) {
            return 1;
        }
        return 0;
    } catch (const VerificationFailure &error) {
        if (automatic_inventory_finished) {
            throw;
        }
        return preserve_for_manual_mount(error.what());
    } catch (const std::system_error &error) {
        if (automatic_inventory_finished) {
            throw;
        }
        return preserve_for_manual_mount(error.what());
    }
}

export [[nodiscard]] auto VerifyFilesystemViews(
    const HANDLE cancellation_event,
    const std::span<const FilesystemVerificationVolume> volumes,
    const std::wstring_view synthetic_mount,
    const std::wstring_view real_mount,
    const double percentage,
    const std::size_t optimization_unavailable,
    const std::size_t preparation_failures) -> int {
    if (volumes.empty()) {
        const auto jobs = std::vector<VolumeJob>{};
        return PrintFinalResults(
            jobs, optimization_unavailable, preparation_failures);
    }
    if (internal::CancellationRequested(cancellation_event)) {
        auto jobs = std::vector<VolumeJob>{};
        jobs.reserve(volumes.size());
        for (const auto &volume : volumes) {
            jobs.push_back({
                .volume_identifier = volume.volume_identifier,
                .percentage = percentage,
                .state = std::make_unique<VerificationState>(),
            });
        }
        return PrintFinalResults(
            jobs, optimization_unavailable, preparation_failures);
    }

    constexpr auto privilege_names = std::array{
        wil::zwstring_view{SE_BACKUP_NAME},
        wil::zwstring_view{SE_MANAGE_VOLUME_NAME},
    };
    auto privileges = internal::ProcessPrivilegeEnabler{
        GetCurrentProcess(), privilege_names,
        std::string_view{
            "the backup and volume-management privileges"}};

    auto io_cancellation = SynchronousIoCancellation{};
    // Every volume job borrows this one lock, so attach, discovery, and detach
    // operations cannot overlap one another.
    auto virtual_disk_lock = wil::srwlock{};
    auto jobs = std::vector<VolumeJob>{};
    jobs.reserve(volumes.size());
    for (const auto &volume : volumes) {
        auto state = std::make_unique<VerificationState>();
        auto *const borrowed_state = state.get();
        auto synthetic_vhdx =
            std::filesystem::path{synthetic_mount} / volume.filename;
        auto real_vhdx =
            std::filesystem::path{real_mount} / volume.filename;
        auto operation = [&]() -> std::future<void> {
            try {
                return std::async(std::launch::async,
                    [volume, synthetic_vhdx = std::move(synthetic_vhdx),
                        real_vhdx = std::move(real_vhdx),
                        cancellation_event, percentage, borrowed_state,
                        &virtual_disk_lock, &io_cancellation] {
                        try {
                            VerifyVolume(volume,
                                synthetic_vhdx, real_vhdx,
                                cancellation_event, percentage,
                                virtual_disk_lock,
                                io_cancellation,
                                *borrowed_state);
                        } catch (const VerificationFailure &error) {
                            borrowed_state->Fail(error.what());
                            borrowed_state->SetPhase(
                                VerificationPhase::Complete);
                        } catch (const std::system_error &error) {
                            if (!(IsCancellationError(error) &&
                                internal::CancellationRequested(
                                    cancellation_event))) {
                                borrowed_state->Fail(error.what());
                            }
                            borrowed_state->SetPhase(
                                VerificationPhase::Complete);
                        }
                    });
            } catch (const std::system_error &error) {
                state->Fail(error.what());
                state->SetPhase(VerificationPhase::Complete);
                return {};
            }
        }();
        jobs.push_back({
            .volume_identifier = volume.volume_identifier,
            .percentage = percentage,
            .state = std::move(state),
            .operation = std::move(operation),
        });
    }

    auto next_progress =
        std::chrono::steady_clock::now() +
        kProgressReportInterval;
    auto cancellation_failure = DWORD{ERROR_SUCCESS};
    while (!JobsReady(jobs)) {
        if (internal::CancellationRequested(cancellation_event)) {
            RequestPendingIoCancellation(
                io_cancellation, cancellation_failure);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_progress) {
            PrintNewMismatches(jobs, std::cout);
            PrintProgress(jobs);
            next_progress = now + kProgressReportInterval;
        }
        std::this_thread::sleep_for(kReporterPollInterval);
    }
    for (auto &job : jobs) {
        if (job.operation.valid()) {
            job.operation.get();
        }
    }
    const auto result = PrintFinalResults(
        jobs, optimization_unavailable, preparation_failures);
    if (result != 0) {
        return result;
    }
    privileges.Restore();
    return 0;
}
