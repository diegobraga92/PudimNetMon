#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

namespace pudimagent {

std::shared_ptr<grpc::ChannelCredentials> MakeChannelCredentials(
    const std::string &ca_path, const std::string &cert_path,
    const std::string &key_path);

std::shared_ptr<grpc::ServerCredentials> MakeServerCredentials(
    const std::string &ca_path, const std::string &cert_path,
    const std::string &key_path);

} // namespace pudimagent
