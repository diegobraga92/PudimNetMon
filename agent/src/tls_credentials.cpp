#include <fstream>
#include <iterator>

#include "tls_credentials.h"

namespace pudimagent {

namespace {

// Reads an entire file into a string.
std::string ReadFile(const std::string &path) {
    if (path.empty()) return "";
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

} // anonymous namespace

std::shared_ptr<grpc::ChannelCredentials> MakeChannelCredentials(
    const std::string &ca_path, const std::string &cert_path,
    const std::string &key_path) {
    if (ca_path.empty() && cert_path.empty() && key_path.empty()) {
        return grpc::InsecureChannelCredentials();
    }
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = ReadFile(ca_path);
    opts.pem_private_key = ReadFile(key_path);
    opts.pem_cert_chain = ReadFile(cert_path);
    if (opts.pem_root_certs.empty()) {
        return grpc::InsecureChannelCredentials();
    }
    return grpc::SslCredentials(opts);
}

std::shared_ptr<grpc::ServerCredentials> MakeServerCredentials(
    const std::string &ca_path, const std::string &cert_path,
    const std::string &key_path) {
    if (ca_path.empty() && cert_path.empty() && key_path.empty()) {
        return grpc::InsecureServerCredentials();
    }
    grpc::SslServerCredentialsOptions opts;
    opts.pem_root_certs = ReadFile(ca_path);  // used to verify client certs
    opts.pem_key_cert_pairs.push_back({ReadFile(key_path),
                                       ReadFile(cert_path)});
    // Require + verify the client certificate.
    opts.client_certificate_request =
        GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
    if (opts.pem_root_certs.empty()) {
        return grpc::InsecureServerCredentials();
    }
    return grpc::SslServerCredentials(opts);
}

} // namespace pudimagent
