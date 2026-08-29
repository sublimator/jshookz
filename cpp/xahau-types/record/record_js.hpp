#pragma once

#include <quickjs.h>

namespace jshookz::provider::types {

[[nodiscard]] bool
registerRecordSchemas(JSContext* context, JSValueConst global);

}  // namespace jshookz::provider::types
