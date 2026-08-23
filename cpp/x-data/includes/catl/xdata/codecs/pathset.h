#pragma once

#include "catl/xdata/codecs/account_id.h"
#include "catl/xdata/codecs/currency.h"
#include "catl/xdata/exception_policy.h"
#include "catl/xdata/parser-error.h"
#include "catl/xdata/pathset-rules.h"
#include "catl/xdata/serializer.h"
#include "catl/xdata/types/pathset.h"
#ifndef CATL_XDATA_NO_BOOST_JSON
#include <boost/json.hpp>
#endif

#include <string>

namespace catl::xdata::codecs {

#ifndef CATL_XDATA_NO_BOOST_JSON
struct PathSetCodec
{
    // Size requires walking the JSON to count hops
    static size_t
    encoded_size(boost::json::value const& v)
    {
        size_t size = 0;
        auto const& paths = v.as_array();
        for (size_t p = 0; p < paths.size(); ++p)
        {
            if (p > 0)
                ++size;  // PATH_SEPARATOR
            for (auto const& hop : paths[p].as_array())
            {
                ++size;  // type byte
                auto const& obj = hop.as_object();
                if (obj.contains("account"))
                    size += 20;
                if (obj.contains("currency"))
                    size += 20;
                if (obj.contains("issuer"))
                    size += 20;
            }
        }
        ++size;  // END_BYTE
        return size;
    }

    // From JSON: array of arrays of hop objects
    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, boost::json::value const& v)
    {
        auto const& paths = v.as_array();
        for (size_t p = 0; p < paths.size(); ++p)
        {
            if (p > 0)
            {
                s.add_path_separator();
            }
            for (auto const& hop : paths[p].as_array())
            {
                auto const& obj = hop.as_object();
                uint8_t type_byte = 0;
                if (obj.contains("account"))
                    type_byte |= PathSet::TYPE_ACCOUNT;
                if (obj.contains("currency"))
                    type_byte |= PathSet::TYPE_CURRENCY;
                if (obj.contains("issuer"))
                    type_byte |= PathSet::TYPE_ISSUER;

                s.add_u8(type_byte);

                if (obj.contains("account"))
                {
                    AccountIDCodec::encode_raw(
                        s, std::string_view(obj.at("account").as_string()));
                }
                if (obj.contains("currency"))
                {
                    CurrencyCodec::encode(
                        s, std::string_view(obj.at("currency").as_string()));
                }
                if (obj.contains("issuer"))
                {
                    AccountIDCodec::encode_raw(
                        s, std::string_view(obj.at("issuer").as_string()));
                }
            }
        }
        s.add_pathset_end();
    }

    static boost::json::value
    decode(Slice const& data)
    {
        struct JsonSink
        {
            boost::json::array paths;
            boost::json::array current_path;

            void
            on_hop(PathSetHop const& value)
            {
                boost::json::object hop;
                if (!value.account.empty())
                    hop["account"] = AccountIDCodec::decode(value.account);
                if (!value.currency.empty())
                    hop["currency"] = CurrencyCodec::decode(value.currency);
                if (!value.issuer.empty())
                    hop["issuer"] = AccountIDCodec::decode(value.issuer);
                current_path.push_back(std::move(hop));
            }

            void
            on_path_end()
            {
                paths.push_back(std::move(current_path));
                current_path = boost::json::array();
            }

            void
            on_end() const noexcept
            {
            }
        } sink;

        ParserContext ctx{data};
        if (!PathSetRules::walk<PathSetRuleMode::CertifyWire>(ctx, sink) ||
            ctx.failed() || ctx.pos() != data.size())
        {
            std::string const error =
                ctx.failed() ? ctx.as_error().message : "PathSet trailing bytes";
            CATL_XDATA_THROW(ParserError(error));
        }
        return std::move(sink.paths);
    }
};
#endif

}  // namespace catl::xdata::codecs
