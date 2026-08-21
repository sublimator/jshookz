#include "account_id_fuel_once.h"

#include "catl/xdata/account-id-view.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/protocol.h"

namespace {

volatile uint32_t g_account_id_escape = 0;

#if defined(CATL_XDATA_ACCOUNT_ID_HELPER_CALL_COUNTS)
uint32_t g_account_id_helper_counts[4]{};
#define CATL_COUNT_ACCOUNT_ID_HELPER(i) ++g_account_id_helper_counts[i]
#else
#define CATL_COUNT_ACCOUNT_ID_HELPER(i) ((void)0)
#endif

uint32_t
checksum(uint8_t const* bytes)
{
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < 20; ++i)
        value = (value ^ bytes[i]) * 16777619u;
    return value;
}

}  // namespace

extern "C" {

uint32_t
account_id_retained_once_c(uint8_t const* normalized)
{
    CATL_COUNT_ACCOUNT_ID_HELPER(0);
    uint32_t const value = checksum(normalized);
    g_account_id_escape = value;
    return value;
}

uint32_t
account_id_prebound_once_c(void const* view)
{
    CATL_COUNT_ACCOUNT_ID_HELPER(1);
    auto const* typed = static_cast<catl::xdata::AccountIDView const*>(view);
    auto const normalized = typed->normalized();
    uint32_t const value = checksum(normalized.data());
    g_account_id_escape = value;
    return value;
}

uint32_t
account_id_rebind_once_c(void const* idx, size_t ordinal)
{
    CATL_COUNT_ACCOUNT_ID_HELPER(2);
    auto const* certified =
        static_cast<catl::xdata::CertifiedIndex const*>(idx);
    auto view = catl::xdata::AccountIDView::bind(*certified, ordinal);
    if (!view)
    {
        g_account_id_escape = 0;
        return 0;
    }
    auto const normalized = view->normalized();
    uint32_t const value = checksum(normalized.data());
    g_account_id_escape = value;
    return value;
}

uint32_t
account_id_raw_once_c(
    uint8_t const* bytes, size_t size, void const* protocol)
{
    CATL_COUNT_ACCOUNT_ID_HELPER(3);
    auto const* p = static_cast<catl::xdata::Protocol const*>(protocol);
    auto idx = catl::xdata::certify_indexed(
        Slice{bytes, size}, 0, *p);
    if (!idx)
    {
        g_account_id_escape = 0;
        return 0;
    }
    auto view = catl::xdata::AccountIDView::bind(*idx, 0);
    if (!view)
    {
        g_account_id_escape = 0;
        return 0;
    }
    auto const normalized = view->normalized();
    uint32_t const value = checksum(normalized.data());
    g_account_id_escape = value;
    return value;
}

#if defined(CATL_XDATA_ACCOUNT_ID_HELPER_CALL_COUNTS)
void
account_id_helper_counts_reset_c()
{
    for (auto& n : g_account_id_helper_counts)
        n = 0;
}

void
account_id_helper_counts_read_c(uint32_t* out)
{
    for (size_t i = 0; i < 4; ++i)
        out[i] = g_account_id_helper_counts[i];
}
#endif
}
