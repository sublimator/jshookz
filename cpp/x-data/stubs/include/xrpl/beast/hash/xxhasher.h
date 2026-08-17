// Stub xxhasher for wasm32 — the real one requires sizeof(size_t)==8
// Uses FNV-1a instead of XXH3. Only used for hash maps, not crypto.

#ifndef BEAST_HASH_XXHASHER_H_INCLUDED
#define BEAST_HASH_XXHASHER_H_INCLUDED

#include <boost/endian/conversion.hpp>
#include <cstddef>
#include <cstdint>

namespace beast {

class xxhasher
{
private:
    std::size_t state_ = 2166136261u;  // FNV offset basis (32-bit)

public:
    static constexpr auto const endian = boost::endian::order::native;

    using result_type = std::size_t;

    xxhasher() = default;

    template <
        class Seed,
        std::enable_if_t<std::is_unsigned<Seed>::value>* = nullptr>
    explicit xxhasher(Seed seed) : state_(seed ^ 2166136261u)
    {
    }

    template <
        class Seed,
        std::enable_if_t<std::is_unsigned<Seed>::value>* = nullptr>
    xxhasher(Seed seed, Seed) : state_(seed ^ 2166136261u)
    {
    }

    void
    operator()(void const* key, std::size_t len) noexcept
    {
        auto const* p = static_cast<unsigned char const*>(key);
        for (std::size_t i = 0; i < len; ++i)
        {
            state_ ^= p[i];
            state_ *= 16777619u;  // FNV prime (32-bit)
        }
    }

    explicit
    operator std::size_t() noexcept
    {
        return state_;
    }
};

}  // namespace beast

#endif
