#include "trust_store.hpp"

#include <openssl/x509.h>

#ifdef _WIN32
// wincrypt.h defines X509_NAME (and four others) as *macros*, while OpenSSL
// declares them as types. OpenSSL's headers come first here - it pulls in
// windows.h itself, through e_ostime.h and winsock2.h - so wincrypt.h's macros
// land on top of types that already exist and shadow them for the rest of the
// translation unit. Undefining them immediately below is what keeps the OpenSSL
// spellings usable; nothing after this point wants CryptoAPI's.
//
// WIN32_LEAN_AND_MEAN so windows.h does not drag winsock1 in behind
// Boost.Asio's winsock2. Its companion NOMINMAX cannot be defined here and is
// a global compile definition instead: the chain above reaches windows.h from
// line 1, so min/max are already macros before this point - and they outlive
// this file in a unity batch, breaking the next file's std::min (C2589).
#define WIN32_LEAN_AND_MEAN
#include <wincrypt.h>
#include <windows.h>

#undef X509_NAME
#undef X509_EXTENSIONS
#undef PKCS7_ISSUER_AND_SERIAL
#undef OCSP_RESPONSE
#undef OCSP_REQUEST
#endif

namespace exchange::transport::detail {

#ifdef _WIN32

namespace {

/// @brief Copy every parseable certificate in the named Windows system store
///        into @p target.
/// @return How many were added.
std::size_t add_system_store(X509_STORE *target, const wchar_t *store_name) {
	// No provider, current-user store: the same view the OS gives a browser, so
	// a corporate root installed for the user is honoured, and CryptoAPI layers
	// the machine store underneath. The 0 is not a null pointer - the parameter
	// is HCRYPTPROV_LEGACY, which is an integer handle, and nullptr does not
	// convert to it.
	HCERTSTORE store = ::CertOpenSystemStoreW(0, store_name);
	if (store == nullptr) return 0;

	std::size_t added          = 0;
	PCCERT_CONTEXT certificate = nullptr;
	while ((certificate = ::CertEnumCertificatesInStore(store, certificate)) !=
		   nullptr) {
		// d2i_X509 advances the pointer it is given, so it gets a copy - the
		// original belongs to the certificate context and CertFreeCertificate
		// (which CertEnumCertificatesInStore does for us on the next call) has
		// to see it unchanged.
		const unsigned char *der = certificate->pbCertEncoded;
		X509 *parsed = ::d2i_X509(nullptr, &der, certificate->cbCertEncoded);
		if (parsed == nullptr) continue; // not a DER certificate we can read

		// A duplicate is the common failure and is not one: ROOT and CA
		// overlap, and X509_STORE_add_cert reports the collision rather than
		// replacing. Only a genuine addition is counted.
		if (::X509_STORE_add_cert(target, parsed) == 1) ++added;
		::X509_free(parsed);
	}

	// The final CertEnumCertificatesInStore already freed the last context, so
	// only the store handle is left to release.
	::CertCloseStore(store, 0);
	return added;
}

} // namespace

trust_store_result load_platform_trust_store(SSL_CTX *ctx) {
	X509_STORE *target = ::SSL_CTX_get_cert_store(ctx);
	if (target == nullptr)
		return {.certificates = 0, .source = "context has no trust store"};

	// ROOT holds the trusted roots a chain must terminate at; CA holds the
	// intermediates that get there. A venue that sends a complete chain needs
	// only the first, and one that omits an intermediate needs the second, so
	// both are read.
	const std::size_t roots         = add_system_store(target, L"ROOT");
	const std::size_t intermediates = add_system_store(target, L"CA");
	const std::size_t total         = roots + intermediates;

	if (total == 0)
		return {.certificates = 0,
				.source = "Windows ROOT/CA stores were empty or unreadable"};
	return {.certificates = total, .source = "Windows ROOT/CA stores"};
}

#else

trust_store_result load_platform_trust_store(SSL_CTX *ctx) {
	// The platform installs a CA bundle and OpenSSL was built knowing where.
	// There is no count to report: the paths are registered with the store and
	// read lazily at verification time, so "how many" is not a question this
	// can answer before a handshake. One is reported to mean "configured",
	// which is the only thing the caller checks.
	if (::SSL_CTX_set_default_verify_paths(ctx) != 1)
		return {.certificates = 0,
				.source       = "OpenSSL default verify paths were rejected"};
	return {.certificates = 1, .source = "OpenSSL default paths"};
}

#endif

} // namespace exchange::transport::detail
