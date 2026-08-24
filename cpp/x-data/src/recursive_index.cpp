#include "catl/xdata/recursive_index.h"

#include "catl/xdata/amount-rules.h"
#include "catl/xdata/number-rules.h"
#include "catl/xdata/parser-context.h"
#include "catl/xdata/pathset-rules.h"
#include "catl/xdata/types/issue.h"
#include "catl/xdata/types/pathset.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace catl::xdata {

char const *scan_message_literal(std::uint16_t message_id) noexcept {
  switch (static_cast<ScanMessage>(message_id)) {
  case ScanMessage::none:
    return "";
  case ScanMessage::begin_past_end:
    return "begin past end";
  case ScanMessage::input_too_large:
    return "serialized object exceeds byte limit";
  case ScanMessage::truncated_field_header:
    return "truncated field header";
  case ScanMessage::noncanonical_type_code:
    return "noncanonical extended type code";
  case ScanMessage::noncanonical_field_code:
    return "noncanonical extended field code";
  case ScanMessage::unknown_field:
    return "unknown field";
  case ScanMessage::illegal_terminator:
    return "illegal scope terminator";
  case ScanMessage::non_object_array_element:
    return "array element is not an STObject";
  case ScanMessage::too_many_nops:
    return "too many NOP fields";
  case ScanMessage::nesting_too_deep:
    return "nesting exceeds maximum depth";
  case ScanMessage::too_many_fields:
    return "serialized object exceeds field limit";
  case ScanMessage::too_many_scopes:
    return "serialized object exceeds scope limit";
  case ScanMessage::truncated_vl:
    return "truncated variable-length prefix";
  case ScanMessage::invalid_vl:
    return "invalid variable-length prefix";
  case ScanMessage::truncated_field:
    return "truncated field";
  case ScanMessage::invalid_account_id:
    return "invalid AccountID payload size";
  case ScanMessage::invalid_amount:
    return "invalid Amount representation";
  case ScanMessage::invalid_number:
    return "invalid Number representation";
  case ScanMessage::invalid_pathset:
    return "invalid PathSet representation";
  case ScanMessage::invalid_vector256:
    return "invalid Vector256 payload size";
  case ScanMessage::invalid_issue:
    return "invalid Issue representation";
  case ScanMessage::invalid_xchain_bridge:
    return "invalid XChainBridge representation";
  case ScanMessage::noncanonical_payload:
    return "field payload is not canonical";
  case ScanMessage::duplicate_field:
    return "duplicate object field";
  case ScanMessage::trailing_bytes:
    return "trailing bytes after root object";
  case ScanMessage::allocation_failed:
    return "allocation failed";
  case ScanMessage::index_size_overflow:
    return "recursive index size overflow";
  case ScanMessage::invalid_index:
    return "invalid recursive index";
  }
  return "unknown scan diagnostic";
}

bool recursive_index_size(std::uint32_t scope_count, std::uint32_t field_count,
                          std::uint32_t &size) noexcept {
  std::uint64_t const total = sizeof(IndexHeader) +
                              std::uint64_t{sizeof(ScopeRecord)} * scope_count +
                              std::uint64_t{sizeof(FieldRecord)} * field_count;
  if (scope_count == 0 || total > std::numeric_limits<std::uint32_t>::max())
    return false;
  size = static_cast<std::uint32_t>(total);
  return true;
}

namespace {

constexpr std::uint32_t kObjectEnd = (14u << 16) | 1u;
constexpr std::uint32_t kArrayEnd = (15u << 16) | 1u;
constexpr std::uint32_t kNop = (9u << 16) | 9u;
constexpr std::uint8_t kNopHeader = 0x99;

[[nodiscard]] std::uint32_t same_byte_prefix(std::uint8_t const *data,
                                             std::uint32_t size,
                                             std::uint8_t value) noexcept {
  constexpr std::uint32_t word_bytes = sizeof(std::uint64_t);
  std::uint64_t const repeated = std::uint64_t{value} * 0x0101010101010101ULL;
  std::uint32_t matched = 0;
  while (size - matched >= word_bytes) {
    std::uint64_t word = 0;
    std::memcpy(&word, data + matched, word_bytes);
    if (word != repeated)
      break;
    matched += word_bytes;
  }
  while (matched < size && data[matched] == value)
    ++matched;
  return matched;
}
constexpr std::uint32_t kInlineFields = 8;
constexpr std::uint32_t kInlineScopes = 4;

struct FieldScratch {
  std::uint32_t parent_scope;
  std::uint32_t encounter_order;
  FieldRecord field;
};

struct ScopeScratch {
  std::uint32_t content_begin;
  std::uint32_t content_end;
  std::uint32_t direct_count;
  std::uint32_t kind_close;
};

static_assert(sizeof(FieldScratch) == 28);
static_assert(sizeof(ScopeScratch) == 16);

struct Cursor {
  Slice bytes;
  std::uint32_t pos = 0;
  ScanStatus status{};

  [[nodiscard]] bool failed() const noexcept { return !status.ok(); }

  void fail(ScanIssue issue, ScanMessage message, std::uint32_t offset,
            std::uint32_t field_code = 0, std::uint32_t aux = 0) noexcept {
    if (failed())
      return;
    status = {static_cast<std::uint16_t>(issue),
              static_cast<std::uint16_t>(message), offset, field_code, aux};
  }

  [[nodiscard]] std::uint32_t remaining() const noexcept {
    return pos <= bytes.size() ? static_cast<std::uint32_t>(bytes.size() - pos)
                               : 0;
  }

  [[nodiscard]] std::uint8_t const *at() const noexcept {
    return bytes.data() + pos;
  }

  [[nodiscard]] bool advance(std::uint32_t count,
                             ScanMessage message = ScanMessage::truncated_field,
                             std::uint32_t field_code = 0) noexcept {
    if (failed())
      return false;
    if (count > remaining()) {
      fail(ScanIssue::malformed_data, message, pos, field_code, count);
      return false;
    }
    pos += count;
    return true;
  }

  [[nodiscard]] bool read_field_code(std::uint32_t &field_code) noexcept {
    std::uint32_t const start = pos;
    if (remaining() == 0) {
      fail(ScanIssue::malformed_data, ScanMessage::truncated_field_header,
           start);
      return false;
    }
    std::uint8_t const first = bytes.data()[pos++];
    std::uint32_t type = first >> 4;
    std::uint32_t nth = first & 0x0f;
    if (type == 0) {
      if (remaining() == 0) {
        pos = start;
        fail(ScanIssue::malformed_data, ScanMessage::truncated_field_header,
             start);
        return false;
      }
      type = bytes.data()[pos++];
      if (type < 16) {
        auto const bad = pos - 1;
        pos = start;
        fail(ScanIssue::malformed_data, ScanMessage::noncanonical_type_code,
             bad, 0, type);
        return false;
      }
    }
    if (nth == 0) {
      if (remaining() == 0) {
        pos = start;
        fail(ScanIssue::malformed_data, ScanMessage::truncated_field_header,
             start);
        return false;
      }
      nth = bytes.data()[pos++];
      if (nth < 16) {
        auto const bad = pos - 1;
        pos = start;
        fail(ScanIssue::malformed_data, ScanMessage::noncanonical_field_code,
             bad, 0, nth);
        return false;
      }
    }
    field_code = (type << 16) | nth;
    return true;
  }

