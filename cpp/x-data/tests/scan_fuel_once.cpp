#include "scan_fuel_once.h"

#include "catl/xdata/amount-rules.h"
#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"

#include <cstdint>

volatile uint64_t g_escape = 0;

#if defined(CATL_XDATA_HELPER_CALL_COUNTS)
uint32_t g_helper_counts[5]{};
#define CATL_COUNT_HELPER(i) ++g_helper_counts[i]
#else
#define CATL_COUNT_HELPER(i) ((void)0)
#endif

extern "C" {

uint64_t
view_once_c(void const* idx, size_t ord, int32_t* exp, uint8_t* tag)
{
    CATL_COUNT_HELPER(0);
    auto const* certified =
        static_cast<catl::xdata::CertifiedIndex const*>(idx);
    auto v = catl::xdata::AmountView::bind(*certified, ord);
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
    CATL_COUNT_HELPER(1);
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
mask_once_c(uint8_t const* bytes, size_t n, int32_t* exp, uint8_t* tag)
{
    CATL_COUNT_HELPER(2);
    if (n < 28)
    {
        *exp = 0;
        *tag = 0;
        g_escape = 0;
        return 0;
    }
    uint64_t const word = catl::xdata::AmountRules::read_be64(bytes);
    uint64_t const mant = word & ~(1023ull << 54);
    int const offset = static_cast<int>(word >> 54);
    *exp = mant ? (offset & 255) - 97 : 0;
    *tag = bytes[27];
    g_escape = mant;
    return mant;
}

uint64_t
parts_once_c(void const* view, int32_t* exp, uint8_t* tag)
{
    CATL_COUNT_HELPER(3);
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
    CATL_COUNT_HELPER(4);
    auto const* p = static_cast<catl::xdata::AmountRules::Parts const*>(parts);
    *exp = p->exponent;
    *tag = (p->currency.size() >= 20) ? p->currency.data()[19] : 0;
    g_escape = p->magnitude;
    return p->magnitude;
}

#if defined(CATL_XDATA_HELPER_CALL_COUNTS)
void
helper_counts_reset_c()
{
    for (auto& n : g_helper_counts)
        n = 0;
}

void
helper_counts_read_c(uint32_t* out)
{
    for (size_t i = 0; i < 5; ++i)
        out[i] = g_helper_counts[i];
}
#endif
}
