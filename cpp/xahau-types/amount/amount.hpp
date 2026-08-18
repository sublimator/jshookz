#pragma once

#include "account/account.hpp"
#include "xfl/xfl.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace hook {

struct Currency
{
    std::array<uint8_t, 20> code{};

    bool is_xrp() const
    {
        for (auto b : code)
            if (b != 0)
                return false;
        return true;
    }

    static Currency from_ascii(const char* s)
    {
        Currency c;
        size_t len = std::min(strlen(s), size_t(3));
        std::memcpy(c.code.data() + 12, s, len);
        return c;
    }
};

class Amount
{
    int64_t drops_{};
    Currency currency_{};
    AccountID issuer_{};
    bool is_iou_{false};

public:
    Amount() = default;

    static Amount xrp(int64_t drops)
    {
        Amount a;
        a.drops_ = drops;
        return a;
    }

    static Amount iou(XFL value, Currency currency, AccountID issuer)
    {
        Amount a;
        a.drops_ = value.raw();
        a.currency_ = currency;
        a.issuer_ = issuer;
        a.is_iou_ = true;
        return a;
    }

    bool is_xrp() const { return !is_iou_; }
    bool is_iou() const { return is_iou_; }
    int64_t drops() const { return drops_; }
    XFL iou_value() const { return XFL(drops_); }
    const Currency& currency() const { return currency_; }
    const AccountID& issuer() const { return issuer_; }
};

}  // namespace hook
