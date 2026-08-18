#pragma once

#include "hash/hash.hpp"

namespace hook {

class AccountID : public Hash160
{
public:
    using Hash160::Hash160;

    bool operator==(AccountID const& other) const
    {
        return Hash160::operator==(other);
    }
};

}  // namespace hook