  [[nodiscard]] bool read_vl_length(std::uint32_t &length,
                                    std::uint32_t field_code) noexcept {
    std::uint32_t const start = pos;
    if (remaining() == 0) {
      fail(ScanIssue::malformed_data, ScanMessage::truncated_vl, start,
           field_code);
      return false;
    }
    std::uint8_t const first = bytes.data()[pos++];
    if (first <= 192) {
      length = first;
      return true;
    }
    if (first <= 240) {
      if (remaining() < 1) {
        pos = start;
        fail(ScanIssue::malformed_data, ScanMessage::truncated_vl, start,
             field_code);
        return false;
      }
      length =
          193u + (std::uint32_t{first} - 193u) * 256u + bytes.data()[pos++];
      return true;
    }
    if (first <= 254) {
      if (remaining() < 2) {
        pos = start;
        fail(ScanIssue::malformed_data, ScanMessage::truncated_vl, start,
             field_code);
        return false;
      }
      std::uint32_t const second = bytes.data()[pos++];
      std::uint32_t const third = bytes.data()[pos++];
      length = 12481u + (std::uint32_t{first} - 241u) * 65536u + second * 256u +
               third;
      return true;
    }
    pos = start;
    fail(ScanIssue::malformed_data, ScanMessage::invalid_vl, start, field_code);
    return false;
  }
};

class FieldStore {
public:
  FieldStore(ScanAllocator allocator, std::uint32_t maximum) noexcept
      : allocator_(allocator), maximum_(maximum) {}

  ~FieldStore() {
    if (heap_ != nullptr)
      allocator_.free(allocator_.opaque, heap_);
  }

  FieldStore(FieldStore const &) = delete;
  FieldStore &operator=(FieldStore const &) = delete;

  [[nodiscard]] bool append(FieldScratch const &value) noexcept {
    if (size_ >= maximum_)
      return false;
    if (size_ == capacity_ && !grow())
      return false;
    data()[size_++] = value;
    return true;
  }

  [[nodiscard]] FieldScratch *data() noexcept {
    return heap_ != nullptr ? static_cast<FieldScratch *>(heap_) : inline_;
  }

  [[nodiscard]] FieldScratch const *data() const noexcept {
    return heap_ != nullptr ? static_cast<FieldScratch const *>(heap_)
                            : inline_;
  }

  [[nodiscard]] std::uint32_t size() const noexcept { return size_; }

private:
  [[nodiscard]] bool grow() noexcept {
    std::uint32_t next = capacity_ * 2;
    std::size_t const bytes = std::size_t{next} * sizeof(FieldScratch);
    void *replacement = allocator_.realloc(allocator_.opaque, heap_, bytes);
    if (replacement == nullptr)
      return false;
    if (heap_ == nullptr)
      std::memcpy(replacement, inline_, size_ * sizeof(FieldScratch));
    heap_ = replacement;
    capacity_ = next;
    return true;
  }

  ScanAllocator allocator_;
  std::uint32_t maximum_;
  std::uint32_t size_ = 0;
  std::uint32_t capacity_ = kInlineFields;
  void *heap_ = nullptr;
  FieldScratch inline_[kInlineFields]{};
};

class ScopeStore {
public:
  ScopeStore(ScanAllocator allocator, std::uint32_t maximum_scopes) noexcept
      : allocator_(allocator), maximum_(maximum_scopes) {}

  ~ScopeStore() {
    if (heap_ != nullptr)
      allocator_.free(allocator_.opaque, heap_);
  }

  ScopeStore(ScopeStore const &) = delete;
  ScopeStore &operator=(ScopeStore const &) = delete;

  void initialize_root(std::uint32_t content_begin) noexcept {
    inline_[0] = {
        content_begin, content_begin, 0,
        ScopeRecord::pack(0, ScopeKind::object, ScopeCloseKind::invalid)};
    size_ = 1;
  }

  [[nodiscard]] bool append_child(ScopeScratch const &value,
                                  std::uint32_t &scope_id) noexcept {
    if (size_ >= maximum_)
      return false;
    if (size_ == capacity_ && !grow())
      return false;
    data()[size_] = value;
    scope_id = size_++;
    return true;
  }

  [[nodiscard]] ScopeScratch &scope(std::uint32_t scope_id) noexcept {
    return data()[scope_id];
  }

  [[nodiscard]] ScopeScratch const &
  scope(std::uint32_t scope_id) const noexcept {
    return data()[scope_id];
  }

  [[nodiscard]] std::uint32_t size() const noexcept { return size_; }

private:
  [[nodiscard]] ScopeScratch *data() noexcept {
    return heap_ != nullptr ? static_cast<ScopeScratch *>(heap_) : inline_;
  }

  [[nodiscard]] ScopeScratch const *data() const noexcept {
    return heap_ != nullptr ? static_cast<ScopeScratch const *>(heap_)
                            : inline_;
  }

  [[nodiscard]] bool grow() noexcept {
    std::uint32_t next = capacity_ * 2;
    std::size_t const bytes = std::size_t{next} * sizeof(ScopeScratch);
    void *replacement = allocator_.realloc(allocator_.opaque, heap_, bytes);
    if (replacement == nullptr)
      return false;
    if (heap_ == nullptr)
      std::memcpy(replacement, inline_, size_ * sizeof(ScopeScratch));
    heap_ = replacement;
    capacity_ = next;
    return true;
  }

  ScanAllocator allocator_;
  std::uint32_t maximum_;
  std::uint32_t size_ = 0;
  std::uint32_t capacity_ = kInlineScopes;
  void *heap_ = nullptr;
  ScopeScratch inline_[kInlineScopes]{};
};

class IndexBuilder {
public:
  IndexBuilder(ScanAllocator allocator, RecursiveScanLimits const &limits,
               ProtocolView const &protocol) noexcept
      : allocator_(allocator), fields_(allocator, limits.max_fields),
        scopes_(allocator, limits.max_scopes), protocol_(protocol) {}

  void initialize_root(std::uint32_t begin) noexcept {
    scopes_.initialize_root(begin);
  }

  [[nodiscard]] bool add_scope(ScopeKind kind, std::uint32_t content_begin,
                               std::uint32_t &scope_id) noexcept {
    return scopes_.append_child(
        ScopeScratch{content_begin, content_begin, 0,
                     ScopeRecord::pack(0, kind, ScopeCloseKind::invalid)},
        scope_id);
  }

  [[nodiscard]] bool add_field(std::uint32_t parent_scope,
                               std::uint32_t encounter_order,
                               FieldRecord const &field,
                               std::uint32_t &scratch_ordinal) noexcept {
    scratch_ordinal = fields_.size();
    return fields_.append(FieldScratch{parent_scope, encounter_order, field});
  }

