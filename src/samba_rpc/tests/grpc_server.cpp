// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <utility>

#include <grpcpp/server_builder.h>

#include "block-device.grpc.pb.h"

namespace {

using namespace std::chrono_literals;

[[nodiscard]] auto ReadFile(const std::filesystem::path &path) -> std::string {
    const auto length = std::filesystem::file_size(path);
    if (!std::in_range<std::size_t>(length) ||
        !std::in_range<std::streamsize>(length)) {
        throw std::length_error{
            std::format("backing image {} is too large", path.string())};
    }

    auto stream = std::ifstream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error{
            std::format("could not open backing image {}", path.string())};
    }

    auto data = std::string(static_cast<std::size_t>(length), '\0');
    if (!stream.read(
            data.data(), static_cast<std::streamsize>(data.size()))) {
        throw std::runtime_error{
            std::format("could not read backing image {}", path.string())};
    }
    return data;
}

class BlockDeviceService final
    : public proxmox::backup::BlockDevice::Service {
public:
    explicit BlockDeviceService(const std::filesystem::path &path)
        : data_{ReadFile(path)} {}

    auto GetLength(grpc::ServerContext *,
        const proxmox::backup::GetLengthRequest *,
        proxmox::backup::GetLengthResponse *const response)
        -> grpc::Status override {
        response->set_length(std::uint64_t{data_.size()});
        return grpc::Status::OK;
    }

    auto Read(grpc::ServerContext *,
        grpc::ServerReaderWriter<
            proxmox::backup::ReadResponse,
            proxmox::backup::ReadRequest> *const stream)
        -> grpc::Status override {
        auto request = proxmox::backup::ReadRequest{};
        while (stream->Read(&request)) {
            auto response = proxmox::backup::ReadResponse{};
            const auto status = Read(request, response);
            if (!status.ok()) {
                return status;
            }
            if (!stream->Write(response)) {
                return grpc::Status::OK;
            }
        }
        return grpc::Status::OK;
    }

private:
    auto Read(const proxmox::backup::ReadRequest &request,
        proxmox::backup::ReadResponse &response) -> grpc::Status {
        {
            auto lock = std::unique_lock{first_reads_mutex_};
            if (first_read_count_ < 2) {
                ++first_read_count_;
                first_reads_ready_.notify_all();
                if (!first_reads_ready_.wait_for(lock, 1s,
                        [this] { return first_read_count_ == 2; })) {
                    return {grpc::StatusCode::DEADLINE_EXCEEDED,
                        "the first two reads did not arrive concurrently"};
                }
            }
        }

        if (request.offset() >= std::uint64_t{data_.size()}) {
            return grpc::Status::OK;
        }

        const auto offset = static_cast<std::size_t>(request.offset());
        const auto count = std::min<std::size_t>(
            request.count(), data_.size() - offset);
        response.set_data(data_.data() + offset, count);
        return grpc::Status::OK;
    }

    const std::string data_;
    std::mutex first_reads_mutex_;
    std::condition_variable first_reads_ready_;
    std::size_t first_read_count_ = 0;
};

} // namespace

auto main(const int argc, char *const argv[]) -> int {
    try {
        if (argc != 3) {
            std::println("usage: {} SOCKET BACKING-IMAGE", argv[0]);
            return 2;
        }

        auto service = BlockDeviceService{argv[2]};
        auto builder = grpc::ServerBuilder{};
        builder.AddListeningPort(std::format("unix:{}", argv[1]),
            grpc::InsecureServerCredentials());
        builder.RegisterService("localhost", &service);
        auto server = builder.BuildAndStart();
        if (!server) {
            throw std::runtime_error{"could not start the gRPC server"};
        }
        std::cout << argv[1] << std::endl;
        server->Wait();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "rpcd_devicefs gRPC fixture failed: " << error.what()
                  << std::endl;
        return 1;
    }
}
