#include "tls_utils.hpp"

#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <wincrypt.h>
#include <windows.h>

#endif

namespace co_wq::net::tls_utils {

std::string collect_error_stack()
{
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio)
        return "unknown";
    ERR_print_errors(bio);
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio, &mem);
    std::string out = (mem && mem->data) ? std::string(mem->data, mem->length) : std::string {};
    BIO_free(bio);
    return out;
}

std::string asn1_time_to_string(const ASN1_TIME* time)
{
    if (!time)
        return {};
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio)
        return {};
    std::string result;
    if (ASN1_TIME_print(bio, time) == 1) {
        BUF_MEM* mem = nullptr;
        BIO_get_mem_ptr(bio, &mem);
        if (mem && mem->data)
            result.assign(mem->data, mem->length);
    }
    BIO_free(bio);
    return result;
}

std::string x509_name_to_string(const X509_NAME* name)
{
    if (!name)
        return {};
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio)
        return {};
    std::string result;
    if (X509_NAME_print_ex(bio, const_cast<X509_NAME*>(name), 0, XN_FLAG_RFC2253) >= 0) {
        BUF_MEM* mem = nullptr;
        BIO_get_mem_ptr(bio, &mem);
        if (mem && mem->data)
            result.assign(mem->data, mem->length);
    }
    BIO_free(bio);
    return result;
}

std::string collect_subject_alt_names(X509* cert)
{
    if (!cert)
        return {};
    STACK_OF(GENERAL_NAME)* names = static_cast<STACK_OF(GENERAL_NAME)*>(
        X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (!names)
        return {};
    std::string result;
    const int   count = sk_GENERAL_NAME_num(names);
    for (int i = 0; i < count; ++i) {
        const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
        if (!name)
            continue;
        if (name->type == GEN_DNS && name->d.dNSName) {
            unsigned char* utf8 = nullptr;
            int            len  = ASN1_STRING_to_UTF8(&utf8, name->d.dNSName);
            if (len <= 0 || !utf8)
                continue;
            if (!result.empty())
                result.append(", ");
            result.append("DNS:");
            result.append(reinterpret_cast<const char*>(utf8), static_cast<size_t>(len));
            OPENSSL_free(utf8);
        }
    }
    GENERAL_NAMES_free(names);
    return result;
}

std::string describe_public_key(EVP_PKEY* key)
{
    if (!key)
        return "unknown";
    const int   bits = EVP_PKEY_bits(key);
    std::string type;
    switch (EVP_PKEY_base_id(key)) {
    case EVP_PKEY_RSA:
        type = "RSA";
        break;
    case EVP_PKEY_EC:
        type = "EC";
        break;
    case EVP_PKEY_DSA:
        type = "DSA";
        break;
    default:
        type = "UNKNOWN";
        break;
    }
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s (%d bits)", type.c_str(), bits);
    return buffer;
}

std::string describe_signature_algorithm(const X509* cert)
{
    if (!cert)
        return "unknown";
    int nid = X509_get_signature_nid(cert);
    if (nid == NID_undef)
        return "unknown";
    const char* name = OBJ_nid2sn(nid);
    return name ? name : "unknown";
}

std::vector<std::string> summarize_session(SSL* ssl, std::string_view host, bool include_chain)
{
    std::vector<std::string> lines;
    if (!ssl)
        return lines;

    const char* protocol = SSL_get_version(ssl);
    const char* cipher   = SSL_get_cipher_name(ssl);
    lines.emplace_back(std::string("SSL connection using ") + (protocol ? protocol : "unknown") + " / "
                       + (cipher ? cipher : "unknown"));

    const unsigned char* alpn     = nullptr;
    unsigned int         alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn && alpn_len > 0) {
        lines.emplace_back(std::string("ALPN negotiated protocol: ")
                           + std::string(reinterpret_cast<const char*>(alpn), alpn_len));
    } else {
        lines.emplace_back("ALPN negotiated protocol: (none)");
    }

    X509* leaf = SSL_get1_peer_certificate(ssl);
    if (leaf) {
        std::string subject    = x509_name_to_string(X509_get_subject_name(leaf));
        std::string issuer     = x509_name_to_string(X509_get_issuer_name(leaf));
        std::string not_before = asn1_time_to_string(X509_get0_notBefore(leaf));
        std::string not_after  = asn1_time_to_string(X509_get0_notAfter(leaf));

        lines.emplace_back(std::string("subject: ") + (subject.empty() ? "<unknown>" : subject));
        lines.emplace_back(std::string("issuer: ") + (issuer.empty() ? "<unknown>" : issuer));
        lines.emplace_back(std::string("start date: ") + (not_before.empty() ? "<unknown>" : not_before));
        lines.emplace_back(std::string("expire date: ") + (not_after.empty() ? "<unknown>" : not_after));

        if (!host.empty()) {
            const int match = X509_check_host(leaf, host.data(), host.size(), 0, nullptr);
            if (match == 1)
                lines.emplace_back(std::string("subjectAltName: host \"") + std::string(host)
                                   + "\" matched certificate");
            else if (match == 0)
                lines.emplace_back(std::string("subjectAltName: host \"") + std::string(host)
                                   + "\" did not match certificate");
        }

        if (auto san = collect_subject_alt_names(leaf); !san.empty())
            lines.emplace_back(std::string("subjectAltName: ") + san);

        if (EVP_PKEY* key = X509_get_pubkey(leaf)) {
            auto key_desc = describe_public_key(key);
            EVP_PKEY_free(key);
            lines.emplace_back(std::string("Public key: ") + key_desc);
        }

        auto sig_desc = describe_signature_algorithm(leaf);
        lines.emplace_back(std::string("Signature algorithm: ") + sig_desc);
        X509_free(leaf);
    } else {
        lines.emplace_back("Server certificate: <none>");
    }

    if (include_chain) {
        if (STACK_OF(X509)* chain = SSL_get_peer_cert_chain(ssl)) {
            const int count = sk_X509_num(chain);
            for (int i = 0; i < count; ++i) {
                X509* chain_cert = sk_X509_value(chain, i);
                if (!chain_cert)
                    continue;
                if (EVP_PKEY* key = X509_get_pubkey(chain_cert)) {
                    auto key_desc = describe_public_key(key);
                    EVP_PKEY_free(key);
                    auto sig_desc = describe_signature_algorithm(chain_cert);
                    lines.emplace_back(std::string("Certificate level ") + std::to_string(i) + ": " + key_desc
                                       + ", signed using " + sig_desc);
                }
            }
        }
    }

    const long verify_result = SSL_get_verify_result(ssl);
    if (verify_result == X509_V_OK)
        lines.emplace_back("SSL certificate verify ok.");
    else
        lines.emplace_back(std::string("SSL certificate verify result: ") + X509_verify_cert_error_string(verify_result)
                           + " (" + std::to_string(verify_result) + ")");

    return lines;
}