  void set_field_payload_begin(std::uint32_t scratch_ordinal,
                               std::uint32_t payload_begin) noexcept {
    fields_.data()[scratch_ordinal].field.payload_begin = payload_begin;
  }

  void finish_field(std::uint32_t scratch_ordinal, std::uint32_t wire_end,
                    std::uint32_t child_scope) noexcept {
    fields_.data()[scratch_ordinal].field.wire_end = wire_end;
    fields_.data()[scratch_ordinal].field.child_scope = child_scope;
  }

  void finish_scope(std::uint32_t scope_id, std::uint32_t content_end,
                    std::uint32_t direct_count, ScopeKind kind,
                    ScopeCloseKind close) noexcept {
    auto &scope = scopes_.scope(scope_id);
    scope.content_end = content_end;
    scope.direct_count = direct_count;
    scope.kind_close = ScopeRecord::pack(direct_count, kind, close);
  }

  [[nodiscard]] IndexBuildResult compact(std::uint32_t consumed,
                                         ScanStatus &status) noexcept {
    std::uint32_t index_size = 0;
    if (!recursive_index_size(scopes_.size(), fields_.size(), index_size)) {
      status = {static_cast<std::uint16_t>(ScanIssue::internal_error),
                static_cast<std::uint16_t>(ScanMessage::index_size_overflow),
                consumed, 0, 0};
      return {status, nullptr, 0, consumed};
    }
    void *allocation =
        allocator_.realloc(allocator_.opaque, nullptr, index_size);
    if (allocation == nullptr) {
      status = {static_cast<std::uint16_t>(ScanIssue::out_of_memory),
                static_cast<std::uint16_t>(ScanMessage::allocation_failed),
                consumed, 0, index_size};
      return {status, nullptr, 0, consumed};
    }
    auto *bytes = static_cast<std::uint8_t *>(allocation);
    auto *header = reinterpret_cast<IndexHeader *>(bytes);
    auto *scopes = reinterpret_cast<ScopeRecord *>(bytes + sizeof(*header));
    auto *fields = reinterpret_cast<FieldRecord *>(
        bytes + sizeof(*header) + sizeof(ScopeRecord) * scopes_.size());

    *header = {1, 0, scopes_.size(), fields_.size()};
    std::uint32_t first_field = 0;
    for (std::uint32_t i = 0; i < scopes_.size(); ++i) {
      auto &source = scopes_.scope(i);
      auto const direct_count = source.direct_count;
      scopes[i] = {source.content_begin, source.content_end, first_field,
                   source.kind_close};
      source.direct_count = first_field;
      first_field += direct_count;
    }
    if (first_field != fields_.size()) {
      allocator_.free(allocator_.opaque, allocation);
      status = {static_cast<std::uint16_t>(ScanIssue::internal_error),
                static_cast<std::uint16_t>(ScanMessage::invalid_index),
                consumed, 0, first_field};
      return {status, nullptr, 0, consumed};
    }
    for (std::uint32_t i = 0; i < fields_.size(); ++i) {
      auto const &source = fields_.data()[i];
      if (source.parent_scope >= scopes_.size()) {
        allocator_.free(allocator_.opaque, allocation);
        status = {static_cast<std::uint16_t>(ScanIssue::internal_error),
                  static_cast<std::uint16_t>(ScanMessage::invalid_index),
                  consumed, source.field.field_code, source.parent_scope};
        return {status, nullptr, 0, consumed};
      }
      auto const &scope = scopes[source.parent_scope];
      auto &write_cursor = scopes_.scope(source.parent_scope).direct_count;
      if (source.encounter_order >= scope.field_count() ||
          write_cursor != scope.first_field + source.encounter_order) {
        allocator_.free(allocator_.opaque, allocation);
        status = {static_cast<std::uint16_t>(ScanIssue::internal_error),
                  static_cast<std::uint16_t>(ScanMessage::invalid_index),
                  consumed, source.field.field_code, source.encounter_order};
        return {status, nullptr, 0, consumed};
      }
      fields[write_cursor++] = source.field;
    }
    for (std::uint32_t i = 0; i < scopes_.size(); ++i) {
      if (scopes[i].kind() != ScopeKind::object)
        continue;
      heap_sort(fields + scopes[i].first_field, scopes[i].field_count());
      for (std::uint32_t j = 1; j < scopes[i].field_count(); ++j) {
        auto const base = scopes[i].first_field;
        if (fields[base + j - 1].field_code == fields[base + j].field_code) {
          allocator_.free(allocator_.opaque, allocation);
          status = {static_cast<std::uint16_t>(ScanIssue::internal_error),
                    static_cast<std::uint16_t>(ScanMessage::duplicate_field),
                    fields[base + j].header_begin, fields[base + j].field_code,
                    0};
          return {status, nullptr, 0, consumed};
        }
      }
    }

    if (!validate_final(*header, scopes, fields, consumed)) {
      allocator_.free(allocator_.opaque, allocation);
      status = {static_cast<std::uint16_t>(ScanIssue::internal_error),
                static_cast<std::uint16_t>(ScanMessage::invalid_index),
                consumed, 0, 0};
      return {status, nullptr, 0, consumed};
    }
    return {ScanStatus::success(), allocation, index_size, consumed};
  }

private:
  [[nodiscard]] bool validate_final(IndexHeader const &header,
                                    ScopeRecord const *scopes,
                                    FieldRecord const *fields,
                                    std::uint32_t wire_size) noexcept {
    if (header.root_scope != 0 || header.scope_count == 0 ||
        header.scope_count > header.field_count + 1 ||
        scopes[0].kind() != ScopeKind::object)
      return false;

    for (std::uint32_t scope_id = 0; scope_id < header.scope_count; ++scope_id)
      scopes_.scope(scope_id).direct_count = 0;

    std::uint32_t covered_fields = 0;
    std::uint32_t child_fields = 0;
    for (std::uint32_t scope_id = 0; scope_id < header.scope_count;
         ++scope_id) {
      auto const &selected_scope = scopes[scope_id];
      if (selected_scope.first_field != covered_fields ||
          selected_scope.field_count() > header.field_count - covered_fields ||
          selected_scope.content_begin > selected_scope.content_end ||
          selected_scope.content_end > wire_size ||
          selected_scope.close_kind() == ScopeCloseKind::invalid ||
          (selected_scope.kind() == ScopeKind::object &&
           selected_scope.close_kind() == ScopeCloseKind::array_end) ||
          (selected_scope.kind() == ScopeKind::array &&
           selected_scope.close_kind() == ScopeCloseKind::object_end))
        return false;
      if (scope_id != 0 &&
          scopes[scope_id - 1].content_begin >= selected_scope.content_begin)
        return false;
      covered_fields += selected_scope.field_count();

      for (std::uint32_t i = 0; i < selected_scope.field_count(); ++i) {
        auto const &selected_field = fields[selected_scope.first_field + i];
        auto const *descriptor =
            protocol_.field_by_code(selected_field.field_code);
        if (descriptor == nullptr ||
            descriptor->material_ordinal == ProtocolView::no_ordinal ||
            selected_field.header_begin >= selected_field.payload_begin ||
            selected_field.payload_begin > selected_field.wire_end ||
            selected_field.header_begin < selected_scope.content_begin ||
            selected_field.wire_end > selected_scope.content_end)
          return false;
        if (i != 0 && selected_scope.kind() == ScopeKind::array &&
            fields[selected_scope.first_field + i - 1].wire_end >
                selected_field.header_begin)
          return false;

        bool const is_container =
            descriptor->wire_type == 14 || descriptor->wire_type == 15;
        if (selected_field.child_scope == FieldRecord::no_child) {
          if (is_container)
            return false;
          continue;
        }
        ++child_fields;
        auto const child_id = selected_field.child_scope;
        if (!is_container || child_id <= scope_id ||
            child_id >= header.scope_count)
          return false;
        auto const &child = scopes[child_id];
        if (child.content_begin != selected_field.payload_begin ||
            child.content_end > selected_field.wire_end ||
            child.kind() != (descriptor->wire_type == 14 ? ScopeKind::object
                                                         : ScopeKind::array))
          return false;
        if (child.close_kind() == ScopeCloseKind::eof) {
          if (child.content_end != selected_field.wire_end ||
              selected_field.wire_end != wire_size)
            return false;
        } else if (child.content_end == std::uint32_t(-1) ||
                   child.content_end + 1 != selected_field.wire_end) {
          return false;
        }
        auto &owners = scopes_.scope(child_id).direct_count;
        if (owners != 0)
          return false;
        owners = 1;
      }
    }
    if (covered_fields != header.field_count ||
        child_fields + 1 != header.scope_count)
      return false;
    for (std::uint32_t scope_id = 1; scope_id < header.scope_count;
         ++scope_id) {
      if (scopes_.scope(scope_id).direct_count != 1)
        return false;
    }
    auto const &root = scopes[0];
    return root.close_kind() == ScopeCloseKind::eof
               ? root.content_end == wire_size
               : root.content_end != std::uint32_t(-1) &&
                     root.content_end + 1 == wire_size;
  }

