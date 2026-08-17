#define CATL_XDATA_NO_BOOST_JSON
#include "qjs_visitor.h"
#include "qjs_encode.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/parser.h"

#if defined(BOOST_JSON_HPP) || defined(BOOST_JSON_VALUE_HPP) || \
    defined(BOOST_JSON_SRC_HPP)
#error boost json header leaked into a CATL_XDATA_NO_BOOST_JSON translation unit
#endif

int
main()
{
    return 0;
}
