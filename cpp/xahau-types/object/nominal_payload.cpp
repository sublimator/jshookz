#include "nominal_payload.hpp"

#include <cstring>

namespace jshookz::provider::types {

namespace xdata = catl::xdata;

bool readNominalPayload(JSContext *ctx, JSValueConst input,
                        xdata::MaterializerKind expected,
                        std::uint8_t (&integerScratch)[8],
                        NominalPayloadView &output) noexcept {
  std::memset(integerScratch, 0, sizeof(integerScratch));
  output = {};
  if (ctx == nullptr)
    return false;

  std::uint8_t bits = 0;
  switch (expected) {
  case xdata::MaterializerKind::uint8:
    bits = 8;
    break;
  case xdata::MaterializerKind::uint16:
    bits = 16;
    break;
  case xdata::MaterializerKind::uint32:
    bits = 32;
    break;
  case xdata::MaterializerKind::uint64:
    bits = 64;
    break;
  default:
    break;
  }
  if (bits != 0) {
    std::uint64_t value = 0;
    if (!detail::readUIntNominalPayload(input, bits, value))
      return false;
    auto const size = static_cast<std::uint32_t>(bits / 8);
    for (std::uint32_t i = size; i != 0; --i) {
      integerScratch[i - 1] = static_cast<std::uint8_t>(value);
      value >>= 8;
    }
    output = {integerScratch, size};
    return true;
  }

  switch (expected) {
  case xdata::MaterializerKind::hash256:
    return detail::readHash256NominalPayload(input, output);
  case xdata::MaterializerKind::blob:
    return detail::readSTBlobNominalPayload(input, output);
  case xdata::MaterializerKind::account_id:
    return detail::readAccountIDNominalPayload(input, output);
  case xdata::MaterializerKind::amount:
    return detail::readAmountNominalPayload(input, output);
  case xdata::MaterializerKind::hash128:
  case xdata::MaterializerKind::hash160:
  case xdata::MaterializerKind::hash192:
  case xdata::MaterializerKind::currency:
  case xdata::MaterializerKind::issue:
  case xdata::MaterializerKind::vector256:
  case xdata::MaterializerKind::xchain_bridge:
    return detail::readRichLeafNominalPayload(ctx, input, expected, output);
  case xdata::MaterializerKind::path_set:
    return detail::readPathSetNominalPayload(ctx, input, output);
  case xdata::MaterializerKind::invalid:
  case xdata::MaterializerKind::number:
  case xdata::MaterializerKind::st_object:
  case xdata::MaterializerKind::st_array:
  case xdata::MaterializerKind::ledger_entry_type:
  case xdata::MaterializerKind::transaction_type:
  case xdata::MaterializerKind::transaction_result:
  case xdata::MaterializerKind::uint8:
  case xdata::MaterializerKind::uint16:
  case xdata::MaterializerKind::uint32:
  case xdata::MaterializerKind::uint64:
    return false;
  }
  return false;
}

} // namespace jshookz::provider::types