  static void swap(FieldRecord &left, FieldRecord &right) noexcept {
    FieldRecord const temporary = left;
    left = right;
    right = temporary;
  }

  static void sift_down(FieldRecord *fields, std::uint32_t root,
                        std::uint32_t count) noexcept {
    while (true) {
      std::uint32_t const child = root * 2 + 1;
      if (child >= count)
        return;
      std::uint32_t selected = child;
      if (child + 1 < count &&
          fields[child].field_code < fields[child + 1].field_code)
        selected = child + 1;
      if (fields[root].field_code >= fields[selected].field_code)
        return;
      swap(fields[root], fields[selected]);
      root = selected;
    }
  }

  static void heap_sort(FieldRecord *fields, std::uint32_t count) noexcept {
    if (count < 2)
      return;
    for (std::uint32_t root = count / 2; root != 0; --root)
      sift_down(fields, root - 1, count);
    for (std::uint32_t end = count - 1; end != 0; --end) {
      swap(fields[0], fields[end]);
      sift_down(fields, 0, end);
    }
  }

  ScanAllocator allocator_;
  FieldStore fields_;
  ScopeStore scopes_;
  ProtocolView const &protocol_;
};

[[nodiscard]] bool all_zero20(std::uint8_t const *value) noexcept {
  for (std::uint32_t i = 0; i < 20; ++i) {
    if (value[i] != 0)
      return false;
  }
  return true;
}

[[nodiscard]] bool valid_issue(std::uint8_t const *payload,
                               std::uint32_t size) noexcept {
  if (size == 20)
    return is_xrp_currency(payload);
  if (size == 44)
    return is_no_account(payload + 20);
  if (size != 40)
    return false;
  bool const native_currency = is_xrp_currency(payload);
  bool const native_account = all_zero20(payload + 20);
  return native_currency == native_account;
}

[[nodiscard]] bool canonical_number(Slice payload) noexcept {
  NormalizedNumber normalized;
  if (!NumberRules::normalize(payload, normalized))
    return false;
  std::uint8_t encoded[12];
  auto const mantissa = static_cast<std::uint64_t>(normalized.mantissa);
  auto const exponent = static_cast<std::uint32_t>(normalized.exponent);
  for (std::uint32_t i = 0; i < 8; ++i)
    encoded[i] = static_cast<std::uint8_t>(mantissa >> (56 - i * 8));
  for (std::uint32_t i = 0; i < 4; ++i)
    encoded[8 + i] = static_cast<std::uint8_t>(exponent >> (24 - i * 8));
  return payload.size() == sizeof(encoded) &&
         std::memcmp(payload.data(), encoded, sizeof(encoded)) == 0;
}

class Scanner {
public:
  Scanner(Slice bytes, std::uint32_t begin, RecursiveScanOptions const &options,
          IndexBuilder *builder) noexcept
      : cursor_{bytes, begin, {}},
        protocol_(options.protocol != nullptr ? *options.protocol
                                              : xahau_static_protocol()),
        limits_(options.limits), counters_(options.counters),
        builder_(builder) {
    if (counters_ != nullptr) {
      *counters_ = {};
      counters_->wire_passes = 1;
    }
  }

  [[nodiscard]] ConstructorParityResult run() noexcept {
    if (cursor_.bytes.size() > limits_.max_bytes) {
      cursor_.fail(ScanIssue::resource_limit, ScanMessage::input_too_large, 0,
                   0, static_cast<std::uint32_t>(cursor_.bytes.size()));
      return result();
    }
    if (cursor_.pos > cursor_.bytes.size()) {
      cursor_.fail(ScanIssue::malformed_data, ScanMessage::begin_past_end,
                   cursor_.pos);
      return result();
    }
    if (limits_.max_scopes == 0 || protocol_.duplicate_word_count != 6 ||
        limits_.max_bytes > RecursiveScanLimits{}.max_bytes ||
        limits_.max_fields > RecursiveScanLimits{}.max_fields ||
        limits_.max_scopes > RecursiveScanLimits{}.max_scopes ||
        limits_.max_depth > RecursiveScanLimits{}.max_depth ||
        protocol_.material_field_count > 6 * 64) {
      cursor_.fail(ScanIssue::internal_error, ScanMessage::invalid_index,
                   cursor_.pos);
      return result();
    }
    total_scopes_ = 1;
    if (builder_ != nullptr)
      builder_->initialize_root(cursor_.pos);
    (void)scan_scope(ScopeKind::object, 0, 0);
    return result();
  }

