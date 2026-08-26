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
import <cstddef>;
import <devicefs/windows_imports.h>;
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

// Four workers keep independent reads in flight on each volume without
// allowing one volume to consume the workers assigned to another drive.
constexpr auto kVerificationWorkerCount = 4uz;
constexpr auto kReadChunkSize = 4uz * 1024 * 1024;
// FSCTL_QUERY_FILE_LAYOUT has no required-size probe and returns only complete
// file entries. A 4-MiB batch keeps the number of volume queries low while
// remaining small beside the cached inventory and comparison-worker buffers.
constexpr auto kLayoutQueryBatchSize = 4uz * 1024 * 1024;
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
    InventoryingSynthetic,
    InventoryingReal,
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
    OpenStream,
    SeekStream,
    ReadStream,
};

using FileIdentity = std::uint64_t;

struct StreamRecord {
    std::wstring name;
    std::int64_t length;
};

struct ObjectRecord {
    std::wstring relative_path;
    ObjectKind kind;
    FileIdentity identity;
    std::vector<StreamRecord> streams;
};

struct OperationFailure {
    DWORD error;
    DWORD transferred = 0;

    auto operator<=>(const OperationFailure &) const = default;
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

[[nodiscard]] auto FailuresMatch(
    const std::optional<OperationFailure> &synthetic,
    const std::optional<OperationFailure> &real) noexcept {
    return synthetic && real && (*synthetic == *real);
}

[[nodiscard]] auto IsMatchedError(
    const OperationComparison &comparison) noexcept {
    return FailuresMatch(comparison.synthetic, comparison.real);
}

struct FilesystemInventory {
    std::filesystem::path root;
    std::vector<ObjectRecord> objects;
};

struct LayoutObject {
    FileIdentity identity;
    ObjectKind kind;
    std::vector<std::wstring> paths;
    std::vector<StreamRecord> streams;
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

struct VerificationObservation {
    VerificationPhase phase = VerificationPhase::AttachingSynthetic;
    std::uint64_t synthetic_layout_entries = 0;
    std::uint64_t real_layout_entries = 0;
    std::uint64_t synthetic_objects = 0;
    std::uint64_t synthetic_streams = 0;
    std::uint64_t real_objects = 0;
    std::uint64_t real_streams = 0;
    std::uint64_t available_bytes = 0;
    std::uint64_t selected_bytes = 0;
    std::uint64_t compared_bytes = 0;
    std::optional<std::chrono::steady_clock::time_point>
        comparison_started;
    std::size_t mismatch_count = 0;
    std::size_t matched_error_count = 0;
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

class VerificationState {
  public:
    auto RecordLayoutEntries(
        const VerificationEndpoint endpoint,
        const std::size_t entry_count) noexcept -> void {
        auto &entries = endpoint == VerificationEndpoint::Synthetic
            ? synthetic_layout_entries_ : real_layout_entries_;
        entries.fetch_add(entry_count, std::memory_order_relaxed);
    }

    auto SetPhase(const VerificationPhase phase) noexcept -> void {
        phase_.store(phase, std::memory_order_release);
    }

    auto RecordInventory(
        const VerificationEndpoint endpoint,
        const std::size_t stream_count) noexcept -> void {
        auto &objects = endpoint == VerificationEndpoint::Synthetic
            ? synthetic_objects_ : real_objects_;
        auto &streams = endpoint == VerificationEndpoint::Synthetic
            ? synthetic_streams_ : real_streams_;
        objects.fetch_add(1, std::memory_order_relaxed);
        streams.fetch_add(stream_count, std::memory_order_relaxed);
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
            .synthetic_layout_entries =
                synthetic_layout_entries_.load(std::memory_order_relaxed),
            .real_layout_entries =
                real_layout_entries_.load(std::memory_order_relaxed),
            .synthetic_objects =
                synthetic_objects_.load(std::memory_order_relaxed),
            .synthetic_streams =
                synthetic_streams_.load(std::memory_order_relaxed),
            .real_objects =
                real_objects_.load(std::memory_order_relaxed),
            .real_streams =
                real_streams_.load(std::memory_order_relaxed),
            .available_bytes =
                available_bytes_.load(std::memory_order_relaxed),
            .selected_bytes =
                selected_bytes_.load(std::memory_order_relaxed),
            .compared_bytes =
                compared_bytes_.load(std::memory_order_relaxed),
            .plan_ready = plan_ready,
            .comparison_complete = comparison_complete,
        };
        const auto lock = std::scoped_lock{mutex_};
        result.comparison_started = comparison_started_;
        result.mismatch_count = mismatches_.size() +
            std::ranges::count_if(operation_comparison_records_,
                [](const OperationComparison &comparison) {
                    return !IsMatchedError(comparison);
                });
        result.matched_error_count = std::ranges::count_if(
            operation_comparison_records_, IsMatchedError);
        result.failed = failed_.load(std::memory_order_acquire);
        result.failure = failure_;
        result.cleanup_failures = cleanup_failures_;
        return result;
    }

