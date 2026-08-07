#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

namespace pudimagent {

// Builds client ChannelCredentials from CA/cert/key file paths. Returns
// insecure credentials when all three paths are empty (backward compatible).
std::shared_ptr<grpc::ChannelCredentials> MakeChannelCredentials(
    const std::string &ca_path, const std::string &cert_path,
    const std::string &key_path);

// Builds server ServerCredentials (mutual TLS: requires and verifies the
// client certificate) from the given files. Returns insecure server
// credentials when all three paths are empty.
std::shared_ptr<grpc::ServerCredentials> MakeServerCredentials(
    const std::string &ca_path, const std::string &cert_path,
    const std::string &key_path);

} // namespace pudimagent
