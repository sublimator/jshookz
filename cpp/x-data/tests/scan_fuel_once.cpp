#include "scan_fuel_once.h"

#include "catl/xdata/amount-rules.h"
#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"

#include <cstdint>

volatile uint64_t g_escape = 0;

extern "C" {

uint64_t
view_once_c(void const* idx, size_t ord, int32_t* exp, uint8_t* tag)
{
    auto const* cidx = static_cast<catl::xdata::CertifiedIndex const*>(idx);
    auto v = catl::xdata::AmountView::bind(*cidx, ord);
    if (!v)
    {
        *exp = 0;
        *tag = 0;
        g_escape = 0;
        return 0;
    }
    auto p = v->parts();
    *exp = p.exponent;
    *tag = (p.currency.size() >= 20) ? p.currency.data()[19] : 0;
    g_escape = p.magnitude;
    return p.magnitude;
}

uint64_t
raw_once_c(uint8_t const* bytes, size_t n, int32_t* exp, uint8_t* tag)
{
    Slice payload{bytes, n};
    if (catl::xdata::AmountRules::certify(payload))
    {
        *exp = 0;
        *tag = 0;
        g_escape = 0;
        return 0;
    }
    auto p = catl::xdata::AmountRules::parts(payload);
    *exp = p.exponent;
    *tag = (p.currency.size() >= 20) ? p.currency.data()[19] : 0;
    g_escape = p.magnitude;
    return p.magnitude;
}

uint64_t
mask_once_c(uint8_t const* bytes, size_t n, uint8_t* tag)
{
    if (n < 28)
    {
        *tag = 0;
        g_escape = 0;
        return 0;
    }
    uint64_t const word = catl::xdata::AmountRules::read_be64(bytes);
    uint64_t const mant = word & ~(1023ull << 54);
    *tag = bytes[27];
    g_escape = mant;
    return mant;
}

uint64_t
parts_once_c(void const* view, int32_t* exp, uint8_t* tag)
{
    auto const* v = static_cast<catl::xdata::AmountView const*>(view);
    auto p = v->parts();
    *exp = p.exponent;
    *tag = (p.currency.size() >= 20) ? p.currency.data()[19] : 0;
    g_escape = p.magnitude;
    return p.magnitude;
}

uint64_t
retained_parts_c(void const* parts, int32_t* exp, uint8_t* tag)
{
    auto const* p = static_cast<catl::xdata::AmountRules::Parts const*>(parts);
    *exp = p->exponent;
    *tag = (p->currency.size() >= 20) ? p->currency.data()[19] : 0;
    g_escape = p->magnitude;
    return p->magnitude;
}

}
