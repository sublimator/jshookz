#pragma once

#include "catl/xdata/serializer.h"
#ifndef CATL_XDATA_NO_BOOST_JSON
#include <boost/json.hpp>
#endif

namespace catl::xdata::codecs {

struct Int32Codec
{
    static constexpr size_t fixed_size = 4;

#ifndef CATL_XDATA_NO_BOOST_JSON
    static size_t
    encoded_size(boost::json::value const&)
    {
        return 4;
    }
#endif

    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, int32_t v)
    {
        s.add_u32(static_cast<uint32_t>(v));
    }

    static int32_t
    decode_raw(Slice const& data)
    {
        return static_cast<int32_t>(
            (static_cast<uint32_t>(data.data()[0]) << 24) |
            (static_cast<uint32_t>(data.data()[1]) << 16) |
            (static_cast<uint32_t>(data.data()[2]) << 8) |
            static_cast<uint32_t>(data.data()[3]));
    }

#ifndef CATL_XDATA_NO_BOOST_JSON
    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, boost::json::value const& v)
    {
        s.add_u32(static_cast<uint32_t>(v.as_int64()));
    }

    static boost::json::value
    decode(Slice const& data)
    {
        return static_cast<std::int64_t>(decode_raw(data));
    }
#endif
};

struct Int64Codec
{
    static constexpr size_t fixed_size = 8;

#ifndef CATL_XDATA_NO_BOOST_JSON
    static size_t
    encoded_size(boost::json::value const&)
    {
        return 8;
    }
#endif

    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, int64_t v)
    {
        s.add_u64(static_cast<uint64_t>(v));
    }

    static int64_t
    decode_raw(Slice const& data)
    {
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i)
        {
            u = (u << 8) | data.data()[i];
        }
        return static_cast<int64_t>(u);
    }

#ifndef CATL_XDATA_NO_BOOST_JSON
    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, boost::json::value const& v)
    {
        s.add_u64(static_cast<uint64_t>(v.as_int64()));
    }

    static boost::json::value
    decode(Slice const& data)
    {
        return static_cast<std::int64_t>(decode_raw(data));
    }
#endif
};

}  // namespace catl::xdata::codecs