  [[nodiscard]] ConstructorParityResult
  run_field_payload(std::uint32_t field_code,
                    std::uint32_t parent_depth) noexcept {
    require_canonical_ = true;
    if (cursor_.bytes.size() > limits_.max_bytes) {
      cursor_.fail(ScanIssue::resource_limit, ScanMessage::input_too_large, 0,
                   field_code,
                   static_cast<std::uint32_t>(cursor_.bytes.size()));
      return result();
    }
    if (cursor_.pos != 0 || limits_.max_scopes == 0 ||
        protocol_.duplicate_word_count != 6 ||
        limits_.max_bytes > RecursiveScanLimits{}.max_bytes ||
        limits_.max_fields > RecursiveScanLimits{}.max_fields ||
        limits_.max_scopes > RecursiveScanLimits{}.max_scopes ||
        limits_.max_depth > RecursiveScanLimits{}.max_depth ||
        protocol_.material_field_count > 6 * 64) {
      cursor_.fail(ScanIssue::internal_error, ScanMessage::invalid_index,
                   cursor_.pos, field_code);
      return result();
    }
    auto const *descriptor = protocol_.field_by_code(field_code);
    if (descriptor == nullptr ||
        descriptor->material_ordinal == ProtocolView::no_ordinal) {
      cursor_.fail(ScanIssue::malformed_data, ScanMessage::unknown_field, 0,
                   field_code);
      return result();
    }

    if ((descriptor->flags & field_vl_encoded) != 0) {
      auto const size = static_cast<std::uint32_t>(cursor_.bytes.size());
      if (size > 918'744) {
        cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_vl, 0,
                     field_code, size);
      } else if (descriptor->wire_type == 8 && size != 0 && size != 20) {
        cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_account_id,
                     0, field_code, size);
      } else if (descriptor->wire_type == 19 && size % 32 != 0) {
        cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_vector256,
                     0, field_code, size);
      } else {
        (void)cursor_.advance(size, ScanMessage::truncated_field, field_code);
      }
    } else if (descriptor->wire_type == 6) {
      auto const size = static_cast<std::uint32_t>(cursor_.bytes.size());
      if (size == 0 || AmountRules::extent(cursor_.at()[0]) != size ||
          AmountRules::certify(cursor_.bytes) != nullptr)
        cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_amount, 0,
                     field_code, size);
      else
        (void)cursor_.advance(size);
    } else if (descriptor->wire_type == 18) {
      (void)scan_pathset(field_code);
    } else if (descriptor->wire_type == 9) {
      if (!canonical_number(cursor_.bytes))
        cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_number, 0,
                     field_code,
                     static_cast<std::uint32_t>(cursor_.bytes.size()));
      else
        (void)cursor_.advance(12);
    } else if (descriptor->wire_type == 14 || descriptor->wire_type == 15) {
      if (parent_depth >= limits_.max_depth) {
        cursor_.fail(ScanIssue::resource_limit, ScanMessage::nesting_too_deep,
                     0, field_code, parent_depth + 1);
      } else {
        total_scopes_ = 1;
        ScopeKind const kind =
            descriptor->wire_type == 14 ? ScopeKind::object : ScopeKind::array;
        (void)scan_scope(kind, parent_depth + 1, 0);
      }
    } else if (descriptor->wire_type == 24) {
      (void)scan_issue(field_code);
    } else if (descriptor->wire_type == 25) {
      (void)scan_xchain_bridge(field_code);
    } else if (descriptor->fixed_size == 0 ||
               descriptor->fixed_size != cursor_.bytes.size()) {
      cursor_.fail(ScanIssue::malformed_data, ScanMessage::truncated_field, 0,
                   field_code, descriptor->fixed_size);
    } else {
      (void)cursor_.advance(descriptor->fixed_size,
                            ScanMessage::truncated_field, field_code);
    }

    if (cursor_.status.ok() && cursor_.pos != cursor_.bytes.size()) {
      cursor_.fail(
          ScanIssue::malformed_data, ScanMessage::trailing_bytes, cursor_.pos,
          field_code,
          static_cast<std::uint32_t>(cursor_.bytes.size() - cursor_.pos));
    }
    return result();
  }

