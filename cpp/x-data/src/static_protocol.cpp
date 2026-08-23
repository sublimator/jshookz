#include "catl/xdata/static_protocol.h"
#include "static_xahau_protocol.h"

namespace catl::xdata {

StaticFieldDescriptor const *
ProtocolView::field_by_code(std::uint32_t field_code) const noexcept {
  auto const type_code = static_cast<std::uint16_t>(field_code >> 16);
  auto const nth = static_cast<std::uint16_t>(field_code);
  if (type_code < fast_type_count && nth < fast_nth_count) {
    auto const ordinal_plus_one = fast_ordinals[type_code][nth];
    return ordinal_plus_one == 0 ? nullptr
                                 : field_by_ordinal(ordinal_plus_one - 1);
  }

  std::uint16_t first = 0;
  std::uint16_t count = fallback_count;
  while (count != 0) {
    auto const step = static_cast<std::uint16_t>(count / 2);
    auto const index = static_cast<std::uint16_t>(first + step);
    auto const code = fallback_fields[index].field_code;
    if (code < field_code) {
      first = static_cast<std::uint16_t>(index + 1);
      count = static_cast<std::uint16_t>(count - step - 1);
    } else {
      count = step;
    }
  }
  if (first == fallback_count ||
      fallback_fields[first].field_code != field_code)
    return nullptr;
  return field_by_ordinal(fallback_fields[first].admission_ordinal);
}

ProtocolView const &xahau_static_protocol() noexcept {
  return xahau_static_data::PROTOCOL;
}

} // namespace catl::xdata
