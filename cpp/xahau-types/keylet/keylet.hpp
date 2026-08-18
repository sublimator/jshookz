#pragma once

#include "hash/hash.hpp"

namespace hook {

class Keylet
{
    uint16_t type_{};
    Hash256 key_{};

public:
    Keylet() = default;
    Keylet(uint16_t type, Hash256 key) : type_(type), key_(key) {}

    uint16_t type() const { return type_; }
    const Hash256& key() const { return key_; }

    const uint8_t* data() const
    {
        return reinterpret_cast<const uint8_t*>(this);
    }
    static constexpr size_t size() { return 34; }

    bool operator==(const Keylet& o) const
    {
        return type_ == o.type_ && key_ == o.key_;
    }
};

}  // namespace hook