private:
  [[nodiscard]] ConstructorParityResult result() const noexcept {
    return {cursor_.status, cursor_.pos, total_scopes_, total_fields_};
  }

  void count_scope() noexcept {
    if (counters_ != nullptr)
      ++counters_->scope_entries;
  }

  void count_header() noexcept {
    if (counters_ != nullptr)
      ++counters_->field_headers;
  }

  void count_material() noexcept {
    if (counters_ != nullptr)
      ++counters_->material_fields;
  }

  void count_leaf() noexcept {
    if (counters_ != nullptr)
      ++counters_->leaf_routes;
  }

  void count_headers(std::uint32_t count) noexcept {
    if (counters_ != nullptr)
      counters_->field_headers += count;
  }

  [[nodiscard]] bool close_scope(std::uint32_t scope_id,
                                 std::uint32_t content_end,
                                 std::uint32_t direct_count, ScopeKind kind,
                                 ScopeCloseKind close) noexcept {
    if (builder_ != nullptr)
      builder_->finish_scope(scope_id, content_end, direct_count, kind, close);
    return true;
  }

  [[nodiscard]] bool scan_scope(ScopeKind kind, std::uint32_t depth,
                                std::uint32_t scope_id) noexcept {
    if (depth > limits_.max_depth) {
      cursor_.fail(ScanIssue::resource_limit, ScanMessage::nesting_too_deep,
                   cursor_.pos, 0, depth);
      return false;
    }
    count_scope();
    if (kind == ScopeKind::object)
      std::memset(duplicate_bits_[depth], 0, sizeof(duplicate_bits_[0]));

    std::uint32_t direct_count = 0;
    std::uint32_t nop_count = 0;
    std::uint32_t previous_field_code = 0;
    bool have_previous_field = false;
    while (!cursor_.failed()) {
      if (cursor_.remaining() == 0) {
        if (require_canonical_) {
          cursor_.fail(
              ScanIssue::malformed_data, ScanMessage::noncanonical_payload,
              cursor_.pos, 0,
              kind == ScopeKind::object
                  ? static_cast<std::uint32_t>(ExpectedTerminator::object_end)
                  : static_cast<std::uint32_t>(ExpectedTerminator::array_end));
          return false;
        }
        return close_scope(scope_id, cursor_.pos, direct_count, kind,
                           ScopeCloseKind::eof);
      }

      std::uint32_t const header_begin = cursor_.pos;
      std::uint32_t field_code = 0;
      if (!cursor_.read_field_code(field_code))
        return false;
      count_header();

      if (field_code == kNop) {
        if (require_canonical_) {
          cursor_.fail(ScanIssue::malformed_data,
                       ScanMessage::noncanonical_payload, header_begin,
                       field_code);
          return false;
        }
        ++nop_count;
        if (nop_count == 64) {
          cursor_.fail(ScanIssue::malformed_data, ScanMessage::too_many_nops,
                       header_begin, field_code, nop_count);
          return false;
        }
        std::uint32_t const allowed = 63 - nop_count;
        std::uint32_t const inspected =
            std::min(cursor_.remaining(), allowed + 1);
        std::uint32_t const run =
            same_byte_prefix(cursor_.at(), inspected, kNopHeader);
        if (run > allowed) {
          count_headers(allowed + 1);
          cursor_.fail(ScanIssue::malformed_data, ScanMessage::too_many_nops,
                       cursor_.pos + allowed, field_code, 64);
          return false;
        }
        if (run != 0) {
          count_headers(run);
          nop_count += run;
          if (!cursor_.advance(run))
            return false;
        }
        continue;
      }

      if (kind == ScopeKind::object && field_code == kObjectEnd)
        return close_scope(scope_id, header_begin, direct_count, kind,
                           ScopeCloseKind::object_end);
      if (kind == ScopeKind::array && field_code == kArrayEnd)
        return close_scope(scope_id, header_begin, direct_count, kind,
                           ScopeCloseKind::array_end);
      if (field_code == kObjectEnd || field_code == kArrayEnd) {
        auto const expected = kind == ScopeKind::array
                                  ? ExpectedTerminator::array_end
                              : depth == 0 ? ExpectedTerminator::root_eof
                                           : ExpectedTerminator::object_end;
        cursor_.fail(ScanIssue::malformed_data, ScanMessage::illegal_terminator,
                     header_begin, field_code,
                     static_cast<std::uint32_t>(expected));
        return false;
      }

      auto const *descriptor = protocol_.field_by_code(field_code);
      if (descriptor == nullptr ||
          descriptor->material_ordinal == ProtocolView::no_ordinal) {
        cursor_.fail(ScanIssue::malformed_data, ScanMessage::unknown_field,
                     header_begin, field_code);
        return false;
      }
      if (kind == ScopeKind::array && descriptor->wire_type != 14) {
        cursor_.fail(ScanIssue::malformed_data,
                     ScanMessage::non_object_array_element, header_begin,
                     field_code, descriptor->wire_type);
        return false;
      }
      if (require_canonical_ && kind == ScopeKind::object &&
          have_previous_field && field_code <= previous_field_code) {
        cursor_.fail(ScanIssue::malformed_data,
                     ScanMessage::noncanonical_payload, header_begin,
                     field_code, previous_field_code);
        return false;
      }
      previous_field_code = field_code;
      have_previous_field = true;
      if (total_fields_ >= limits_.max_fields) {
        cursor_.fail(ScanIssue::resource_limit, ScanMessage::too_many_fields,
                     header_begin, field_code, total_fields_ + 1);
        return false;
      }
      bool const is_container =
          descriptor->wire_type == 14 || descriptor->wire_type == 15;
      if (is_container && total_scopes_ >= limits_.max_scopes) {
        cursor_.fail(ScanIssue::resource_limit, ScanMessage::too_many_scopes,
                     header_begin, field_code, total_scopes_ + 1);
        return false;
      }
      if (is_container && depth >= limits_.max_depth) {
        cursor_.fail(ScanIssue::resource_limit, ScanMessage::nesting_too_deep,
                     header_begin, field_code, depth + 1);
        return false;
      }

      std::uint32_t const payload_prefix_begin = cursor_.pos;
      std::uint32_t scratch_ordinal = 0;
      if (builder_ != nullptr &&
          !builder_->add_field(scope_id, direct_count,
                               FieldRecord{field_code, header_begin,
                                           payload_prefix_begin, 0,
                                           FieldRecord::no_child},
                               scratch_ordinal)) {
        cursor_.fail(ScanIssue::out_of_memory, ScanMessage::allocation_failed,
                     header_begin, field_code,
                     std::uint32_t{sizeof(FieldScratch)} * (total_fields_ + 1));
        return false;
      }
      ++direct_count;
      ++total_fields_;
      count_material();

      std::uint32_t payload_begin = payload_prefix_begin;
      std::uint32_t child_scope = FieldRecord::no_child;
      if ((descriptor->flags & field_vl_encoded) != 0) {
        std::uint32_t length = 0;
        if (!cursor_.read_vl_length(length, field_code))
          return false;
        payload_begin = cursor_.pos;
        if (descriptor->wire_type == 8 && length != 0 && length != 20) {
          cursor_.fail(ScanIssue::malformed_data,
                       ScanMessage::invalid_account_id, payload_begin,
                       field_code, length);
          return false;
        }
        if (descriptor->wire_type == 19 && length % 32 != 0) {
          cursor_.fail(ScanIssue::malformed_data,
                       ScanMessage::invalid_vector256, payload_begin,
                       field_code, length);
          return false;
        }
        if (!cursor_.advance(length, ScanMessage::truncated_field, field_code))
          return false;
        count_leaf();
      } else if (descriptor->wire_type == 6) {
        if (cursor_.remaining() == 0) {
          cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_amount,
                       cursor_.pos, field_code);
          return false;
        }
        std::uint32_t const length =
            static_cast<std::uint32_t>(AmountRules::extent(cursor_.at()[0]));
        if (length > cursor_.remaining() ||
            AmountRules::certify(Slice{cursor_.at(), length}) != nullptr) {
          cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_amount,
                       cursor_.pos, field_code, length);
          return false;
        }
        (void)cursor_.advance(length);
        count_leaf();
      } else if (descriptor->wire_type == 18) {
        if (!scan_pathset(field_code))
          return false;
        count_leaf();
      } else if (descriptor->wire_type == 9) {
        if (cursor_.remaining() < 12 ||
            !NumberRules::certify(Slice{cursor_.at(), std::size_t{12}}) ||
            (require_canonical_ &&
             !canonical_number(Slice{cursor_.at(), std::size_t{12}}))) {
          cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_number,
                       cursor_.pos, field_code);
          return false;
        }
        (void)cursor_.advance(12);
        count_leaf();
      } else if (descriptor->wire_type == 14 || descriptor->wire_type == 15) {
        ScopeKind const child_kind =
            descriptor->wire_type == 14 ? ScopeKind::object : ScopeKind::array;
        child_scope = total_scopes_;
        if (builder_ != nullptr &&
            !builder_->add_scope(child_kind, payload_begin, child_scope)) {
          cursor_.fail(ScanIssue::out_of_memory, ScanMessage::allocation_failed,
                       payload_begin, field_code,
                       std::uint32_t{sizeof(ScopeScratch)} * total_scopes_);
          return false;
        }
        ++total_scopes_;
        if (!scan_scope(child_kind, depth + 1, child_scope))
          return false;
      } else if (descriptor->wire_type == 24) {
        if (!scan_issue(field_code))
          return false;
        count_leaf();
      } else if (descriptor->wire_type == 25) {
        if (!scan_xchain_bridge(field_code))
          return false;
        count_leaf();
      } else {
        if (descriptor->fixed_size == 0 ||
            !cursor_.advance(descriptor->fixed_size,
                             ScanMessage::truncated_field, field_code)) {
          if (!cursor_.failed())
            cursor_.fail(ScanIssue::internal_error, ScanMessage::unknown_field,
                         payload_begin, field_code, descriptor->wire_type);
          return false;
        }
        count_leaf();
      }

      if (builder_ != nullptr) {
        builder_->set_field_payload_begin(scratch_ordinal, payload_begin);
        builder_->finish_field(scratch_ordinal, cursor_.pos, child_scope);
      }
      if (kind == ScopeKind::object) {
        std::uint32_t const ordinal = descriptor->material_ordinal;
        std::uint32_t const word = ordinal >> 6;
        std::uint64_t const mask = std::uint64_t{1} << (ordinal & 63);
        if ((duplicate_bits_[depth][word] & mask) != 0) {
          cursor_.fail(ScanIssue::malformed_data, ScanMessage::duplicate_field,
                       header_begin, field_code, ordinal);
          return false;
        }
        duplicate_bits_[depth][word] |= mask;
      }
    }
    return false;
  }

  [[nodiscard]] bool scan_pathset(std::uint32_t field_code) noexcept {
    ParserContext context{cursor_.bytes};
    context.cursor.pos = cursor_.pos;
    PathSetNullSink sink;
    bool const accepted =
        PathSetRules::walk<PathSetRuleMode::CertifyWire>(context, sink);
    cursor_.pos = static_cast<std::uint32_t>(context.pos());
    if (accepted && !context.failed())
      return true;
    cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_pathset,
                 context.fail_offset(), field_code);
    return false;
  }

  [[nodiscard]] bool issue_extent(std::uint32_t field_code,
                                  std::uint32_t &size) noexcept {
    if (cursor_.remaining() < 20) {
      cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_issue,
                   cursor_.pos, field_code);
      return false;
    }
    if (is_xrp_currency(cursor_.at())) {
      size = 20;
      return true;
    }
    if (cursor_.remaining() < 40) {
      cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_issue,
                   cursor_.pos, field_code);
      return false;
    }
    if (is_no_account(cursor_.at() + 20)) {
      if (cursor_.remaining() < 44) {
        cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_issue,
                     cursor_.pos, field_code);
        return false;
      }
      size = 44;
      return true;
    }
    size = 40;
    return true;
  }

  [[nodiscard]] bool scan_issue(std::uint32_t field_code) noexcept {
    std::uint32_t size = 0;
    if (!issue_extent(field_code, size))
      return false;
    if (!valid_issue(cursor_.at(), size)) {
      cursor_.fail(ScanIssue::malformed_data, ScanMessage::invalid_issue,
                   cursor_.pos, field_code, size);
      return false;
    }
    return cursor_.advance(size, ScanMessage::invalid_issue, field_code);
  }

  [[nodiscard]] bool scan_xchain_bridge(std::uint32_t field_code) noexcept {
    for (std::uint32_t door = 0; door < 2; ++door) {
      std::uint32_t length = 0;
      if (!cursor_.read_vl_length(length, field_code))
        return false;
      if (length != 0 && length != 20) {
        cursor_.fail(ScanIssue::malformed_data,
                     ScanMessage::invalid_xchain_bridge, cursor_.pos,
                     field_code, length);
        return false;
      }
      if (!cursor_.advance(length, ScanMessage::invalid_xchain_bridge,
                           field_code) ||
          !scan_issue(field_code))
        return false;
    }
    return true;
  }

  Cursor cursor_;
  ProtocolView const &protocol_;
  RecursiveScanLimits limits_;
  RecursiveScanCounters *counters_;
  IndexBuilder *builder_;
  std::uint32_t total_scopes_ = 0;
  std::uint32_t total_fields_ = 0;
  bool require_canonical_ = false;
  using DuplicateBits = std::uint64_t[11][6];
  static_assert(sizeof(DuplicateBits) == 528);
  DuplicateBits duplicate_bits_{};
};