  private:
    std::atomic<VerificationPhase> phase_{
        VerificationPhase::AttachingSynthetic};
    std::atomic<std::uint64_t> synthetic_layout_entries_{};
    std::atomic<std::uint64_t> real_layout_entries_{};
    std::atomic<std::uint64_t> synthetic_objects_{};
    std::atomic<std::uint64_t> synthetic_streams_{};
    std::atomic<std::uint64_t> real_objects_{};
    std::atomic<std::uint64_t> real_streams_{};
    std::atomic<std::uint64_t> available_bytes_{};
    std::atomic<std::uint64_t> selected_bytes_{};
    std::atomic<std::uint64_t> compared_bytes_{};
    std::atomic<bool> plan_ready_{};
    std::atomic<bool> comparison_complete_{};
    std::atomic<bool> failed_{};
    mutable std::mutex mutex_;
    std::string failure_;
    std::optional<std::chrono::steady_clock::time_point>
        comparison_started_;
    std::vector<std::string> cleanup_failures_;
    std::vector<MismatchDetail> mismatches_;
    std::map<MismatchKey, std::size_t> content_mismatches_;
    std::set<OperationComparison> operation_comparisons_;
    std::vector<OperationComparison> operation_comparison_records_;
};

using UniqueFindVolume = wil::unique_any_handle_invalid<
    decltype(&FindVolumeClose), FindVolumeClose>;

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

auto WaitForVolumes(const HANDLE disk) -> void {
    // Microsoft documents IOCTL_DISK_ARE_VOLUMES_READY for this operation,
    // but the Windows 10.0.26100 user-mode SDK omits its definition from
    // ntdddisk.h. This is the corresponding published control code.
    // See <https://learn.microsoft.com/en-us/windows/win32/fileio/ioctl-disk-are-volumes-ready>.
    constexpr auto kDiskAreVolumesReady = DWORD{CTL_CODE(
        FILE_DEVICE_DISK, 0x0087, METHOD_BUFFERED, FILE_READ_ACCESS)};
    auto returned = DWORD{};
    if (!DeviceIoControl(disk, kDiskAreVolumesReady,
            nullptr, 0, nullptr, 0, &returned, nullptr)) {
        WinError("the attached VHDX volumes did not become ready");
    }
}

[[nodiscard]] auto TryQueryVolumeDiskNumber(
    const std::wstring_view volume) -> std::optional<DWORD> {
    auto path = std::wstring{volume};
    path.pop_back();
    auto handle = wil::unique_hfile{CreateFileW(
        path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!handle) {
        return std::nullopt;
    }
    auto extents = VOLUME_DISK_EXTENTS{};
    auto returned = DWORD{};
    if (!DeviceIoControl(handle.get(),
            IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
            nullptr, 0, &extents, sizeof(extents), &returned, nullptr)) {
        return std::nullopt;
    }
    return extents.Extents[0].DiskNumber;
}

[[nodiscard]] auto FindVolumeRoot(const DWORD disk_number) {
    auto volume = std::array<wchar_t, MAX_PATH>{};
    auto search = UniqueFindVolume{FindFirstVolumeW(
        volume.data(), wil::safe_cast_failfast<DWORD>(volume.size()))};
    if (!search) {
        WinError("could not begin enumerating volumes for an attached VHDX");
    }
    while (true) {
        const auto name = std::wstring_view{volume.data()};
        if (TryQueryVolumeDiskNumber(name) == disk_number) {
            return std::wstring{name};
        }
        if (FindNextVolumeW(search.get(), volume.data(),
                wil::safe_cast_failfast<DWORD>(volume.size()))) {
            continue;
        }
        const auto error = GetLastError();
        if (error == ERROR_NO_MORE_FILES) {
            break;
        }
        WinError("could not continue enumerating volumes for an attached VHDX",
            ExplicitWin32Error{error});
    }
    throw VerificationFailure(
        "could not locate the volume belonging to an attached VHDX");
}

class AttachedVhdx {
  public:
    [[nodiscard]] static auto Attach(
        const std::filesystem::path &path,
        const std::string_view name) {
        try {
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

            auto attach_parameters = ATTACH_VIRTUAL_DISK_PARAMETERS{
                .Version = ATTACH_VIRTUAL_DISK_VERSION_1,
            };
            constexpr auto kAttachFlags =
                ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY |
                ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER;
            const auto attach_status = AttachVirtualDisk(
                disk.get(), nullptr,
                static_cast<ATTACH_VIRTUAL_DISK_FLAG>(kAttachFlags),
                0, &attach_parameters, nullptr);
            if (attach_status != ERROR_SUCCESS) {
                WinError("could not attach a filesystem-verification VHDX",
                    ExplicitWin32Error{attach_status});
            }

            const auto physical_path = QueryPhysicalDiskPath(disk.get());
            auto physical_disk = OpenPhysicalDisk(physical_path);
            WaitForVolumes(physical_disk.get());
            auto root = FindVolumeRoot(
                QueryDiskNumber(physical_disk.get()));
            return AttachedVhdx{std::move(disk), std::move(root)};
        } catch (const VerificationFailure &error) {
            throw VerificationFailure(std::format(
                "could not prepare the {} VHDX view: {}",
                name, error.what()));
        } catch (const std::system_error &error) {
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
            static_cast<void>(DetachVirtualDisk(
                disk_.get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0));
        }
    }

    [[nodiscard]] auto Root() const noexcept
        -> std::wstring_view {
        return root_;
    }

    [[nodiscard]] auto IsLoaded() const {
        auto information = GET_VIRTUAL_DISK_INFO{
            .Version = GET_VIRTUAL_DISK_INFO_IS_LOADED,
        };
        auto bytes = ULONG{sizeof(information)};
        const auto status = GetVirtualDiskInformation(
            disk_.get(), &bytes, &information, nullptr);
        if (status != ERROR_SUCCESS) {
            WinError("could not determine whether the VHDX is mounted",
                ExplicitWin32Error{status});
        }
        return information.IsLoaded != FALSE;
    }

    [[nodiscard]] auto Detach() noexcept {
        const auto status = DetachVirtualDisk(
            disk_.get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        if (status == ERROR_SUCCESS) {
            disk_.reset();
        }
        return status;
    }

  private:
    AttachedVhdx(
        wil::unique_handle disk,
        std::wstring root) noexcept
        : disk_{std::move(disk)}, root_{std::move(root)} {}

    wil::unique_handle disk_;
    std::wstring root_;
};

auto DetachView(
    AttachedVhdx &view,
    const std::string_view name,
    VerificationState &state) noexcept -> void {
    const auto status = view.Detach();
    if (status != ERROR_SUCCESS) {
        try {
            state.RecordCleanupFailure(std::format(
                "could not detach the {} VHDX view (Windows error {})",
                name, status));
        } catch (const std::bad_alloc &) {
            // Cleanup reporting must not replace the verification result.
        }
    }
}

[[nodiscard]] auto TryOpenFilesystemObject(
    const std::filesystem::path &path) noexcept
    -> std::expected<wil::unique_hfile, OperationFailure> {
    auto result = wil::unique_hfile{CreateFileW(
        path.c_str(), GENERIC_READ, kShareMode, nullptr, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION |
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
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

// NTFS file references use the low 48 bits for the MFT segment number. NTFS
// reserves the first 16 MFT records; record 5 is the root directory of the
// ordinary namespace. See
// <https://learn.microsoft.com/en-us/windows/win32/devnotes/mft-segment-reference>
// and
// <https://learn.microsoft.com/en-us/windows/win32/devnotes/master-file-table>.
constexpr auto kMftSegmentNumberMask =
    (std::uint64_t{1} << 48) - 1;
constexpr auto kNtfsRootSegmentNumber = std::uint64_t{5};
constexpr auto kFirstOrdinaryNtfsSegmentNumber = std::uint64_t{16};

// NTFS attribute type 0x80 is $DATA. FSCTL_QUERY_FILE_LAYOUT also returns
// other attribute streams, which are not part of the verifier's ordinary
// stream surface. See
// <https://learn.microsoft.com/en-us/windows/win32/devnotes/attribute-record-header>.
constexpr auto kNtfsDataAttributeType = DWORD{0x80};

[[nodiscard]] constexpr auto MftSegmentNumber(
    const FileIdentity identity) noexcept {
    return identity & kMftSegmentNumberMask;
}

template <typename Entry>
[[nodiscard]] auto LayoutEntryAt(
    const std::span<const std::byte> buffer,
    const std::size_t offset) noexcept {
    const auto storage = buffer.subspan(offset, sizeof(Entry));
    auto result = Entry{};
    std::memcpy(std::addressof(result), storage.data(), storage.size());
    return result;
}

[[nodiscard]] auto LayoutString(
    const std::span<const std::byte> buffer,
    const std::size_t offset,
    const DWORD length) {
    if (length == 0) {
        return std::wstring{};
    }
    [[gsl::suppress("26493",
        justification:
            "Braced initialization proves this construction safe at compile time.")]]
    const auto count = std::size_t{length} /
        sizeof(std::wstring::value_type);
    const auto storage = buffer.subspan(offset, length);
    std::wstring result(count, L'\0');
    std::memcpy(result.data(), storage.data(), storage.size());
    return result;
}

auto AppendLayoutBatch(
    const std::span<const std::byte> buffer,
    std::vector<LayoutObject> &objects,
    const HANDLE cancellation_event) -> bool {
    const auto output =
        LayoutEntryAt<QUERY_FILE_LAYOUT_OUTPUT>(buffer, 0);
    [[gsl::suppress("26493",
        justification:
            "Braced initialization proves this construction safe at compile time.")]]
    auto file_offset = std::size_t{output.FirstFileOffset};
    for (auto file_index = DWORD{};
        file_index < output.FileEntryCount; ++file_index) {
        if (internal::CancellationRequested(cancellation_event)) {
            return false;
        }
        const auto file =
            LayoutEntryAt<FILE_LAYOUT_ENTRY>(buffer, file_offset);
        auto object = LayoutObject{
            .identity = file.FileReferenceNumber,
            .kind = ClassifyObject(file.FileAttributes),
        };

        if (MftSegmentNumber(object.identity) ==
            kNtfsRootSegmentNumber) {
            object.paths.emplace_back();
        } else if (file.FirstNameOffset != 0) {
            auto name_offset = file_offset + file.FirstNameOffset;
            while (true) {
                const auto name = LayoutEntryAt<FILE_LAYOUT_NAME_ENTRY>(
                    buffer, name_offset);
                const auto dos =
                    (name.Flags & FILE_LAYOUT_NAME_ENTRY_DOS) != 0;
                const auto primary =
                    (name.Flags & FILE_LAYOUT_NAME_ENTRY_PRIMARY) != 0;
                if (!dos || primary) {
                    const auto path = std::filesystem::path{LayoutString(
                        buffer,
                        name_offset +
                            offsetof(FILE_LAYOUT_NAME_ENTRY, FileName),
                        name.FileNameLength)};
                    object.paths.push_back(
                        path.relative_path().native());
                }
                if (name.NextNameOffset == 0) {
                    break;
                }
                name_offset += name.NextNameOffset;
            }
        }

        if (file.FirstStreamOffset != 0) {
            auto stream_offset = file_offset + file.FirstStreamOffset;
            while (true) {
                const auto stream = LayoutEntryAt<STREAM_LAYOUT_ENTRY>(
                    buffer, stream_offset);
                if (stream.AttributeTypeCode ==
                    kNtfsDataAttributeType) {
                    auto name = LayoutString(
                        buffer,
                        stream_offset + offsetof(
                            STREAM_LAYOUT_ENTRY, StreamIdentifier),
                        stream.StreamIdentifierLength);
                    if (name == L"::$DATA") {
                        name.clear();
                    }
                    object.streams.push_back({
                        .name = std::move(name),
                        .length = stream.EndOfFile.QuadPart,
                    });
                }
                if (stream.NextStreamOffset == 0) {
                    break;
                }
                stream_offset += stream.NextStreamOffset;
            }
        }
        std::ranges::sort(
            object.streams, {}, &StreamRecord::name);
        objects.push_back(std::move(object));

        if (file.NextFileOffset != 0) {
            file_offset += file.NextFileOffset;
        }
    }
    return true;
}

[[nodiscard]] auto IsInExcludedSubtree(
    const std::wstring_view path,
    const std::vector<std::wstring> &prefixes) {
    const auto found = std::ranges::upper_bound(prefixes, path);
    return (found != prefixes.begin()) &&
        path.starts_with(*std::prev(found));
}

[[nodiscard]] auto InventoryFilesystem(
    const std::wstring_view root,
    const VerificationEndpoint endpoint,
    const HANDLE cancellation_event,
    VerificationState &state)
    -> std::optional<FilesystemInventory> {
    auto volume_path = std::wstring{root};
    volume_path.pop_back();
    auto volume = wil::unique_hfile{CreateFileW(
        volume_path.c_str(), GENERIC_READ, kShareMode, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!volume) {
        WinError("could not open the attached volume for filesystem inventory");
    }

    auto query = QUERY_FILE_LAYOUT_INPUT{
        .FilterEntryCount = 0,
        .Flags = QUERY_FILE_LAYOUT_RESTART |
            QUERY_FILE_LAYOUT_INCLUDE_NAMES |
            QUERY_FILE_LAYOUT_INCLUDE_STREAMS |
            QUERY_FILE_LAYOUT_INCLUDE_STREAMS_WITH_NO_CLUSTERS_ALLOCATED |
            QUERY_FILE_LAYOUT_INCLUDE_FULL_PATH_IN_NAMES,
        .FilterType = QUERY_FILE_LAYOUT_FILTER_TYPE_NONE,
    };
    static_assert(
        (std::numeric_limits<std::size_t>::max() / 2) >=
        std::numeric_limits<DWORD>::max());
    [[gsl::suppress("26493",
        justification:
            "Braced initialization proves this construction safe at compile time.")]]
    constexpr auto kMaximumBufferSize =
        std::size_t{std::numeric_limits<DWORD>::max()};
    auto storage = std::vector<std::byte>(kLayoutQueryBatchSize);
    auto objects = std::vector<LayoutObject>{};

    while (true) {
        if (internal::CancellationRequested(cancellation_event)) {
            return std::nullopt;
        }
        const auto buffer = std::span{storage};
        // The buffer is capped by the DWORD-sized DeviceIoControl parameter.
        const auto buffer_size_for_api =
            wil::safe_cast_failfast<DWORD>(buffer.size());
        auto returned = DWORD{};
        if (DeviceIoControl(volume.get(), FSCTL_QUERY_FILE_LAYOUT,
                &query, sizeof(query), buffer.data(), buffer_size_for_api,
                &returned, nullptr)) {
            const auto previous_size = objects.size();
            if (!AppendLayoutBatch(
                    buffer.first(returned), objects,
                    cancellation_event)) {
                return std::nullopt;
            }
            state.RecordLayoutEntries(
                endpoint, objects.size() - previous_size);
            query.Flags &= ~DWORD{QUERY_FILE_LAYOUT_RESTART};
            continue;
        }
        const auto error = GetLastError();
        if (error == ERROR_HANDLE_EOF) {
            break;
        }
        if (error == ERROR_INSUFFICIENT_BUFFER) {
            if (storage.size() == kMaximumBufferSize) {
                throw VerificationFailure(
                    "a filesystem layout entry exceeds the maximum "
                    "DeviceIoControl buffer size");
            }
            storage.resize(std::min(
                kMaximumBufferSize, storage.size() * 2));
            continue;
        }
        WinError("could not query the attached filesystem layout",
            ExplicitWin32Error{error});
    }

    auto excluded_subtree_prefixes = std::vector<std::wstring>{};
    for (const auto &object : objects) {
        if (internal::CancellationRequested(cancellation_event)) {
            return std::nullopt;
        }
        const auto segment = MftSegmentNumber(object.identity);
        const auto reserved =
            (segment < kFirstOrdinaryNtfsSegmentNumber) &&
            (segment != kNtfsRootSegmentNumber);
        if (reserved ||
            (object.kind == ObjectKind::DirectoryReparsePoint)) {
            for (const auto &path : object.paths) {
                auto prefix = path;
                prefix.push_back(L'\\');
                excluded_subtree_prefixes.push_back(
                    std::move(prefix));
            }
        }
    }
    std::ranges::sort(excluded_subtree_prefixes);
    auto minimal_prefixes = std::vector<std::wstring>{};
    for (auto &prefix : excluded_subtree_prefixes) {
        // With a trailing separator, a nested prefix sorts contiguously
        // after its ancestor and adds no excluded paths.
        if (minimal_prefixes.empty() ||
            !prefix.starts_with(minimal_prefixes.back())) {
            minimal_prefixes.push_back(std::move(prefix));
        }
    }
    excluded_subtree_prefixes = std::move(minimal_prefixes);

    auto result = FilesystemInventory{
        .root = std::filesystem::path{root},
    };
    for (auto &object : objects) {
        if (internal::CancellationRequested(cancellation_event)) {
            return std::nullopt;
        }
        const auto segment = MftSegmentNumber(object.identity);
        if ((segment < kFirstOrdinaryNtfsSegmentNumber) &&
            (segment != kNtfsRootSegmentNumber)) {
            continue;
        }
        for (auto &path : object.paths) {
            if (IsInExcludedSubtree(
                    path, excluded_subtree_prefixes)) {
                continue;
            }
            state.RecordInventory(endpoint, object.streams.size());
            result.objects.push_back({
                .relative_path = std::move(path),
                .kind = object.kind,
                .identity = object.identity,
                .streams = object.streams,
            });
        }
    }
    std::ranges::sort(
        result.objects, {}, &ObjectRecord::relative_path);
    return result;
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
    // Namespace, object type, stream presence, and stream length are always
    // compared. The percentage independently selects ordinary stream-content
    // chunks, with B's snapshot ID making the sample repeatable for this run.
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
    while ((synthetic_object < synthetic.objects.size()) ||
        (real_object < real.objects.size())) {
        if (internal::CancellationRequested(cancellation_event)) {
            return std::optional<ComparisonPlan>{};
        }
        if (synthetic_object == synthetic.objects.size()) {
            const auto &entry = real.objects[real_object++];
            AddObjectMismatch(state,
                MismatchKind::ObjectMissingFromSynthetic,
                entry);
            continue;
        }
        if (real_object == real.objects.size()) {
            const auto &entry = synthetic.objects[synthetic_object++];
            AddObjectMismatch(state,
                MismatchKind::ObjectMissingFromReal,
                entry);
            continue;
        }
        const auto &synthetic_entry =
            synthetic.objects[synthetic_object];
        const auto &real_entry = real.objects[real_object];
        if (synthetic_entry.relative_path < real_entry.relative_path) {
            AddObjectMismatch(state,
                MismatchKind::ObjectMissingFromReal,
                synthetic_entry);
            ++synthetic_object;
            continue;
        }
        if (real_entry.relative_path < synthetic_entry.relative_path) {
            AddObjectMismatch(state,
                MismatchKind::ObjectMissingFromSynthetic,
                real_entry);
            ++real_object;
            continue;
        }
        if (synthetic_entry.kind != real_entry.kind) {
            AddTypeMismatch(state, synthetic_entry, real_entry);
        } else {
            // Hard-link names remain separate namespace entries, but equal
            // paired file identities expose the same streams and need only one
            // content comparison.
            const auto compare_contents =
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

[[nodiscard]] auto ObjectPath(
    const FilesystemInventory &inventory,
    const ObjectRecord &object) {
    return object.relative_path.empty()
        ? inventory.root
        : inventory.root / object.relative_path;
}

[[nodiscard]] auto StreamPath(
    const FilesystemInventory &inventory,
    const ObjectRecord &object,
    const StreamRecord &stream) {
    auto path = ObjectPath(inventory, object);
    if (!stream.name.empty()) {
        path = std::filesystem::path{
            std::format(L"{}{}", path.native(), stream.name)};
    }
    return path;
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
        VerificationState &state)
        : synthetic_{synthetic}, real_{real}, state_{state},
          synthetic_buffer_(kReadChunkSize),
          real_buffer_(kReadChunkSize) {}

    [[nodiscard]] auto Compare(const ReadTask &task) -> bool {
        const auto key = std::array{
            task.synthetic_object, task.real_object,
            task.synthetic_stream, task.real_stream};
        if (!current_stream_ || (*current_stream_ != key)) {
            current_stream_ = key;
            current_stream_readable_ = OpenStream(task);
        }
        if (!current_stream_readable_) {
            return current_stream_open_failure_matched_;
        }

        const auto &synthetic_object =
            synthetic_.objects[task.synthetic_object];
        const auto &synthetic_stream =
            synthetic_object.streams[task.synthetic_stream];
        const auto synthetic_seek = TrySeekStream(
            synthetic_handle_.get(), task.offset);
        const auto real_seek = TrySeekStream(
            real_handle_.get(), task.offset);
        if (synthetic_seek || real_seek) {
            RecordOperation(task, synthetic_object, synthetic_stream,
                FilesystemOperation::SeekStream, 0,
                synthetic_seek, real_seek);
            return FailuresMatch(synthetic_seek, real_seek);
        }
        const auto synthetic_chunk =
            std::span{synthetic_buffer_}.first(task.length);
        const auto real_chunk =
            std::span{real_buffer_}.first(task.length);
        const auto synthetic_read = TryReadStreamChunk(
            synthetic_handle_.get(), synthetic_chunk);
        const auto real_read = TryReadStreamChunk(
            real_handle_.get(), real_chunk);
        auto compared_length = task.length;
        if (synthetic_read || real_read) {
            RecordOperation(task, synthetic_object, synthetic_stream,
                FilesystemOperation::ReadStream,
                wil::safe_cast_failfast<DWORD>(task.length),
                synthetic_read, real_read);
            if (!synthetic_read || !real_read ||
                (*synthetic_read != *real_read)) {
                return false;
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
        current_stream_open_failure_matched_ = false;
        const auto &synthetic_object =
            synthetic_.objects[task.synthetic_object];
        const auto &real_object = real_.objects[task.real_object];
        const auto &synthetic_stream =
            synthetic_object.streams[task.synthetic_stream];
        const auto &real_stream =
            real_object.streams[task.real_stream];
        auto synthetic_handle = TryOpenFilesystemObject(
            StreamPath(synthetic_, synthetic_object, synthetic_stream));
        auto real_handle = TryOpenFilesystemObject(
            StreamPath(real_, real_object, real_stream));
        if (!synthetic_handle || !real_handle) {
            current_stream_open_failure_matched_ = FailuresMatch(
                synthetic_handle
                    ? std::nullopt
                    : std::optional{synthetic_handle.error()},
                real_handle
                    ? std::nullopt
                    : std::optional{real_handle.error()});
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
    VerificationState &state_;
    std::vector<unsigned char> synthetic_buffer_;
    std::vector<unsigned char> real_buffer_;
    std::optional<std::array<std::size_t, 4>> current_stream_;
    bool current_stream_readable_ = false;
    bool current_stream_open_failure_matched_ = false;
    wil::unique_hfile synthetic_handle_;
    wil::unique_hfile real_handle_;
};

auto RunComparisonWorkers(
    const ComparisonPlan &plan,
    const FilesystemInventory &synthetic,
    const FilesystemInventory &real,
    const HANDLE cancellation_event,
    VerificationState &state) -> bool {
    auto next_task = std::atomic<std::size_t>{};
    auto completed_tasks = std::atomic<std::size_t>{};
    const auto run = [&] {
        auto worker = ComparisonWorker{synthetic, real, state};
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
        std::ranges::all_of(
            state.OperationComparisonsAfterJoin(), IsMatchedError) &&
        (completed_tasks.load(std::memory_order_relaxed) ==
            plan.tasks.size());
}

auto CompareAttachedFilesystems(
    const FilesystemVerificationVolume &volume,
    const AttachedVhdx &synthetic_attachment,
    const AttachedVhdx &real_attachment,
    const HANDLE cancellation_event,
    const double percentage,
    VerificationState &state) -> void {
    if (internal::CancellationRequested(cancellation_event)) {
        return;
    }
    state.SetPhase(VerificationPhase::InventoryingSynthetic);
    auto synthetic = InventoryFilesystem(
        synthetic_attachment.Root(), VerificationEndpoint::Synthetic,
        cancellation_event, state);
    auto real = [&]() -> std::optional<FilesystemInventory> {
        if (!synthetic || state.Failed() ||
            internal::CancellationRequested(cancellation_event)) {
            return std::nullopt;
        }
        state.SetPhase(VerificationPhase::InventoryingReal);
        return InventoryFilesystem(
            real_attachment.Root(), VerificationEndpoint::Real,
            cancellation_event, state);
    }();
    if (synthetic && real && !state.Failed()) {
        state.SetPhase(VerificationPhase::Planning);
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
                    cancellation_event, state)) {
                state.CompleteComparison();
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
    VerificationState &state) -> void {
    if (internal::CancellationRequested(cancellation_event)) {
        return;
    }
    state.SetPhase(VerificationPhase::AttachingSynthetic);
    auto synthetic_attachment =
        AttachedVhdx::Attach(synthetic_vhdx, "synthetic");
    if (internal::CancellationRequested(cancellation_event)) {
        state.SetPhase(VerificationPhase::Detaching);
        DetachView(synthetic_attachment, "synthetic", state);
        state.SetPhase(VerificationPhase::Complete);
        return;
    }
    try {
        state.SetPhase(VerificationPhase::AttachingReal);
        auto real_attachment =
            AttachedVhdx::Attach(real_vhdx, "real B");
        try {
            CompareAttachedFilesystems(volume,
                synthetic_attachment, real_attachment,
                cancellation_event, percentage, state);
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
    case VerificationPhase::InventoryingSynthetic:
        return "inventorying the synthetic filesystem";
    case VerificationPhase::InventoryingReal:
        return "inventorying the real-B filesystem";
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
    } else {
        devicefs::WriteToStream(output,
            "\n*** FILESYSTEM VERIFICATION MISMATCH ***\n");
    }
    devicefs::WriteToStream(output,
        L"  Volume ID: {}\n"
        L"  Object: {}\n",
        std::wstring_view{identifier}, DisplayPath(comparison.key.path));
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
    devicefs::WriteToStream(output,
        "\n*** FILESYSTEM VERIFICATION MISMATCH ***\n");
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
    auto wrote = false;
    for (auto &job : jobs) {
        for (const auto &mismatch :
            job.state->NewMismatches(job.printed_mismatches)) {
            PrintMismatch(output, job.volume_identifier, mismatch);
            wrote = true;
        }
        for (const auto &comparison :
            job.state->NewOperationComparisons(
                job.printed_operation_comparisons)) {
            if (!IsMatchedError(comparison)) {
                PrintOperationComparison(
                    output, job.volume_identifier, comparison);
                wrote = true;
            }
        }
    }
    if (wrote) {
        output.flush();
    }
}

[[nodiscard]] constexpr auto Percentage(
    const std::uint64_t completed,
    const std::uint64_t total) noexcept {
    return total == 0 ? 100.0 :
        (static_cast<double>(completed) /
            static_cast<double>(total)) * 100.0;
}

auto PrintProgress(const std::span<VolumeJob> jobs) -> void {
    auto &output = devicefs::WriteToStream(
        std::cout, "\nFilesystem verification progress:\n");
    for (const auto &job : jobs) {
        const auto observation = job.state->Observe();
        const auto identifier =
            winrt::to_hstring(job.volume_identifier);
        devicefs::WriteToStream(output,
            L"  Volume ID: {}\n",
            std::wstring_view{identifier});
        devicefs::WriteToStream(output,
            "    Phase: {}\n"
            "    Layout entries received: synthetic {}; real B {}\n"
            "    Namespace inventory: synthetic {} object(s), {} "
            "stream(s); real B {} object(s), {} stream(s)\n",
            PhaseName(observation.phase),
            observation.synthetic_layout_entries,
            observation.real_layout_entries,
            observation.synthetic_objects,
            observation.synthetic_streams,
            observation.real_objects,
            observation.real_streams);
        if (observation.plan_ready) {
            devicefs::WriteToStream(output,
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
            devicefs::WriteToStream(output,
                "    Comparison rate: {:.2f} MiB/s\n",
                mebibytes_per_second);
        }
        devicefs::WriteToStream(output,
            "    Mismatches observed: {}\n"
            "    Matched filesystem errors: {}\n",
            observation.mismatch_count,
            observation.matched_error_count);
        if (observation.failed) {
            devicefs::WriteToStream(output,
                "    Verification failure: {}\n",
                observation.failure);
        }
    }
    output.flush();
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
    auto &output = devicefs::WriteToStream(
        std::cout, "\nFilesystem verification results:\n");
    auto verified = 0uz;
    auto sample_verified = 0uz;
    auto mismatched = 0uz;
    auto indeterminate = 0uz;
    auto incomplete = 0uz;
    auto cleanup_failed = 0uz;
    auto matched_error_volumes = 0uz;
    for (const auto &job : jobs) {
        const auto observation = job.state->Observe();
        const auto &mismatches = job.state->MismatchesAfterJoin();
        const auto &operation_comparisons =
            job.state->OperationComparisonsAfterJoin();
        const auto operation_mismatches = std::ranges::count_if(
            operation_comparisons,
            [](const OperationComparison &comparison) {
                return !IsMatchedError(comparison);
            });
        if (observation.matched_error_count != 0) {
            ++matched_error_volumes;
        }
        const auto identifier =
            winrt::to_hstring(job.volume_identifier);
        devicefs::WriteToStream(output,
            L"\n  Volume ID: {}\n",
            std::wstring_view{identifier});
        if (!mismatches.empty() || (operation_mismatches != 0)) {
            ++mismatched;
            if (observation.failed) {
                devicefs::WriteToStream(output,
                    "    Status: MISMATCH; an operational failure also "
                    "prevented complete comparison\n");
                devicefs::WriteToStream(output,
                    "    Failure: {}\n", observation.failure);
            } else if (observation.comparison_complete) {
                devicefs::WriteToStream(
                    output, "    Status: MISMATCH\n");
            } else {
                devicefs::WriteToStream(output,
                    "    Status: MISMATCH; comparison incomplete\n");
            }
        } else if (observation.failed) {
            ++indeterminate;
            devicefs::WriteToStream(output,
                "    Status: INDETERMINATE\n"
                "    Failure: {}\n",
                observation.failure);
        } else if (!observation.comparison_complete) {
            ++incomplete;
            devicefs::WriteToStream(output,
                "    Status: CANCELLED/INCOMPLETE\n");
        } else if (observation.selected_bytes !=
            observation.available_bytes) {
            ++sample_verified;
            devicefs::WriteToStream(output,
                "    Status: SAMPLE VERIFIED ({:.2f}% requested)\n",
                job.percentage);
        } else {
            ++verified;
            devicefs::WriteToStream(output,
                "    Status: VERIFIED\n");
        }
        devicefs::WriteToStream(output,
            "    Namespace: synthetic {} object(s), {} stream(s); "
            "real B {} object(s), {} stream(s)\n"
            "    Content available: {} bytes\n"
            "    Content selected: {} bytes ({:.2f}%)\n"
            "    Content compared: {} bytes ({:.2f}% of selection)\n"
            "    Mismatches: {}\n"
            "    Matched filesystem errors: {}\n",
            observation.synthetic_objects,
            observation.synthetic_streams,
            observation.real_objects,
            observation.real_streams,
            observation.available_bytes,
            observation.selected_bytes,
            Percentage(observation.selected_bytes,
                observation.available_bytes),
            observation.compared_bytes,
            Percentage(observation.compared_bytes,
                observation.selected_bytes),
            observation.mismatch_count,
            observation.matched_error_count);
        if (!observation.cleanup_failures.empty()) {
            ++cleanup_failed;
            for (const auto &failure : observation.cleanup_failures) {
                devicefs::WriteToStream(output,
                    "    Cleanup failure: {}\n", failure);
            }
        }
        for (const auto &mismatch : mismatches) {
            PrintMismatch(output, job.volume_identifier, mismatch);
        }
        for (const auto &comparison : operation_comparisons) {
            if (!IsMatchedError(comparison)) {
                PrintOperationComparison(
                    output, job.volume_identifier, comparison);
            }
        }
        for (const auto &comparison : operation_comparisons) {
            if (IsMatchedError(comparison)) {
                PrintOperationComparison(
                    output, job.volume_identifier, comparison);
            }
        }
    }
    devicefs::WriteToStream(output,
        "\nSummary: {} verified, {} sample verified, {} mismatched, "
        "{} indeterminate, {} cancelled/incomplete, "
        "{} volume(s) with matched filesystem errors, "
        "{} volume(s) optimization unavailable, "
        "{} view-preparation failure(s), and "
        "{} cleanup-failure volume(s).\n",
        verified, sample_verified, mismatched,
        indeterminate, incomplete, matched_error_volumes,
        optimization_unavailable,
        preparation_failures, cleanup_failed);
    output.flush();

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

[[nodiscard]] auto MountVhdx(
    const HANDLE cancellation_event,
    const std::wstring_view device) -> int {
    const auto mount_target = std::format(
        LR"(\\devicefs\mount-vhdx-{})", internal::UniqueName());
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
                .mount_target = mount_target,
                .vhdx = true,
            })};
    if (!internal::WaitForDeviceFs(
            child.Process(), cancellation_event)) {
        return 0;
    }

    const auto wait = [&] {
        const auto processes = std::array{&child.Process()};
        const auto result = internal::WaitForDeviceFsExitOrCancellation(
            processes, cancellation_event);
        if (result) {
            auto &output = devicefs::WriteToStream(std::cout,
                "devicefs exited with code {}.\n",
                result->exit_code);
            output.flush();
        }
        return result;
    };
    const auto preserve_for_manual_mount =
        [&](const std::string_view error) {
            auto &output = devicefs::WriteToStream(
                std::cout,
                "\nAutomatic VHDX mounting failed: {}\n",
                error);
            devicefs::WriteToStream(output,
                L"The VHDX remains available for manual mounting:\n"
                L"  VHDX file: {}\n"
                L"After unmounting any manual attachment, press Ctrl+C "
                L"to stop devicefs.\n",
                child.Process().readiness_path.native());
            output.flush();
            static_cast<void>(wait());
            child.Stop();
            return 1;
        };

    auto setup_complete = false;
    try {
        constexpr auto privilege_names =
            std::array{wil::zwstring_view{SE_MANAGE_VOLUME_NAME}};
        auto privileges = internal::ProcessPrivilegeEnabler{
            GetCurrentProcess(), privilege_names,
            std::string_view{"the volume-management privilege"}};
        const auto exit = [&] {
            auto mount_identifier = GUID{};
            const auto identifier_status = CoCreateGuid(&mount_identifier);
            if (FAILED(identifier_status)) {
                WinError("could not create a VHDX mount-point identifier",
                    ExplicitWin32Error::FromHresult(identifier_status));
            }
            const auto mount_identifier_text =
                winrt::to_hstring(mount_identifier);
            const auto windows_directory = [] {
                auto result = std::wstring{};
                const auto status = wil::GetWindowsDirectoryW(result);
                if (FAILED(status)) {
                    WinError("could not obtain the Windows directory",
                        ExplicitWin32Error::FromHresult(status));
                }
                return result;
            }();
            const auto mount_directory =
                std::filesystem::path{windows_directory} /
                L"SystemTemp" /
                std::format(L"devicefs-vhdx-{}",
                    std::wstring_view{mount_identifier_text});
            std::filesystem::create_directory(mount_directory);
            auto remove_mount_directory = wil::scope_exit([&] {
                auto ignored = std::error_code{};
                static_cast<void>(
                    std::filesystem::remove(mount_directory, ignored));
            });
            const auto volume_mount_point =
                std::format(L"{}\\", mount_directory.native());
            const auto volume_mount_point_name =
                wil::zwstring_view{volume_mount_point};
            auto view = AttachedVhdx::Attach(
                child.Process().readiness_path, "requested device");
            const auto volume_name = wil::zwstring_view{
                view.Root().data(), view.Root().size()};
            if (!SetVolumeMountPointW(
                    volume_mount_point_name.c_str(), volume_name.c_str())) {
                WinError("could not assign the VHDX volume mount point {}",
                    mount_directory.string());
            }
            setup_complete = true;
            auto &output = devicefs::WriteToStream(
                std::cout,
                L"\nMounted VHDX view:\n"
                L"  Source device: {}\n"
                L"  VHDX file: {}\n"
                L"  Volume: {}\n"
                L"  Mount point: {}\n"
                L"Press Ctrl+C to unmount.\n",
                device, child.Process().readiness_path.native(),
                view.Root(), mount_directory.native());
            output.flush();

            const auto result = wait();
            devicefs::WriteToStream(output, "Unmounting the VHDX.\n");
            output.flush();

            if (!view.IsLoaded()) {
                devicefs::WriteToStream(
                    output, "The VHDX was already unmounted.\n");
            } else {
                const auto detach_status = view.Detach();
                if (detach_status != ERROR_SUCCESS) {
                    WinError("could not unmount VHDX {}",
                        child.Process().readiness_path.string(),
                        ExplicitWin32Error{detach_status});
                }
                devicefs::WriteToStream(
                    output, "The VHDX was unmounted.\n");
            }
            output.flush();
            return result;
        }();
        privileges.Restore();
        child.Stop();
        return exit && (exit->exit_code != 0) ? 1 : 0;
    } catch (const VerificationFailure &error) {
        if (setup_complete) {
            throw;
        }
        return preserve_for_manual_mount(error.what());
    } catch (const std::system_error &error) {
        if (setup_complete) {
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
                        cancellation_event, percentage, borrowed_state] {
                        try {
                            VerifyVolume(volume,
                                synthetic_vhdx, real_vhdx,
                                cancellation_event, percentage,
                                *borrowed_state);
                        } catch (const VerificationFailure &error) {
                            borrowed_state->Fail(error.what());
                            borrowed_state->SetPhase(
                                VerificationPhase::Complete);
                        } catch (const std::system_error &error) {
                            borrowed_state->Fail(error.what());
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
    while (!JobsReady(jobs)) {
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
