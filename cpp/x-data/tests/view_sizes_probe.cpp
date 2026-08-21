#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/scan.h"

#include <optional>

int
main()
{
    using catl::xdata::AmountRules;
    using catl::xdata::AmountView;
    using catl::xdata::AnchoredAmount;
    using catl::xdata::CertifiedIndex;
    using catl::xdata::CertifiedObject;
    using catl::xdata::CertifiedRoot;
    using catl::xdata::FieldFrame;
#if defined(__wasm32__)
    static_assert(sizeof(AmountView) == 8);
    static_assert(sizeof(std::optional<AmountView>) == 12);
    static_assert(sizeof(FieldFrame) == 16);
    static_assert(sizeof(CertifiedIndex) == 28);
    static_assert(sizeof(CertifiedRoot) == 40);
    static_assert(sizeof(CertifiedObject) == 40);
    static_assert(sizeof(AnchoredAmount) == 48);
    static_assert(sizeof(AmountRules::Parts) == 48);
#else
    static_assert(sizeof(FieldFrame) == 16);
#if defined(__aarch64__) || defined(__x86_64__)
    static_assert(sizeof(CertifiedIndex) == 48);
    static_assert(sizeof(CertifiedRoot) == 72);
    static_assert(sizeof(CertifiedObject) == 72);
    static_assert(sizeof(AnchoredAmount) == 88);
    static_assert(sizeof(AmountView) == 16);
#else
    static_assert(sizeof(CertifiedIndex) <= 56);
#endif
#endif
    static_assert(sizeof(AmountView) == sizeof(Slice));
    return 0;
}
