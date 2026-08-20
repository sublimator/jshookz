// Deliberate red control: under CATL_XDATA_NO_THROWING_CURSOR this must
// fail to compile because peek_u8 is a throwing facade.
#include "catl/xdata/slice-cursor.h"

void
use_throwing_facade(catl::xdata::SliceCursor& c)
{
    (void)c.peek_u8();
}
