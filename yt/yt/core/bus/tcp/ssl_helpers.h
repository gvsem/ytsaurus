#pragma once

#include <openssl/ssl.h>

#include <util/generic/string.h>

namespace NYT::NBus {

////////////////////////////////////////////////////////////////////////////////

struct TDeleter
{
    void operator()(SSL_CTX* ctx) const;
    void operator()(SSL* ctx) const;
    void operator()(BIO* bio) const;
    void operator()(RSA* rsa) const;
    void operator()(X509* x509) const;
};

////////////////////////////////////////////////////////////////////////////////

TString GetLastSslErrorString();

////////////////////////////////////////////////////////////////////////////////

bool UseCertificateChain(const TString& certificate, SSL* ssl);

////////////////////////////////////////////////////////////////////////////////

bool UsePrivateKey(const TString& privateKey, SSL* ssl);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NBus
