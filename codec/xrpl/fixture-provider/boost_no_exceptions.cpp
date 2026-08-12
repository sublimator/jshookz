#include <boost/assert/source_location.hpp>
#include <exception>

namespace boost {

[[noreturn]] void
throw_exception(std::exception const&)
{
    __builtin_trap();
    __builtin_unreachable();
}

[[noreturn]] void
throw_exception(std::exception const&, boost::source_location const&)
{
    __builtin_trap();
    __builtin_unreachable();
}

}  // namespace boost
