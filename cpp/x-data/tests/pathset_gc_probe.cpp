#include "pathset_quickjs_fixture.h"

#include <quickjs.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using catl::xdata::test::parse_pathset_mark_edge;
using catl::xdata::test::pathset_mark_edge_name;
using catl::xdata::test::PathSetMarkEdge;
using catl::xdata::test::PathSetQuickJSFixture;

namespace {

void append_component(std::vector<std::uint8_t> &bytes, std::uint8_t first) {
  for (std::uint8_t i = 0; i < 20; ++i)
    bytes.push_back(static_cast<std::uint8_t>(first + i));
}

std::vector<std::uint8_t> one_path_object() {
  std::vector<std::uint8_t> bytes{0x01, 0x12, 0x31};
  append_component(bytes, 0xa0);
  append_component(bytes, 0xc0);
  append_component(bytes, 0xe0);
  bytes.push_back(0x00);
  return bytes;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s EDGE enabled|disabled\n", argv[0]);
    return 64;
  }
  PathSetMarkEdge edge = PathSetMarkEdge::none;
  if (!parse_pathset_mark_edge(argv[1], edge)) {
    std::fprintf(stderr, "unknown edge: %s\n", argv[1]);
    return 64;
  }
  bool const disabled = std::strcmp(argv[2], "disabled") == 0;
  if (!disabled && std::strcmp(argv[2], "enabled") != 0) {
    std::fprintf(stderr, "unknown mark mode: %s\n", argv[2]);
    return 64;
  }

  PathSetQuickJSFixture fixture(disabled ? edge : PathSetMarkEdge::none);
  if (!fixture.ready()) {
    std::fprintf(stderr, "fixture init failed: %s\n", fixture.init_error());
    return 70;
  }
  auto object = one_path_object();
  JSValue facade = fixture.make_facade(object);
  if (JS_IsException(facade)) {
    std::fprintf(stderr, "facade mint failed: %s\n",
                 fixture.take_exception_message().c_str());
    return 70;
  }
  if (!fixture.plant_cycle(edge, facade)) {
    std::fprintf(stderr, "cycle plant failed: %s\n",
                 fixture.take_exception_message().c_str());
    JS_FreeValue(fixture.context(), facade);
    return 70;
  }
  JS_FreeValue(fixture.context(), facade);
  fixture.run_gc();

  bool const collected = fixture.cycle_collected(edge);
  if (!disabled) {
    if (!collected) {
      std::fprintf(stderr, "enabled edge retained: %s\n", argv[1]);
      return 71;
    }
    std::printf("collected %s\n", pathset_mark_edge_name(edge));
    return 0;
  }

  if (collected) {
    std::fprintf(stderr, "disabled edge unexpectedly collected: %s\n", argv[1]);
    return 72;
  }
  std::printf("retained %s\n", pathset_mark_edge_name(edge));
  if (!fixture.cleanup_red_cycle()) {
    std::fprintf(stderr, "red-cycle cleanup failed: %s\n", argv[1]);
    return 73;
  }

  // Intentional nonzero: the runner requires this exact failure receipt.
  return 23;
}
