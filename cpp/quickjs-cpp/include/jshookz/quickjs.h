#pragma once

// The one relative include of the QuickJS C API. QuickJS's tree has a
// VERSION file that shadows C++ <version> if cpp/quickjs is on -I.
extern "C" {
#include "../../../quickjs/quickjs.h"
}
