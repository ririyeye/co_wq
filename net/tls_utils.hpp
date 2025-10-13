#pragma once

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <string>
#include <string_view>
#include <vector>

namespace co_wq::net::tls_utils {

std::string              collect_error_stack();
std::string              asn1_time_to_string(const ASN1_TIME* time);
std::string              x509_name_to_string(const X509_NAME* name);
std::string              collect_subject_alt_names(X509* cert);
std::string              describe_public_key(EVP_PKEY* key);
std::string              describe_signature_algorithm(const X509* cert);
std::vector<std::string> summarize_session(SSL* ssl, std::string_view host, bool include_chain = true);

#if defined(_WIN32)
bool load_system_root_certificates(SSL_CTX* ctx);
#else
inline bool load_system_root_certificates(SSL_CTX*)
{
    return false;
}
#endif

} // namespace co_wq::net::tls_utils
