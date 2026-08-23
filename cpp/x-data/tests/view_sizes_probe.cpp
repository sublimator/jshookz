#include "catl/xdata/account-id-view.h"
#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/pathset-view.h"
#include "catl/xdata/scan.h"
#include "catl/xdata/uint32-view.h"

#include <optional>

int
main()
{
    using catl::xdata::AccountIDBytes;
    using catl::xdata::AccountIDView;
    using catl::xdata::AmountRules;
    using catl::xdata::AmountView;
    using catl::xdata::CertifiedIndex;
    using catl::xdata::CertifiedObject;
    using catl::xdata::CertifiedRoot;
    using catl::xdata::FieldFrame;
    using catl::xdata::IndexSink;
    using catl::xdata::OwnedAmountParts;
    using catl::xdata::PathSetView;
    using catl::xdata::UInt32View;
#if defined(__wasm32__)
    static_assert(sizeof(AmountView) == 8);
    static_assert(sizeof(std::optional<AmountView>) == 12);
    static_assert(sizeof(FieldFrame) == 16);
    static_assert(sizeof(CertifiedIndex) == 28);
    static_assert(sizeof(CertifiedRoot) == 40);
    static_assert(sizeof(CertifiedObject) == 40);
    static_assert(sizeof(AmountRules::Parts) == 48);
    static_assert(sizeof(OwnedAmountParts) == 64);
    static_assert(sizeof(std::optional<OwnedAmountParts>) == 72);
    static_assert(sizeof(IndexSink) == 144);
    static_assert(sizeof(UInt32View) == 8);
    static_assert(sizeof(std::optional<UInt32View>) == 12);
    static_assert(sizeof(AccountIDView) == 8);
    static_assert(sizeof(std::optional<AccountIDView>) == 12);
    static_assert(sizeof(PathSetView) == 8);
    static_assert(sizeof(std::optional<PathSetView>) == 12);
#else
    static_assert(sizeof(FieldFrame) == 16);
#if defined(__aarch64__) || defined(__x86_64__)
    static_assert(sizeof(CertifiedIndex) == 48);
    static_assert(sizeof(CertifiedRoot) == 72);
    static_assert(sizeof(CertifiedObject) == 72);
    static_assert(sizeof(AmountView) == 16);
    static_assert(sizeof(OwnedAmountParts) == 64);
    static_assert(sizeof(std::optional<OwnedAmountParts>) == 72);
    static_assert(sizeof(IndexSink) == 160);
    static_assert(sizeof(UInt32View) == 16);
    static_assert(sizeof(std::optional<UInt32View>) == 24);
    static_assert(sizeof(AccountIDView) == 16);
    static_assert(sizeof(std::optional<AccountIDView>) == 24);
    static_assert(sizeof(PathSetView) == 16);
    static_assert(sizeof(std::optional<PathSetView>) == 24);
#else
    static_assert(sizeof(CertifiedIndex) <= 56);
#endif
#endif
    static_assert(sizeof(AmountView) == sizeof(Slice));
    static_assert(sizeof(UInt32View) == sizeof(Slice));
    static_assert(sizeof(AccountIDView) == sizeof(Slice));
    static_assert(sizeof(PathSetView) == sizeof(Slice));
    static_assert(sizeof(AccountIDBytes) == 20);
    return 0;
}
