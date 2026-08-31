// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <grpcpp/server_builder.h>

#include "block-device.grpc.pb.h"

import std;

namespace {

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
        const proxmox::backup::ReadRequest *const request,
        proxmox::backup::ReadResponse *const response)
        -> grpc::Status override {
        if (request->offset() >= std::uint64_t{data_.size()}) {
            return grpc::Status::OK;
        }

        const auto offset = static_cast<std::size_t>(request->offset());
        const auto count = std::min<std::size_t>(
            request->count(), data_.size() - offset);
        response->set_data(data_.data() + offset, count);
        return grpc::Status::OK;
    }

private:
    const std::string data_;
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
        builder.RegisterService(&service);
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
