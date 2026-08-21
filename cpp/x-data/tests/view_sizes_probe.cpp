#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/scan.h"

#include <optional>

int
main()
{
    using catl::xdata::AmountRules;
    using catl::xdata::AmountView;
    using catl::xdata::CertifiedIndex;
    using catl::xdata::CertifiedRoot;
    using catl::xdata::FieldFrame;
#if defined(__wasm32__)
    static_assert(sizeof(AmountView) == 8);
    static_assert(sizeof(std::optional<AmountView>) == 12);
    static_assert(sizeof(FieldFrame) == 16);
    static_assert(sizeof(CertifiedIndex) == 32);
    static_assert(sizeof(AmountRules::Parts) == 48);
#else
    static_assert(sizeof(FieldFrame) == 16);
#endif
    static_assert(sizeof(AmountView) == sizeof(Slice));
    (void)sizeof(CertifiedRoot);
    return 0;
}