[[nodiscard]] RecursiveScanOptions
normalized_options(RecursiveScanOptions const &options) noexcept {
  RecursiveScanOptions normalized = options;
  if (normalized.protocol == nullptr)
    normalized.protocol = &xahau_static_protocol();
  return normalized;
}

} // namespace

IndexHeader const *RecursiveIndexView::header() const noexcept {
  if (data_ == nullptr || size_ < sizeof(IndexHeader))
    return nullptr;
  return reinterpret_cast<IndexHeader const *>(data_);
}

ScopeRecord const *
RecursiveIndexView::scope(std::uint32_t scope_id) const noexcept {
  auto const *h = header();
  if (h == nullptr || scope_id >= h->scope_count)
    return nullptr;
  std::uint32_t expected = 0;
  if (!recursive_index_size(h->scope_count, h->field_count, expected) ||
      expected != size_)
    return nullptr;
  auto const *scopes =
      reinterpret_cast<ScopeRecord const *>(data_ + sizeof(IndexHeader));
  return scopes + scope_id;
}

FieldRecord const *
RecursiveIndexView::field(std::uint32_t field_ordinal) const noexcept {
  auto const *h = header();
  if (h == nullptr || field_ordinal >= h->field_count)
    return nullptr;
  std::uint32_t expected = 0;
  if (!recursive_index_size(h->scope_count, h->field_count, expected) ||
      expected != size_)
    return nullptr;
  auto const *fields = reinterpret_cast<FieldRecord const *>(
      data_ + sizeof(IndexHeader) + sizeof(ScopeRecord) * h->scope_count);
  return fields + field_ordinal;
}

FieldRecord const *
RecursiveIndexView::find_object_field(std::uint32_t scope_id,
                                      std::uint32_t field_code) const noexcept {
  auto const *selected_scope = scope(scope_id);
  if (selected_scope == nullptr || selected_scope->kind() != ScopeKind::object)
    return nullptr;
  std::uint32_t first = 0;
  std::uint32_t count = selected_scope->field_count();
  while (count != 0) {
    std::uint32_t const step = count / 2;
    std::uint32_t const index = first + step;
    auto const *candidate = field(selected_scope->first_field + index);
    if (candidate == nullptr)
      return nullptr;
    if (candidate->field_code < field_code) {
      first = index + 1;
      count -= step + 1;
    } else {
      count = step;
    }
  }
  if (first >= selected_scope->field_count())
    return nullptr;
  auto const *candidate = field(selected_scope->first_field + first);
  return candidate != nullptr && candidate->field_code == field_code ? candidate
                                                                     : nullptr;
}

FieldRecord const *
RecursiveIndexView::array_element(std::uint32_t scope_id,
                                  std::uint32_t index) const noexcept {
  auto const *selected_scope = scope(scope_id);
  if (selected_scope == nullptr || selected_scope->kind() != ScopeKind::array ||
      index >= selected_scope->field_count())
    return nullptr;
  return field(selected_scope->first_field + index);
}

