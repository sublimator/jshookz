#include "catl/xdata/static_protocol.h"

#include <cstdint>

int main() {
  using namespace catl::xdata;
  auto const &protocol = xahau_static_protocol();
  if (protocol.field_name_count != 337 || protocol.field_count != 327 ||
      protocol.material_field_count != 325 || protocol.type_count != 19 ||
      protocol.fallback_count != 0 || protocol.inferred_vl_count != 0 ||
      protocol.duplicate_word_count != 6)
    return 1;

  for (std::uint16_t ordinal = 0; ordinal < protocol.field_count; ++ordinal) {
    auto const *field = protocol.field_by_ordinal(ordinal);
    if (field == nullptr || protocol.field_by_code(field->code) != field)
      return 2;
    auto const name = protocol.field_name(field->name_ordinal);
    if (name.data == nullptr || name.size == 0)
      return 3;
  }
  for (std::uint16_t ordinal = 0; ordinal < protocol.material_field_count;
       ++ordinal) {
    auto const *material = protocol.material_field(ordinal);
    if (material == nullptr ||
        material->materializer == MaterializerKind::invalid)
      return 4;
    auto const *field = protocol.field_by_ordinal(material->admission_ordinal);
    if (field == nullptr || field->code != material->field_code ||
        field->material_ordinal != ordinal ||
        field->materializer != material->materializer)
      return 5;
  }
  if (protocol.field_by_code(0xffffffffu) != nullptr)
    return 6;
  return 0;
}
