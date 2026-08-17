// digest_stub.cpp — Shared crypto for both backends.
//
// For xahaud backend: implements ripple::openssl_*_hasher using sha_impl.h
// For xdata backend: just provides sha256_oneshot (used by base58)
//
// The #if guards ensure only the relevant code compiles per backend.

#include "sha_impl.h"

// One-shot SHA-256 used by catl::base58 (both backends)
extern "C" void sha256_oneshot(const uint8_t* in, size_t in_len, uint8_t* out) {
    sha_impl::Sha256State s;
    sha_impl::sha256_init(s);
    sha_impl::sha256_update(s, in, in_len);
    sha_impl::sha256_final(s, out);
}

#ifdef CODEC_BACKEND_XAHAUD
// =================================================================
// xahaud backend: implement ripple::openssl_*_hasher interfaces
// =================================================================

#include <xrpl/protocol/digest.h>

using namespace sha_impl;

namespace ripple {

// --- SHA-256 ---

openssl_sha256_hasher::openssl_sha256_hasher()
{
    static_assert(sizeof(ctx_) >= sizeof(Sha256State));
    sha256_init(*reinterpret_cast<Sha256State*>(ctx_));
}

void
openssl_sha256_hasher::operator()(void const* data, std::size_t size) noexcept
{
    sha256_update(*reinterpret_cast<Sha256State*>(ctx_), data, size);
}

openssl_sha256_hasher::operator result_type() noexcept
{
    result_type out{};
    sha256_final(*reinterpret_cast<Sha256State*>(ctx_), out.data());
    return out;
}

// --- SHA-512 ---

openssl_sha512_hasher::openssl_sha512_hasher()
{
    static_assert(sizeof(ctx_) >= sizeof(Sha512State));
    sha512_init(*reinterpret_cast<Sha512State*>(ctx_));
}

void
openssl_sha512_hasher::operator()(void const* data, std::size_t size) noexcept
{
    sha512_update(*reinterpret_cast<Sha512State*>(ctx_), data, size);
}

openssl_sha512_hasher::operator result_type() noexcept
{
    result_type out{};
    sha512_final(*reinterpret_cast<Sha512State*>(ctx_), out.data());
    return out;
}

// --- RIPEMD-160 (stub — returns zeros) ---

openssl_ripemd160_hasher::openssl_ripemd160_hasher()
{
    std::memset(ctx_, 0, sizeof(ctx_));
}

void
openssl_ripemd160_hasher::operator()(void const*, std::size_t) noexcept
{
}

openssl_ripemd160_hasher::operator result_type() noexcept
{
    return result_type{};
}

}  // namespace ripple

#endif  // CODEC_BACKEND_XAHAUD