bool RecursiveIndexView::structurally_valid() const noexcept {
  auto const *h = header();
  if (h == nullptr || h->format_version != 1 || h->root_scope != 0 ||
      h->scope_count == 0 ||
      std::uint64_t{h->scope_count} > std::uint64_t{h->field_count} + 1)
    return false;
  std::uint32_t expected = 0;
  if (!recursive_index_size(h->scope_count, h->field_count, expected) ||
      expected != size_)
    return false;

  auto const *root = scope(0);
  if (root == nullptr || root->kind() != ScopeKind::object)
    return false;
  std::uint32_t covered_fields = 0;
  std::uint32_t child_fields = 0;
  auto const &protocol = xahau_static_protocol();
  for (std::uint32_t scope_id = 0; scope_id < h->scope_count; ++scope_id) {
    auto const *selected_scope = scope(scope_id);
    if (selected_scope == nullptr ||
        selected_scope->close_kind() == ScopeCloseKind::invalid ||
        selected_scope->content_begin > selected_scope->content_end ||
        selected_scope->content_end > wire_size_ ||
        selected_scope->first_field != covered_fields ||
        selected_scope->field_count() > h->field_count - covered_fields)
      return false;
    covered_fields += selected_scope->field_count();
    if (scope_id != 0 &&
        scope(scope_id - 1)->content_begin >= selected_scope->content_begin)
      return false;
    if ((selected_scope->kind() == ScopeKind::object &&
         selected_scope->close_kind() == ScopeCloseKind::array_end) ||
        (selected_scope->kind() == ScopeKind::array &&
         selected_scope->close_kind() == ScopeCloseKind::object_end))
      return false;

    std::uint32_t previous_code = 0;
    std::uint32_t previous_header = 0;
    for (std::uint32_t i = 0; i < selected_scope->field_count(); ++i) {
      auto const *selected_field = field(selected_scope->first_field + i);
      auto const *descriptor =
          selected_field == nullptr
              ? nullptr
              : protocol.field_by_code(selected_field->field_code);
      if (selected_field == nullptr || descriptor == nullptr ||
          descriptor->material_ordinal == ProtocolView::no_ordinal ||
          selected_field->header_begin >= selected_field->payload_begin ||
          selected_field->payload_begin > selected_field->wire_end ||
          selected_field->header_begin < selected_scope->content_begin ||
          selected_field->wire_end > selected_scope->content_end)
        return false;
      for (std::uint32_t prior = 0; prior < i; ++prior) {
        auto const *other = field(selected_scope->first_field + prior);
        if (other == nullptr ||
            !(other->wire_end <= selected_field->header_begin ||
              selected_field->wire_end <= other->header_begin))
          return false;
      }
      if (selected_field->child_scope != FieldRecord::no_child) {
        ++child_fields;
        if ((descriptor->wire_type != 14 && descriptor->wire_type != 15) ||
            selected_field->child_scope <= scope_id ||
            selected_field->child_scope >= h->scope_count)
          return false;
        auto const *child = scope(selected_field->child_scope);
        if (child == nullptr ||
            child->content_begin != selected_field->payload_begin ||
            child->content_end > selected_field->wire_end ||
            child->kind() != (descriptor->wire_type == 14 ? ScopeKind::object
                                                          : ScopeKind::array))
          return false;
        if (child->close_kind() == ScopeCloseKind::eof) {
          if (child->content_end != selected_field->wire_end ||
              selected_field->wire_end != wire_size_)
            return false;
        } else if (child->content_end ==
                       std::numeric_limits<std::uint32_t>::max() ||
                   child->content_end + 1 != selected_field->wire_end)
          return false;
      } else if (descriptor->wire_type == 14 || descriptor->wire_type == 15)
        return false;
      if (selected_scope->kind() == ScopeKind::array &&
          (descriptor->wire_type != 14 ||
           selected_field->child_scope == FieldRecord::no_child))
        return false;
      if (i != 0) {
        if (selected_scope->kind() == ScopeKind::object &&
            previous_code >= selected_field->field_code)
          return false;
        if (selected_scope->kind() == ScopeKind::array &&
            previous_header >= selected_field->header_begin)
          return false;
      }
      previous_code = selected_field->field_code;
      previous_header = selected_field->header_begin;
    }
  }
  if (covered_fields != h->field_count || child_fields + 1 != h->scope_count)
    return false;
  for (std::uint32_t child_scope = 1; child_scope < h->scope_count;
       ++child_scope) {
    std::uint32_t owners = 0;
    for (std::uint32_t field_id = 0; field_id < h->field_count; ++field_id) {
      auto const *candidate = field(field_id);
      if (candidate != nullptr && candidate->child_scope == child_scope)
        ++owners;
    }
    if (owners != 1)
      return false;
  }
  if (root->close_kind() == ScopeCloseKind::eof)
    return root->content_end == wire_size_;
  return root->content_end != std::numeric_limits<std::uint32_t>::max() &&
         root->content_end + 1 == wire_size_;
}

ConstructorParityResult
constructor_parity_scan(Slice bytes, std::uint32_t begin,
                        RecursiveScanOptions const &options) noexcept {
  auto normalized = normalized_options(options);
  Scanner scanner{bytes, begin, normalized, nullptr};
  return scanner.run();
}

ScanStatus
guest_exact_validate_object(Slice bytes,
                            RecursiveScanOptions const &options) noexcept {
  auto const result = constructor_parity_scan(bytes, 0, options);
  if (!result.status.ok())
    return result.status;
  if (result.consumed != bytes.size()) {
    return {static_cast<std::uint16_t>(ScanIssue::malformed_data),
            static_cast<std::uint16_t>(ScanMessage::trailing_bytes),
            result.consumed, 0,
            static_cast<std::uint32_t>(bytes.size() - result.consumed)};
  }
  return ScanStatus::success();
}

ScanStatus guest_exact_validate_field_payload(
    Slice payload, std::uint32_t field_code, std::uint32_t parent_depth,
    RecursiveScanOptions const &options) noexcept {
  auto normalized = normalized_options(options);
  Scanner scanner{payload, 0, normalized, nullptr};
  return scanner.run_field_payload(field_code, parent_depth).status;
}

IndexBuildResult constructor_parity_index(Slice bytes, std::uint32_t begin,
                                          RecursiveScanOptions const &options,
                                          ScanAllocator allocator) noexcept {
  if (!allocator.valid()) {
    ScanStatus const status{
        static_cast<std::uint16_t>(ScanIssue::internal_error),
        static_cast<std::uint16_t>(ScanMessage::invalid_index), begin, 0, 0};
    return {status, nullptr, 0, begin};
  }
  auto normalized = normalized_options(options);
  IndexBuilder builder{allocator, normalized.limits, *normalized.protocol};
  Scanner scanner{bytes, begin, normalized, &builder};
  auto scan = scanner.run();
  if (!scan.status.ok())
    return {scan.status, nullptr, 0, scan.consumed};
  return builder.compact(scan.consumed, scan.status);
}

IndexBuildResult guest_exact_object_index(Slice bytes,
                                          RecursiveScanOptions const &options,
                                          ScanAllocator allocator) noexcept {
  if (!allocator.valid()) {
    ScanStatus const status{
        static_cast<std::uint16_t>(ScanIssue::internal_error),
        static_cast<std::uint16_t>(ScanMessage::invalid_index), 0, 0, 0};
    return {status, nullptr, 0, 0};
  }
  auto normalized = normalized_options(options);
  IndexBuilder builder{allocator, normalized.limits, *normalized.protocol};
  Scanner scanner{bytes, 0, normalized, &builder};
  auto scan = scanner.run();
  if (!scan.status.ok())
    return {scan.status, nullptr, 0, scan.consumed};
  if (scan.consumed != bytes.size()) {
    scan.status = {static_cast<std::uint16_t>(ScanIssue::malformed_data),
                   static_cast<std::uint16_t>(ScanMessage::trailing_bytes),
                   scan.consumed, 0,
                   static_cast<std::uint32_t>(bytes.size() - scan.consumed)};
    return {scan.status, nullptr, 0, scan.consumed};
  }
  return builder.compact(scan.consumed, scan.status);
}

} // namespace catl::xdata