#if defined(_WIN32)

bool load_system_root_certificates(SSL_CTX* ctx)
{
    if (!ctx)
        return false;

    X509_STORE* x509_store = SSL_CTX_get_cert_store(ctx);
    if (!x509_store)
        return false;

    const DWORD store_flags = CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG;
    const struct {
        DWORD          location;
        const wchar_t* name;
    } stores[] = {
        { CERT_SYSTEM_STORE_CURRENT_USER,  L"ROOT" },
        { CERT_SYSTEM_STORE_LOCAL_MACHINE, L"ROOT" },
    };

    bool loaded_any = false;
    for (const auto& entry : stores) {
        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W,
                                         0,
                                         static_cast<HCRYPTPROV_LEGACY>(0),
                                         entry.location | store_flags,
                                         entry.name);
        if (!store)
            continue;

        PCCERT_CONTEXT cert_context = nullptr;
        while (true) {
            PCCERT_CONTEXT next = CertEnumCertificatesInStore(store, cert_context);
            if (!next)
                break;

            const unsigned char* encoded = next->pbCertEncoded;
            X509*                cert    = d2i_X509(nullptr, &encoded, next->cbCertEncoded);
            if (cert) {
                if (X509_STORE_add_cert(x509_store, cert) == 1) {
                    loaded_any = true;
                } else {
                    unsigned long err = ERR_peek_last_error();
                    if (ERR_GET_LIB(err) == ERR_LIB_X509 && ERR_GET_REASON(err) == X509_R_CERT_ALREADY_IN_HASH_TABLE)
                        ERR_clear_error();
                }
                X509_free(cert);
            }

            cert_context = next;
        }

        if (cert_context)
            CertFreeCertificateContext(cert_context);
        CertCloseStore(store, 0);
    }

    return loaded_any;
}

#endif // defined(_WIN32)

} // namespace co_wq::net::tls_utils
