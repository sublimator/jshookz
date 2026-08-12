// Implementation of embedded protocol loading functions
#include "catl/xdata/protocol.h"
#include "catl/xdata/exception_policy.h"
#include "embedded_xahau_definitions.h"  // Generated: constexpr char[]
#include "embedded_xrpl_definitions.h"   // Generated: constexpr char[]
#include <boost/json.hpp>

namespace catl::xdata {

Protocol
Protocol::load_embedded_xahau_protocol(const ProtocolOptions& opts)
{
    // Parse the embedded constexpr char array (no heap allocation for the source)
    boost::system::error_code ec;
    boost::json::value jv = boost::json::parse(
        std::string_view(xahau::EMBEDDED_DEFINITIONS), ec);

    if (ec)
    {
        CATL_XDATA_THROW(std::runtime_error(
            "Failed to parse embedded Xahau definitions: " + ec.message()));
    }

    return Protocol::load_from_json_value(jv, opts);
}

Protocol
Protocol::load_embedded_xrpl_protocol(const ProtocolOptions& opts)
{
    boost::system::error_code ec;
    boost::json::value jv = boost::json::parse(
        std::string_view(xrpl::EMBEDDED_DEFINITIONS), ec);

    if (ec)
    {
        CATL_XDATA_THROW(std::runtime_error(
            "Failed to parse embedded XRPL definitions: " + ec.message()));
    }

    return Protocol::load_from_json_value(jv, opts);
}

}  // namespace catl::xdata
