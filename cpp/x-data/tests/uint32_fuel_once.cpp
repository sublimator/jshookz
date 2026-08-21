#include "uint32_fuel_once.h"

#include "catl/xdata/certified-index.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/uint32-view.h"

namespace {

volatile uint32_t g_uint32_escape = 0;

#if defined(CATL_XDATA_UINT32_HELPER_CALL_COUNTS)
uint32_t g_uint32_helper_counts[4]{};
#define CATL_COUNT_UINT32_HELPER(i) ++g_uint32_helper_counts[i]
#else
#define CATL_COUNT_UINT32_HELPER(i) ((void)0)
#endif

}  // namespace

extern "C" {

uint32_t
u32_retained_once_c(uint32_t value)
{
    CATL_COUNT_UINT32_HELPER(0);
    g_uint32_escape = value;
    return value;
}

uint32_t
u32_prebound_once_c(void const* view)
{
    CATL_COUNT_UINT32_HELPER(1);
    auto const* typed = static_cast<catl::xdata::UInt32View const*>(view);
    uint32_t const value = typed->value();
    g_uint32_escape = value;
    return value;
}

uint32_t
u32_rebind_once_c(void const* idx, size_t ordinal)
{
    CATL_COUNT_UINT32_HELPER(2);
    auto const* certified =
        static_cast<catl::xdata::CertifiedIndex const*>(idx);
    auto view = catl::xdata::UInt32View::bind(*certified, ordinal);
    uint32_t const value = view ? view->value() : 0;
    g_uint32_escape = value;
    return value;
}

uint32_t
u32_raw_once_c(uint8_t const* bytes, size_t size, void const* protocol)
{
    CATL_COUNT_UINT32_HELPER(3);
    auto const* p = static_cast<catl::xdata::Protocol const*>(protocol);
    auto idx = catl::xdata::certify_indexed(Slice{bytes, size}, 0, *p);
    if (!idx)
    {
        g_uint32_escape = 0;
        return 0;
    }
    auto view = catl::xdata::UInt32View::bind(*idx, 0);
    uint32_t const value = view ? view->value() : 0;
    g_uint32_escape = value;
    return value;
}

#if defined(CATL_XDATA_UINT32_HELPER_CALL_COUNTS)
void
u32_helper_counts_reset_c()
{
    for (auto& n : g_uint32_helper_counts)
        n = 0;
}

void
u32_helper_counts_read_c(uint32_t* out)
{
    for (size_t i = 0; i < 4; ++i)
        out[i] = g_uint32_helper_counts[i];
}
#endif
}
