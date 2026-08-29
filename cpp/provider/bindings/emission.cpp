#include "common.hpp"
#include "hook_imports.hpp"
#include "../generated/transaction_builders.inc"
#include "../provider_internal.hpp"
#include "js.hpp"
#include "object/nominal_payload.hpp"

#include "catl/xdata/canonical_writer.h"
#include "runtime_profile_limits.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace jshookz::provider::bindings {
namespace {

namespace xdata = catl::xdata;
namespace limits = catl::xdata::xahau_profile_limits;
using generated::BuilderFieldCode;
using generated::BuilderOption;
using generated::TransactionBuilderFieldSpec;

constexpr std::uint8_t objectEndMarker = 0xE1;
constexpr std::uint8_t arrayEndMarker = 0xF1;
constexpr std::uint32_t maximumHookCount = 10;
constexpr std::uint32_t maximumParameterCount = 16;
constexpr std::uint32_t maximumGrantCount = 8;
constexpr std::uint32_t maximumParameterNameBytes = 32;
constexpr std::uint32_t maximumParameterValueBytes = 256;
constexpr std::uint64_t maximumNativeDrops = 100'000'000'000'000'000ULL;
constexpr std::uint64_t nativeAmountBit = 1ULL << 62;
constexpr std::uint32_t fullyCanonicalSignatureFlag = 0x80000000U;
constexpr std::array<std::uint8_t, 33> zeroSigningPublicKey{};

[[nodiscard]] constexpr std::uint32_t
wireCode(BuilderFieldCode code) noexcept
{
    return static_cast<std::uint32_t>(code);
}

enum class ParseDisposition : std::uint8_t
{
    ok,
    encodeFailure,
    exception,
};

struct ParseOutcome
{
    ParseDisposition disposition = ParseDisposition::ok;
    char const *field = nullptr;

    [[nodiscard]] static constexpr ParseOutcome success() noexcept
    {
        return {};
    }

    [[nodiscard]] static constexpr ParseOutcome encode(
        char const *fieldName) noexcept
    {
        return {ParseDisposition::encodeFailure, fieldName};
    }

    [[nodiscard]] static constexpr ParseOutcome exception() noexcept
    {
        return {ParseDisposition::exception, nullptr};
    }
};

template <class T>
class ContextAllocation
{
    JSContext *context_ = nullptr;
    T *value_ = nullptr;

public:
    explicit ContextAllocation(JSContext *context) noexcept
        : context_(context),
          value_(static_cast<T *>(js_mallocz(context, sizeof(T))))
    {
    }

    ~ContextAllocation()
    {
        if (value_ != nullptr)
            js_free(context_, value_);
    }

    ContextAllocation(ContextAllocation const &) = delete;
    ContextAllocation &operator=(ContextAllocation const &) = delete;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value_ != nullptr;
    }

    [[nodiscard]] T *operator->() noexcept { return value_; }
    [[nodiscard]] T &operator*() noexcept { return *value_; }
};

template <std::size_t Capacity>
struct FixedBytes
{
    std::array<std::uint8_t, Capacity> bytes{};
    std::uint16_t size = 0;
    bool present = false;

    [[nodiscard]] std::span<std::uint8_t const> view() const noexcept
    {
        return {bytes.data(), size};
    }
};

void setUInt32(FixedBytes<4> &output, std::uint32_t value) noexcept
{
    output.bytes = {
        static_cast<std::uint8_t>(value >> 24),
        static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(value),
    };
    output.size = 4;
    output.present = true;
}

[[nodiscard]] std::uint32_t readUInt32(FixedBytes<4> const &value) noexcept
{
    return (static_cast<std::uint32_t>(value.bytes[0]) << 24) |
        (static_cast<std::uint32_t>(value.bytes[1]) << 16) |
        (static_cast<std::uint32_t>(value.bytes[2]) << 8) |
        static_cast<std::uint32_t>(value.bytes[3]);
}

struct HookParameterSnapshot
{
    FixedBytes<maximumParameterNameBytes> name;
    FixedBytes<maximumParameterValueBytes> value;
};

struct HookParameterList
{
    std::array<HookParameterSnapshot, maximumParameterCount> values{};
    std::uint8_t count = 0;
    bool present = false;
};

struct HookGrantSnapshot
{
    FixedBytes<32> hookHash;
    FixedBytes<20> authorize;
};

struct HookGrantList
{
    std::array<HookGrantSnapshot, maximumGrantCount> values{};
    std::uint8_t count = 0;
    bool present = false;
};

enum class HookActionKind : std::uint8_t
{
    blank,
    reference,
    deletion,
};

struct HookActionSnapshot
{
    HookActionKind kind = HookActionKind::blank;
    FixedBytes<32> hookHash;
    FixedBytes<32> hookNamespace;
    HookParameterList parameters;
    HookGrantList grants;
};

struct PaymentSnapshot
{
    FixedBytes<4> flags;
    FixedBytes<4> sourceTag;
    FixedBytes<4> destinationTag;
    FixedBytes<32> invoiceId;
    FixedBytes<48> amount;
    FixedBytes<48> sendMax;
    FixedBytes<48> deliverMin;
    FixedBytes<20> account;
    FixedBytes<20> destination;
    HookParameterList hookParameters;
};

struct HookSetSnapshot
{
    FixedBytes<4> flags;
    FixedBytes<20> account;
    std::array<HookActionSnapshot, maximumHookCount> hooks{};
    std::uint8_t hookCount = 0;
    HookParameterList hookParameters;
};

struct CommonSnapshot
{
    std::array<std::uint8_t, 20> account{};
    std::array<std::uint8_t, 138> details{};
    std::uint32_t detailsSize = 0;
    std::uint32_t ledgerSequence = 0;
    std::uint16_t transactionType = 0;
};

struct BuildStats
{
    std::uint32_t fields = 0;
    std::uint32_t scopes = 1;
    std::uint32_t maximumDepth = 0;

    [[nodiscard]] bool field() noexcept
    {
        return fields < limits::serialized_object_max_fields && ++fields != 0;
    }

    [[nodiscard]] bool scope(std::uint32_t depth) noexcept
    {
        if (scopes >= limits::serialized_object_max_scopes ||
            depth > limits::serialized_object_max_depth)
            return false;
        ++scopes;
        if (depth > maximumDepth)
            maximumDepth = depth;
        return true;
    }
};

class ObjectCursor
{
    xdata::CanonicalWriter &writer_;
    BuildStats &stats_;
    std::uint32_t lastCode_ = 0;

public:
    ObjectCursor(xdata::CanonicalWriter &writer, BuildStats &stats) noexcept
        : writer_(writer), stats_(stats)
    {
    }

    [[nodiscard]] bool begin(BuilderFieldCode field) noexcept
    {
        std::uint32_t const code = wireCode(field);
        if (code <= lastCode_ || !stats_.field() ||
            !writer_.field_header(code))
            return false;
        lastCode_ = code;
        return true;
    }

    [[nodiscard]] bool value(TransactionBuilderFieldSpec const &field,
                             std::span<std::uint8_t const> bytes) noexcept
    {
        return begin(field.code) &&
            (!field.vl_encoded || writer_.vl_prefix(
                static_cast<std::uint32_t>(bytes.size()))) &&
            writer_.bytes(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    }

    [[nodiscard]] bool rawField(BuilderFieldCode field,
                                std::span<std::uint8_t const> bytes) noexcept
    {
        std::uint32_t const code = wireCode(field);
        if (code <= lastCode_ || !stats_.field())
            return false;
        lastCode_ = code;
        return writer_.bytes(
            bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    }
};

JSClassID emittedTransactionClassId = 0;

struct EmittedTransactionState
{
    JSValue blob = JS_UNDEFINED;
    std::uint16_t kind = 0;
};

void emittedTransactionFinalizer(JSRuntime *runtime, JSValue value)
{
    auto *state = static_cast<EmittedTransactionState *>(
        JS_GetOpaque(value, emittedTransactionClassId));
    if (state == nullptr)
        return;
    JS_FreeValueRT(runtime, state->blob);
    js_free_rt(runtime, state);
}

void emittedTransactionMark(JSRuntime *runtime, JSValueConst value,
                            JS_MarkFunc *mark)
{
    auto const *state = static_cast<EmittedTransactionState const *>(
        JS_GetOpaque(value, emittedTransactionClassId));
    if (state != nullptr)
        JS_MarkValue(runtime, state->blob, mark);
}

JSClassDef const emittedTransactionClass{
    .class_name = "EmittedTransaction",
    .finalizer = emittedTransactionFinalizer,
    .gc_mark = emittedTransactionMark,
};

[[nodiscard]] JSValue encodeFailure(JSContext *ctx, char const *field)
{
    qjs::OwnedValue error(ctx, result_error(ctx, "encode"));
    if (error.isException())
        return error.release();
    if (JS_DefinePropertyValueStr(
            ctx, error.get(), "issue", JS_NewString(ctx, "invalid-value"),
            JS_PROP_ENUMERABLE) < 0 ||
        (field != nullptr &&
         JS_DefinePropertyValueStr(
             ctx, error.get(), "field", JS_NewString(ctx, field),
             JS_PROP_ENUMERABLE) < 0) ||
        JS_DefinePropertyValueStr(
            ctx, error.get(), "stage", JS_NewString(ctx, "encode"),
            JS_PROP_ENUMERABLE) < 0 ||
        !result_finish(ctx, error.get()))
        return JS_EXCEPTION;
    return result_failure(ctx, error.release());
}

[[nodiscard]] JSValue buildHostFailure(
    JSContext *ctx, std::int64_t code, char const *stage)
{
    qjs::OwnedValue error(ctx, result_error(ctx, "host"));
    if (error.isException())
        return error.release();
    if (JS_DefinePropertyValueStr(
            ctx, error.get(), "code", JS_NewInt64(ctx, code),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            ctx, error.get(), "stage", JS_NewString(ctx, stage),
            JS_PROP_ENUMERABLE) < 0 ||
        !result_finish(ctx, error.get()))
        return JS_EXCEPTION;
    return result_failure(ctx, error.release());
}

[[nodiscard]] ParseOutcome typeFailure(
    JSContext *ctx, char const *operation, char const *field,
    char const *expected)
{
    JS_ThrowTypeError(ctx, "%s: %s must be %s", operation, field, expected);
    return ParseOutcome::exception();
}

[[nodiscard]] ParseOutcome property(
    JSContext *ctx, JSValueConst object, char const *name,
    qjs::OwnedValue &value, bool &present)
{
    value = qjs::property(ctx, object, name);
    if (value.isException())
        return ParseOutcome::exception();
    present = !JS_IsUndefined(value.get());
    return ParseOutcome::success();
}

[[nodiscard]] ParseOutcome arrayLength(
    JSContext *ctx, JSValueConst value, char const *operation,
    char const *field, std::uint32_t maximum, std::uint32_t &length)
{
    int const array = JS_IsArray(ctx, value);
    if (array < 0)
        return ParseOutcome::exception();
    if (array == 0)
        return typeFailure(ctx, operation, field, "an array");
    qjs::OwnedValue lengthValue(ctx, JS_GetPropertyStr(ctx, value, "length"));
    if (lengthValue.isException())
        return ParseOutcome::exception();
    double numericLength = 0;
    if (!JS_IsNumber(lengthValue.get()) ||
        JS_ToFloat64(ctx, &numericLength, lengthValue.get()) < 0)
        return typeFailure(ctx, operation, field, "a bounded array");
    if (!std::isfinite(numericLength) ||
        std::trunc(numericLength) != numericLength || numericLength < 0 ||
        numericLength > maximum)
        return ParseOutcome::encode(field);
    length = static_cast<std::uint32_t>(numericLength);
    return ParseOutcome::success();
}

template <std::size_t Capacity>
[[nodiscard]] ParseOutcome nominalValue(
    JSContext *ctx, JSValueConst value, xdata::MaterializerKind expected,
    FixedBytes<Capacity> &output, char const *operation, char const *field)
{
    std::uint8_t scratch[8]{};
    types::NominalPayloadView payload{};
    if (!types::readNominalPayload(ctx, value, expected, scratch, payload)) {
        if (JS_HasException(ctx))
            return ParseOutcome::exception();
        return typeFailure(ctx, operation, field, "the declared provider value");
    }
    if (payload.size > Capacity || (payload.size != 0 && payload.data == nullptr)) {
        JS_ThrowInternalError(
            ctx, "%s: invalid nominal payload for %s", operation, field);
        return ParseOutcome::exception();
    }
    if (payload.size != 0)
        std::memcpy(output.bytes.data(), payload.data, payload.size);
    output.size = static_cast<std::uint16_t>(payload.size);
    output.present = true;
    return ParseOutcome::success();
}

template <std::size_t Capacity>
[[nodiscard]] ParseOutcome byteValue(
    JSContext *ctx, JSValueConst value, qjs::BytePolicy policy,
    FixedBytes<Capacity> &output, char const *operation, char const *field)
{
    auto bytes = qjs::ByteView::getBinding(
        ctx, value, "emit.build", 0, policy);
    if (!bytes) {
        if (JS_HasException(ctx))
            return ParseOutcome::exception();
        return typeFailure(ctx, operation, field, "the declared byte value");
    }
    if (bytes.size() > Capacity)
        return ParseOutcome::encode(field);
    if (bytes.size() != 0)
        std::memcpy(output.bytes.data(), bytes.data(), bytes.size());
    output.size = static_cast<std::uint16_t>(bytes.size());
    output.present = true;
    return ParseOutcome::success();
}

[[nodiscard]] bool sameBytes(
    std::span<std::uint8_t const> left,
    std::span<std::uint8_t const> right) noexcept
{
    return left.size() == right.size() &&
        (left.empty() ||
         std::memcmp(left.data(), right.data(), left.size()) == 0);
}

[[nodiscard]] ParseOutcome parseParameters(
    JSContext *ctx, JSValueConst value, HookParameterList &output,
    char const *operation, char const *field)
{
    std::uint32_t length = 0;
    auto status = arrayLength(
        ctx, value, operation, field, maximumParameterCount, length);
    if (status.disposition != ParseDisposition::ok)
        return status;
    output.present = true;
    output.count = static_cast<std::uint8_t>(length);
    for (std::uint32_t index = 0; index < length; ++index) {
        qjs::OwnedValue item(ctx, JS_GetPropertyUint32(ctx, value, index));
        if (item.isException())
            return ParseOutcome::exception();
        if (!JS_IsObject(item.get()) || JS_IsNull(item.get()))
            return ParseOutcome::encode(field);

        qjs::OwnedValue name(ctx);
        bool namePresent = false;
        status = property(
            ctx, item.get(), "HookParameterName", name, namePresent);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (!namePresent)
            return ParseOutcome::encode(field);
        status = byteValue(
            ctx, name.get(), qjs::BytePolicy::stateKeyLike,
            output.values[index].name, operation, field);
        if (status.disposition != ParseDisposition::ok)
            return status;

        qjs::OwnedValue parameterValue(ctx);
        bool valuePresent = false;
        status = property(
            ctx, item.get(), "HookParameterValue", parameterValue,
            valuePresent);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (!valuePresent)
            return ParseOutcome::encode(field);
        status = byteValue(
            ctx, parameterValue.get(), qjs::BytePolicy::stateValueLike,
            output.values[index].value, operation, field);
        if (status.disposition != ParseDisposition::ok)
            return status;

        for (std::uint32_t previous = 0; previous < index; ++previous) {
            if (sameBytes(
                    output.values[previous].name.view(),
                    output.values[index].name.view()))
                return ParseOutcome::encode(field);
        }
    }
    return ParseOutcome::success();
}

[[nodiscard]] ParseOutcome parseGrants(
    JSContext *ctx, JSValueConst value, HookGrantList &output,
    char const *operation, char const *field)
{
    std::uint32_t length = 0;
    auto status = arrayLength(
        ctx, value, operation, field, maximumGrantCount, length);
    if (status.disposition != ParseDisposition::ok)
        return status;
    output.present = true;
    output.count = static_cast<std::uint8_t>(length);
    for (std::uint32_t index = 0; index < length; ++index) {
        qjs::OwnedValue item(ctx, JS_GetPropertyUint32(ctx, value, index));
        if (item.isException())
            return ParseOutcome::exception();
        if (!JS_IsObject(item.get()) || JS_IsNull(item.get()))
            return ParseOutcome::encode(field);

        qjs::OwnedValue hash(ctx);
        bool hashPresent = false;
        status = property(ctx, item.get(), "HookHash", hash, hashPresent);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (!hashPresent)
            return ParseOutcome::encode(field);
        status = nominalValue(
            ctx, hash.get(), xdata::MaterializerKind::hash256,
            output.values[index].hookHash, operation, field);
        if (status.disposition != ParseDisposition::ok)
            return status;

        qjs::OwnedValue authorize(ctx);
        bool authorizePresent = false;
        status = property(
            ctx, item.get(), "Authorize", authorize, authorizePresent);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (!authorizePresent)
            return ParseOutcome::encode(field);
        status = nominalValue(
            ctx, authorize.get(), xdata::MaterializerKind::account_id,
            output.values[index].authorize, operation, field);
        if (status.disposition != ParseDisposition::ok)
            return status;

        for (std::uint32_t previous = 0; previous < index; ++previous) {
            if (sameBytes(
                    output.values[previous].hookHash.view(),
                    output.values[index].hookHash.view()) &&
                sameBytes(
                    output.values[previous].authorize.view(),
                    output.values[index].authorize.view()))
                return ParseOutcome::encode(field);
        }
    }
    return ParseOutcome::success();
}

template <std::size_t Capacity>
[[nodiscard]] ParseOutcome parseLeaf(
    JSContext *ctx, JSValueConst value,
    TransactionBuilderFieldSpec const &field,
    FixedBytes<Capacity> &output, char const *operation)
{
    if (field.materializer == xdata::MaterializerKind::st_array)
        return ParseOutcome::encode(field.option_name);
    return nominalValue(
        ctx, value, field.materializer, output, operation, field.option_name);
}

[[nodiscard]] ParseOutcome parseFlagNumber(
    JSContext *ctx, JSValueConst value, std::uint32_t &output,
    char const *field)
{
    if (!JS_IsNumber(value))
        return ParseOutcome::encode(field);
    double numeric = 0;
    if (JS_ToFloat64(ctx, &numeric, value) < 0)
        return ParseOutcome::exception();
    if (!std::isfinite(numeric) || std::trunc(numeric) != numeric ||
        numeric < 0 ||
        numeric > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
        return ParseOutcome::encode(field);
    output = static_cast<std::uint32_t>(numeric);
    return ParseOutcome::success();
}

[[nodiscard]] ParseOutcome parseFlags(
    JSContext *ctx, JSValueConst value,
    TransactionBuilderFieldSpec const &field, FixedBytes<4> &output,
    char const *operation)
{
    if (JS_IsNumber(value)) {
        std::uint32_t flags = 0;
        auto const status = parseFlagNumber(
            ctx, value, flags, field.option_name);
        if (status.disposition == ParseDisposition::ok)
            setUInt32(output, flags);
        return status;
    }

    int const isArray = JS_IsArray(ctx, value);
    if (isArray < 0)
        return ParseOutcome::exception();
    if (isArray != 0) {
        std::uint32_t length = 0;
        auto status = arrayLength(
            ctx, value, operation, field.option_name, 32, length);
        if (status.disposition != ParseDisposition::ok)
            return status;
        std::uint32_t flags = 0;
        for (std::uint32_t index = 0; index < length; ++index) {
            qjs::OwnedValue item(
                ctx, JS_GetPropertyUint32(ctx, value, index));
            if (item.isException())
                return ParseOutcome::exception();
            std::uint32_t flag = 0;
            status = parseFlagNumber(
                ctx, item.get(), flag, field.option_name);
            if (status.disposition != ParseDisposition::ok)
                return status;
            flags |= flag;
        }
        setUInt32(output, flags);
        return ParseOutcome::success();
    }

    return nominalValue(
        ctx, value, field.materializer, output, operation, field.option_name);
}

[[nodiscard]] ParseOutcome parsePayment(
    JSContext *ctx, JSValueConst options, PaymentSnapshot &output)
{
    constexpr char operation[] = "emit.build.payment";
    if (!JS_IsObject(options) || JS_IsNull(options))
        return typeFailure(ctx, operation, "options", "an object");

    for (auto const &field : generated::Payment_FIELDS) {
        qjs::OwnedValue value(ctx);
        bool present = false;
        auto status = property(
            ctx, options, field.option_name, value, present);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (!present) {
            if (field.required)
                return ParseOutcome::encode(field.option_name);
            continue;
        }

        switch (field.option) {
        case BuilderOption::Flags:
            status = parseFlags(
                ctx, value.get(), field, output.flags, operation);
            break;
        case BuilderOption::SourceTag:
            status = parseLeaf(
                ctx, value.get(), field, output.sourceTag, operation);
            break;
        case BuilderOption::DestinationTag:
            status = parseLeaf(
                ctx, value.get(), field, output.destinationTag, operation);
            break;
        case BuilderOption::InvoiceID:
            status = parseLeaf(
                ctx, value.get(), field, output.invoiceId, operation);
            break;
        case BuilderOption::Amount:
            status = parseLeaf(ctx, value.get(), field, output.amount, operation);
            break;
        case BuilderOption::SendMax:
            status = parseLeaf(ctx, value.get(), field, output.sendMax, operation);
            break;
        case BuilderOption::DeliverMin:
            status = parseLeaf(
                ctx, value.get(), field, output.deliverMin, operation);
            break;
        case BuilderOption::Account:
            status = parseLeaf(ctx, value.get(), field, output.account, operation);
            break;
        case BuilderOption::Destination:
            status = parseLeaf(
                ctx, value.get(), field, output.destination, operation);
            break;
        case BuilderOption::HookParameters:
            status = parseParameters(
                ctx, value.get(), output.hookParameters, operation,
                field.option_name);
            break;
        case BuilderOption::Hooks:
        case BuilderOption::none:
            return ParseOutcome::encode(field.option_name);
        }
        if (status.disposition != ParseDisposition::ok)
            return status;
    }
    setUInt32(
        output.flags,
        (output.flags.present ? readUInt32(output.flags) : 0U) |
            fullyCanonicalSignatureFlag);
    return ParseOutcome::success();
}

[[nodiscard]] ParseOutcome parsePosition(
    JSContext *ctx, JSValueConst value, std::uint32_t &position)
{
    constexpr char operation[] = "emit.build.hookSet";
    if (!JS_IsNumber(value))
        return typeFailure(ctx, operation, "Hooks.$position", "an integer");
    double numeric = 0;
    if (JS_ToFloat64(ctx, &numeric, value) < 0)
        return ParseOutcome::exception();
    if (!std::isfinite(numeric) || std::trunc(numeric) != numeric ||
        numeric < 0 || numeric >= maximumHookCount)
        return ParseOutcome::encode("Hooks.$position");
    position = static_cast<std::uint32_t>(numeric);
    return ParseOutcome::success();
}

[[nodiscard]] ParseOutcome rejectDeletionPayload(
    JSContext *ctx, JSValueConst action, char const *propertyName)
{
    qjs::OwnedValue value(ctx);
    bool present = false;
    auto const status = property(ctx, action, propertyName, value, present);
    if (status.disposition != ParseDisposition::ok)
        return status;
    return present ? ParseOutcome::encode("Hooks") : ParseOutcome::success();
}

[[nodiscard]] ParseOutcome parseHooks(
    JSContext *ctx, JSValueConst value, HookSetSnapshot &output)
{
    constexpr char operation[] = "emit.build.hookSet";
    std::uint32_t length = 0;
    auto status = arrayLength(
        ctx, value, operation, "Hooks", maximumHookCount, length);
    if (status.disposition != ParseDisposition::ok)
        return status;
    if (length == 0)
        return ParseOutcome::encode("Hooks");

    std::array<bool, maximumHookCount> seen{};
    std::uint32_t highest = 0;
    for (std::uint32_t index = 0; index < length; ++index) {
        qjs::OwnedValue action(ctx, JS_GetPropertyUint32(ctx, value, index));
        if (action.isException())
            return ParseOutcome::exception();
        if (!JS_IsObject(action.get()) || JS_IsNull(action.get()))
            return ParseOutcome::encode("Hooks");

        qjs::OwnedValue positionValue(ctx);
        bool positionPresent = false;
        status = property(
            ctx, action.get(), "$position", positionValue, positionPresent);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (!positionPresent)
            return ParseOutcome::encode("Hooks.$position");
        std::uint32_t position = 0;
        status = parsePosition(ctx, positionValue.get(), position);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (seen[position])
            return ParseOutcome::encode("Hooks.$position");
        seen[position] = true;
        if (position > highest)
            highest = position;

        HookActionSnapshot &snapshot = output.hooks[position];
        qjs::OwnedValue deletion(ctx);
        bool deletionPresent = false;
        status = property(
            ctx, action.get(), "$delete", deletion, deletionPresent);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (deletionPresent) {
            if (!JS_IsBool(deletion.get()) || JS_ToBool(ctx, deletion.get()) != 1)
                return ParseOutcome::encode("Hooks.$delete");
            snapshot.kind = HookActionKind::deletion;
            for (char const *propertyName : {
                     "HookHash", "HookNamespace", "HookParameters",
                     "HookGrants"}) {
                status = rejectDeletionPayload(
                    ctx, action.get(), propertyName);
                if (status.disposition != ParseDisposition::ok)
                    return status;
            }
            continue;
        }

        qjs::OwnedValue hash(ctx);
        bool hashPresent = false;
        status = property(ctx, action.get(), "HookHash", hash, hashPresent);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (!hashPresent)
            return ParseOutcome::encode("Hooks.HookHash");

        snapshot.kind = HookActionKind::reference;
        status = nominalValue(
            ctx, hash.get(), xdata::MaterializerKind::hash256,
            snapshot.hookHash, operation, "Hooks.HookHash");
        if (status.disposition != ParseDisposition::ok)
            return status;

        qjs::OwnedValue namespaceValue(ctx);
        bool namespacePresent = false;
        status = property(
            ctx, action.get(), "HookNamespace", namespaceValue,
            namespacePresent);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (namespacePresent) {
            status = nominalValue(
                ctx, namespaceValue.get(), xdata::MaterializerKind::hash256,
                snapshot.hookNamespace, operation, "Hooks.HookNamespace");
            if (status.disposition != ParseDisposition::ok)
                return status;
        }

        qjs::OwnedValue parameters(ctx);
        bool parametersPresent = false;
        status = property(
            ctx, action.get(), "HookParameters", parameters,
            parametersPresent);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (parametersPresent) {
            status = parseParameters(
                ctx, parameters.get(), snapshot.parameters, operation,
                "Hooks.HookParameters");
            if (status.disposition != ParseDisposition::ok)
                return status;
        }

        qjs::OwnedValue grants(ctx);
        bool grantsPresent = false;
        status = property(
            ctx, action.get(), "HookGrants", grants, grantsPresent);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (grantsPresent) {
            status = parseGrants(
                ctx, grants.get(), snapshot.grants, operation,
                "Hooks.HookGrants");
            if (status.disposition != ParseDisposition::ok)
                return status;
        }
    }
    output.hookCount = static_cast<std::uint8_t>(highest + 1);
    return ParseOutcome::success();
}

[[nodiscard]] ParseOutcome parseHookSet(
    JSContext *ctx, JSValueConst options, HookSetSnapshot &output)
{
    constexpr char operation[] = "emit.build.hookSet";
    if (!JS_IsObject(options) || JS_IsNull(options))
        return typeFailure(ctx, operation, "options", "an object");

    for (auto const &field : generated::HookSet_FIELDS) {
        qjs::OwnedValue value(ctx);
        bool present = false;
        auto status = property(
            ctx, options, field.option_name, value, present);
        if (status.disposition != ParseDisposition::ok)
            return status;
        if (!present) {
            if (field.required)
                return ParseOutcome::encode(field.option_name);
            continue;
        }
        switch (field.option) {
        case BuilderOption::Flags:
            status = parseFlags(
                ctx, value.get(), field, output.flags, operation);
            break;
        case BuilderOption::Account:
            status = parseLeaf(ctx, value.get(), field, output.account, operation);
            break;
        case BuilderOption::Hooks:
            status = parseHooks(ctx, value.get(), output);
            break;
        case BuilderOption::HookParameters:
            status = parseParameters(
                ctx, value.get(), output.hookParameters, operation,
                field.option_name);
            break;
        default:
            return ParseOutcome::encode(field.option_name);
        }
        if (status.disposition != ParseDisposition::ok)
            return status;
    }
    return ParseOutcome::success();
}

[[nodiscard]] ParseOutcome resolveCommon(
    JSContext *ctx, FixedBytes<20> const &explicitAccount,
    std::uint16_t transactionType, CommonSnapshot &common)
{
    common.transactionType = transactionType;
    if (explicitAccount.present) {
        std::memcpy(
            common.account.data(), explicitAccount.bytes.data(),
            common.account.size());
    } else {
        std::int64_t const accountResult = hook_hook_account(
            static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(common.account.data())),
            static_cast<std::uint32_t>(common.account.size()));
        if (accountResult != static_cast<std::int64_t>(common.account.size())) {
            JS_ThrowInternalError(
                ctx, "emit.build: hook_account returned %lld, expected 20",
                (long long)accountResult);
            return ParseOutcome::exception();
        }
    }

    std::int64_t const sequence = hook_ledger_seq();
    if (sequence < 0 ||
        sequence > static_cast<std::int64_t>(
            std::numeric_limits<std::uint32_t>::max() - 5U))
        return ParseOutcome::encode("ledger.sequence");
    common.ledgerSequence = static_cast<std::uint32_t>(sequence);
    return ParseOutcome::success();
}

[[nodiscard]] ParseOutcome validateSetHookAccount(
    HookSetSnapshot const &snapshot, CommonSnapshot const &common) noexcept
{
    for (std::uint32_t hookIndex = 0;
         hookIndex < snapshot.hookCount; ++hookIndex) {
        auto const &action = snapshot.hooks[hookIndex];
        if (action.kind != HookActionKind::reference)
            continue;
        for (std::uint32_t grantIndex = 0;
             grantIndex < action.grants.count; ++grantIndex) {
            if (std::memcmp(
                    action.grants.values[grantIndex].authorize.bytes.data(),
                    common.account.data(), common.account.size()) == 0)
                return ParseOutcome::encode("Hooks.HookGrants");
        }
    }
    return ParseOutcome::success();
}

[[nodiscard]] bool validateDetails(
    std::span<std::uint8_t const> details) noexcept
{
    if (details.size() != 116 && details.size() != 138)
        return false;
    std::uint8_t header[3]{};
    xdata::CanonicalWriter writer(header, sizeof(header));
    if (!writer.field_header(wireCode(BuilderFieldCode::EmitDetails)))
        return false;
    std::uint32_t const headerSize = writer.position();
    return details.size() > headerSize &&
        std::memcmp(details.data(), header, headerSize) == 0 &&
        details.back() == objectEndMarker;
}

[[nodiscard]] bool acquireDetails(
    JSContext *ctx, CommonSnapshot &common, JSValue &failure)
{
    std::int64_t const result = hook_etxn_details(
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(common.details.data())),
        static_cast<std::uint32_t>(common.details.size()));
    if (result < 0) {
        failure = buildHostFailure(ctx, result, "details");
        return false;
    }
    if ((result != 116 && result != 138) ||
        !validateDetails({
            common.details.data(), static_cast<std::size_t>(result)})) {
        failure = JS_ThrowInternalError(
            ctx, "emit.build: etxn_details returned invalid field length %lld",
            (long long)result);
        return false;
    }
    common.detailsSize = static_cast<std::uint32_t>(result);
    return true;
}

[[nodiscard]] bool emitParameters(
    xdata::CanonicalWriter &writer, BuildStats &stats,
    HookParameterList const &parameters, std::uint32_t depth) noexcept
{
    if (!stats.scope(depth))
        return false;
    auto const &nameSpec = generated::HookParameter_FIELDS[0];
    auto const &valueSpec = generated::HookParameter_FIELDS[1];
    for (std::uint32_t index = 0; index < parameters.count; ++index) {
        if (!stats.field() ||
            !writer.field_header(wireCode(BuilderFieldCode::HookParameter)) ||
            !stats.scope(depth + 1))
            return false;
        ObjectCursor object(writer, stats);
        if (!object.value(nameSpec, parameters.values[index].name.view()) ||
            !object.value(valueSpec, parameters.values[index].value.view()) ||
            !writer.byte(objectEndMarker))
            return false;
    }
    return writer.byte(arrayEndMarker);
}

[[nodiscard]] bool emitGrants(
    xdata::CanonicalWriter &writer, BuildStats &stats,
    HookGrantList const &grants, std::uint32_t depth) noexcept
{
    if (!stats.scope(depth))
        return false;
    auto const &hashSpec = generated::HookGrant_FIELDS[0];
    auto const &authorizeSpec = generated::HookGrant_FIELDS[1];
    for (std::uint32_t index = 0; index < grants.count; ++index) {
        if (!stats.field() ||
            !writer.field_header(wireCode(BuilderFieldCode::HookGrant)) ||
            !stats.scope(depth + 1))
            return false;
        ObjectCursor object(writer, stats);
        if (!object.value(hashSpec, grants.values[index].hookHash.view()) ||
            !object.value(
                authorizeSpec, grants.values[index].authorize.view()) ||
            !writer.byte(objectEndMarker))
            return false;
    }
    return writer.byte(arrayEndMarker);
}

[[nodiscard]] bool emitHooks(
    xdata::CanonicalWriter &writer, BuildStats &stats,
    HookSetSnapshot const &snapshot, std::uint32_t depth) noexcept
{
    if (!stats.scope(depth))
        return false;
    for (std::uint32_t index = 0; index < snapshot.hookCount; ++index) {
        if (!stats.field() ||
            !writer.field_header(wireCode(BuilderFieldCode::Hook)) ||
            !stats.scope(depth + 1))
            return false;
        auto const &action = snapshot.hooks[index];
        ObjectCursor object(writer, stats);
        if (action.kind != HookActionKind::blank) {
            if (!object.begin(BuilderFieldCode::Flags) || !writer.be32(1))
                return false;
        }
        if (action.kind == HookActionKind::reference) {
            if (!object.begin(BuilderFieldCode::HookHash) ||
                !writer.bytes(action.hookHash.bytes.data(), 32))
                return false;
            if (action.hookNamespace.present &&
                (!object.begin(BuilderFieldCode::HookNamespace) ||
                 !writer.bytes(action.hookNamespace.bytes.data(), 32)))
                return false;
            if (action.parameters.present &&
                (!object.begin(BuilderFieldCode::HookParameters) ||
                 !emitParameters(
                     writer, stats, action.parameters, depth + 2)))
                return false;
            if (action.grants.present &&
                (!object.begin(BuilderFieldCode::HookGrants) ||
                 !emitGrants(writer, stats, action.grants, depth + 2)))
                return false;
        } else if (action.kind == HookActionKind::deletion) {
            if (!object.begin(BuilderFieldCode::CreateCode) ||
                !writer.vl_prefix(0))
                return false;
        }
        if (!writer.byte(objectEndMarker))
            return false;
    }
    return writer.byte(arrayEndMarker);
}

[[nodiscard]] bool paymentFieldPresent(
    PaymentSnapshot const &snapshot, BuilderOption option) noexcept
{
    switch (option) {
    case BuilderOption::Flags: return snapshot.flags.present;
    case BuilderOption::SourceTag: return snapshot.sourceTag.present;
    case BuilderOption::DestinationTag: return snapshot.destinationTag.present;
    case BuilderOption::InvoiceID: return snapshot.invoiceId.present;
    case BuilderOption::Amount: return snapshot.amount.present;
    case BuilderOption::SendMax: return snapshot.sendMax.present;
    case BuilderOption::DeliverMin: return snapshot.deliverMin.present;
    case BuilderOption::Account: return snapshot.account.present;
    case BuilderOption::Destination: return snapshot.destination.present;
    case BuilderOption::HookParameters: return snapshot.hookParameters.present;
    default: return false;
    }
}

[[nodiscard]] bool emitPaymentField(
    xdata::CanonicalWriter &writer, BuildStats &stats, ObjectCursor &object,
    TransactionBuilderFieldSpec const &field,
    PaymentSnapshot const &snapshot) noexcept
{
    switch (field.option) {
    case BuilderOption::Flags: return object.value(field, snapshot.flags.view());
    case BuilderOption::SourceTag:
        return object.value(field, snapshot.sourceTag.view());
    case BuilderOption::DestinationTag:
        return object.value(field, snapshot.destinationTag.view());
    case BuilderOption::InvoiceID:
        return object.value(field, snapshot.invoiceId.view());
    case BuilderOption::Amount: return object.value(field, snapshot.amount.view());
    case BuilderOption::SendMax: return object.value(field, snapshot.sendMax.view());
    case BuilderOption::DeliverMin:
        return object.value(field, snapshot.deliverMin.view());
    case BuilderOption::Account: return object.value(field, snapshot.account.view());
    case BuilderOption::Destination:
        return object.value(field, snapshot.destination.view());
    case BuilderOption::HookParameters:
        return object.begin(field.code) &&
            emitParameters(writer, stats, snapshot.hookParameters, 1);
    default: return false;
    }
}

[[nodiscard]] bool hookSetFieldPresent(
    HookSetSnapshot const &snapshot, BuilderOption option) noexcept
{
    switch (option) {
    case BuilderOption::Flags: return snapshot.flags.present;
    case BuilderOption::Account: return snapshot.account.present;
    case BuilderOption::Hooks: return snapshot.hookCount != 0;
    case BuilderOption::HookParameters: return snapshot.hookParameters.present;
    default: return false;
    }
}

[[nodiscard]] bool emitHookSetField(
    xdata::CanonicalWriter &writer, BuildStats &stats, ObjectCursor &object,
    TransactionBuilderFieldSpec const &field,
    HookSetSnapshot const &snapshot) noexcept
{
    switch (field.option) {
    case BuilderOption::Flags: return object.value(field, snapshot.flags.view());
    case BuilderOption::Account: return object.value(field, snapshot.account.view());
    case BuilderOption::Hooks:
        return object.begin(field.code) && emitHooks(writer, stats, snapshot, 1);
    case BuilderOption::HookParameters:
        return object.begin(field.code) &&
            emitParameters(writer, stats, snapshot.hookParameters, 1);
    default: return false;
    }
}

[[nodiscard]] bool emitCommonField(
    xdata::CanonicalWriter &writer, BuildStats &stats, ObjectCursor &object,
    TransactionBuilderFieldSpec const &field, CommonSnapshot const &common,
    std::uint32_t *feeOffset) noexcept
{
    switch (field.code) {
    case BuilderFieldCode::TransactionType:
        return object.begin(field.code) &&
            writer.byte(static_cast<std::uint8_t>(common.transactionType >> 8)) &&
            writer.byte(static_cast<std::uint8_t>(common.transactionType));
    case BuilderFieldCode::Sequence:
        return object.begin(field.code) && writer.be32(0);
    case BuilderFieldCode::FirstLedgerSequence:
        return object.begin(field.code) &&
            writer.be32(common.ledgerSequence + 1);
    case BuilderFieldCode::LastLedgerSequence:
        return object.begin(field.code) &&
            writer.be32(common.ledgerSequence + 5);
    case BuilderFieldCode::Fee:
        if (!object.begin(field.code))
            return false;
        if (feeOffset != nullptr)
            *feeOffset = writer.position();
        return writer.be64(nativeAmountBit);
    case BuilderFieldCode::SigningPubKey:
        // Match xahaud HookAPI::prepare: emitted transactions carry its
        // 33-byte all-zero public-key convention. HookAPI::emit also
        // accepts an empty VL, but that spelling changes the transaction ID.
        if (!object.begin(field.code) || !writer.vl_prefix(33))
            return false;
        return writer.bytes(
            zeroSigningPublicKey.data(), zeroSigningPublicKey.size());
    case BuilderFieldCode::Account:
        return object.begin(field.code) &&
            writer.vl_prefix(
                static_cast<std::uint32_t>(common.account.size())) &&
            writer.bytes(
                common.account.data(),
                static_cast<std::uint32_t>(common.account.size()));
    case BuilderFieldCode::EmitDetails:
        return stats.scope(1) && object.rawField(
            field.code, {common.details.data(), common.detailsSize});
    default:
        return false;
    }
}

template <class Snapshot, std::size_t SpecificCount,
          class Presence, class Emitter>
[[nodiscard]] bool emitTransaction(
    xdata::CanonicalWriter &writer, BuildStats &stats,
    CommonSnapshot const &common, Snapshot const &snapshot,
    TransactionBuilderFieldSpec const (&specific)[SpecificCount],
    Presence present, Emitter emitSpecific,
    std::uint32_t *feeOffset) noexcept
{
    ObjectCursor object(writer, stats);
    std::size_t commonIndex = 0;
    std::size_t specificIndex = 0;
    while (commonIndex < std::size(generated::OWNED_COMMON_FIELDS) ||
           specificIndex < SpecificCount) {
        if (specificIndex < SpecificCount &&
            !present(snapshot, specific[specificIndex].option)) {
            ++specificIndex;
            continue;
        }

        bool const haveCommon =
            commonIndex < std::size(generated::OWNED_COMMON_FIELDS);
        bool const haveSpecific = specificIndex < SpecificCount;
        std::uint32_t const commonCode = haveCommon
            ? wireCode(generated::OWNED_COMMON_FIELDS[commonIndex].code)
            : std::numeric_limits<std::uint32_t>::max();
        std::uint32_t const specificCode = haveSpecific
            ? wireCode(specific[specificIndex].code)
            : std::numeric_limits<std::uint32_t>::max();

        if (commonCode <= specificCode) {
            if (!emitCommonField(
                    writer, stats, object,
                    generated::OWNED_COMMON_FIELDS[commonIndex], common,
                    feeOffset))
                return false;
            ++commonIndex;
            if (commonCode == specificCode)
                ++specificIndex;
        } else {
            if (!emitSpecific(
                    writer, stats, object, specific[specificIndex], snapshot))
                return false;
            ++specificIndex;
        }
    }
    return true;
}

[[nodiscard]] JSValue makeEmittedTransaction(
    JSContext *ctx, JSValue blob, std::uint16_t kind)
{
    qjs::OwnedValue ownedBlob(ctx, blob);
    auto *state = static_cast<EmittedTransactionState *>(
        js_mallocz(ctx, sizeof(EmittedTransactionState)));
    if (state == nullptr)
        return JS_ThrowOutOfMemory(ctx);
    state->blob = JS_DupValue(ctx, ownedBlob.get());
    state->kind = kind;

    qjs::OwnedValue value(
        ctx, JS_NewObjectClass(ctx, emittedTransactionClassId));
    if (value.isException()) {
        JS_FreeValue(ctx, state->blob);
        js_free(ctx, state);
        return value.release();
    }
    JS_SetOpaque(value.get(), state);
    if (JS_DefinePropertyValueStr(
            ctx, value.get(), "blob", JS_DupValue(ctx, ownedBlob.get()),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            ctx, value.get(), "kind", JS_NewUint32(ctx, kind),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_PreventExtensions(ctx, value.get()) < 0)
        return JS_EXCEPTION;
    return value.release();
}

template <class Snapshot, std::size_t SpecificCount,
          class Presence, class Emitter>
[[nodiscard]] JSValue finishBuild(
    JSContext *ctx, Snapshot const &snapshot, CommonSnapshot &common,
    TransactionBuilderFieldSpec const (&specific)[SpecificCount],
    Presence present, Emitter emitter)
{
    JSValue failure = JS_UNDEFINED;
    if (!acquireDetails(ctx, common, failure))
        return failure;

    xdata::CanonicalWriter measuring = xdata::CanonicalWriter::measuring();
    BuildStats measuredStats{};
    if (!emitTransaction(
            measuring, measuredStats, common, snapshot, specific,
            present, emitter, nullptr) ||
        measuring.position() > limits::serialized_object_max_bytes)
        return JS_ThrowInternalError(
            ctx, "emit.build: canonical transaction measurement failed");
    std::uint32_t const size = measuring.position();

    std::uint8_t *output = nullptr;
    qjs::OwnedValue blob(
        ctx, types::makeSTBlobUninitialized(ctx, size, &output));
    if (blob.isException())
        return blob.release();

    xdata::CanonicalWriter writing(output, size);
    BuildStats writtenStats{};
    std::uint32_t feeOffset = std::numeric_limits<std::uint32_t>::max();
    if (!emitTransaction(
            writing, writtenStats, common, snapshot, specific,
            present, emitter, &feeOffset) ||
        writing.position() != size ||
        measuredStats.fields != writtenStats.fields ||
        measuredStats.scopes != writtenStats.scopes ||
        measuredStats.maximumDepth != writtenStats.maximumDepth ||
        feeOffset > size || size - feeOffset < 8)
        return JS_ThrowInternalError(
            ctx, "emit.build: canonical count/write invariant failed");

    std::int64_t const fee = hook_etxn_fee_base(
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(output)),
        size);
    if (fee < 0)
        return buildHostFailure(ctx, fee, "fee");
    if (static_cast<std::uint64_t>(fee) > maximumNativeDrops)
        return JS_ThrowInternalError(
            ctx, "emit.build: etxn_fee_base returned unencodable fee %lld",
            (long long)fee);
    std::uint64_t nativeFee = nativeAmountBit | static_cast<std::uint64_t>(fee);
    for (std::uint32_t index = 0; index < 8; ++index)
        output[feeOffset + index] = static_cast<std::uint8_t>(
            nativeFee >> (56 - index * 8));

    return result_success(
        ctx, makeEmittedTransaction(
            ctx, blob.release(), common.transactionType));
}

// @binding provider:emit.build.payment
[[nodiscard]] JSValue jsEmitBuildPayment(
    JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "emit.build.payment: expected options");
    ContextAllocation<PaymentSnapshot> snapshot(ctx);
    if (!snapshot)
        return JS_ThrowOutOfMemory(ctx);
    ParseOutcome status = parsePayment(ctx, argv[0], *snapshot);
    if (status.disposition == ParseDisposition::exception)
        return JS_EXCEPTION;
    if (status.disposition == ParseDisposition::encodeFailure)
        return encodeFailure(ctx, status.field);

    CommonSnapshot common{};
    status = resolveCommon(
        ctx, snapshot->account, generated::Payment_TRANSACTION_TYPE, common);
    if (status.disposition == ParseDisposition::exception)
        return JS_EXCEPTION;
    if (status.disposition == ParseDisposition::encodeFailure)
        return encodeFailure(ctx, status.field);
    return finishBuild(
        ctx, *snapshot, common, generated::Payment_FIELDS,
        paymentFieldPresent, emitPaymentField);
}

// @binding provider:emit.build.hookSet
[[nodiscard]] JSValue jsEmitBuildHookSet(
    JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "emit.build.hookSet: expected options");
    ContextAllocation<HookSetSnapshot> snapshot(ctx);
    if (!snapshot)
        return JS_ThrowOutOfMemory(ctx);
    ParseOutcome status = parseHookSet(ctx, argv[0], *snapshot);
    if (status.disposition == ParseDisposition::exception)
        return JS_EXCEPTION;
    if (status.disposition == ParseDisposition::encodeFailure)
        return encodeFailure(ctx, status.field);

    CommonSnapshot common{};
    status = resolveCommon(
        ctx, snapshot->account, generated::HookSet_TRANSACTION_TYPE, common);
    if (status.disposition == ParseDisposition::exception)
        return JS_EXCEPTION;
    if (status.disposition == ParseDisposition::encodeFailure)
        return encodeFailure(ctx, status.field);
    status = validateSetHookAccount(*snapshot, common);
    if (status.disposition == ParseDisposition::encodeFailure)
        return encodeFailure(ctx, status.field);
    return finishBuild(
        ctx, *snapshot, common, generated::HookSet_FIELDS,
        hookSetFieldPresent, emitHookSetField);
}

JSValue
// @binding provider:emit.prepare
js_emit_prepare(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "emit.prepare: expected transaction bytes");
    auto transaction = qjs::ByteView::getBinding(
        ctx, argv[0], "emit.prepare", 0, qjs::BytePolicy::bytesLikeOrSTBlob);
    if (!transaction)
        return qjs::pendingOrTypeError(
            ctx, "emit.prepare: invalid transaction bytes");

    static const uint32_t max_transaction_bytes = 1024U * 1024U;
    static const uint32_t preparation_allowance = 4U * 1024U;
    uint64_t requested_capacity =
        (uint64_t)transaction.size() + preparation_allowance;
    uint32_t output_capacity = requested_capacity < max_transaction_bytes
        ? (uint32_t)requested_capacity
        : max_transaction_bytes;
    uint8_t *output = (uint8_t *)js_malloc(ctx, output_capacity);
    if (!output)
        return JS_EXCEPTION;
    int64_t result = hook_prepare(
        (uint32_t)(uintptr_t)output, output_capacity,
        (uint32_t)(uintptr_t)transaction.data(), transaction.size());
    if (result < 0) {
        js_free(ctx, output);
        return host_failure(ctx, result);
    }
    if ((uint64_t)result > output_capacity) {
        js_free(ctx, output);
        return JS_ThrowInternalError(
            ctx, "emit.prepare: host returned oversized length %lld",
            (long long)result);
    }
    JSValue value = makeSTBlob(ctx, output, (uint32_t)result);
    js_free(ctx, output);
    return host_success(ctx, value);
}

JSValue
// @binding provider:emit.reserve
js_emit_reserve(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    uint32_t count;
    if (argc < 1 || JS_ToUint32(ctx, &count, argv[0]))
        return JS_ThrowTypeError(ctx, "emit.reserve: expected count");
    int64_t result = hook_etxn_reserve(count);
    return result < 0
        ? host_effect_failure(ctx, result)
        : host_effect_success(ctx);
}

[[nodiscard]] bool emittedTransactionBytes(
    JSContext *ctx, JSValueConst value,
    types::NominalPayloadView &payload) noexcept
{
    auto const *state = static_cast<EmittedTransactionState const *>(
        JS_GetOpaque(value, emittedTransactionClassId));
    if (state == nullptr)
        return false;
    std::uint8_t scratch[8]{};
    return types::readNominalPayload(
        ctx, state->blob, xdata::MaterializerKind::blob, scratch, payload);
}

JSValue
// @binding provider:emit.tx
js_emit_tx(JSContext *ctx, JSValueConst this_val,
           int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "emit.tx: expected transaction bytes");

    std::uint8_t const *data = nullptr;
    std::uint32_t size = 0;
    types::NominalPayloadView emitted{};
    qjs::ByteView transaction = qjs::ByteView::get(
        ctx, JS_UNDEFINED, qjs::BytePolicy::bytesLikeOrSTBlob);
    if (emittedTransactionBytes(ctx, argv[0], emitted)) {
        data = emitted.data;
        size = emitted.size;
    } else {
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        transaction = qjs::ByteView::getBinding(
            ctx, argv[0], "emit.tx", 0,
            qjs::BytePolicy::bytesLikeOrSTBlob);
        if (!transaction)
            return qjs::pendingOrTypeError(
                ctx, "emit.tx: invalid transaction bytes");
        data = transaction.data();
        size = transaction.size();
    }

    uint8_t hash[32];
    int64_t result = hook_emit(
        (uint32_t)(uintptr_t)hash, sizeof(hash),
        (uint32_t)(uintptr_t)data, size);
    if (result < 0)
        return host_failure(ctx, result);
    if (result != (int64_t)sizeof(hash))
        return JS_ThrowInternalError(
            ctx, "emit.tx: host returned length %lld", (long long)result);
    return host_success(ctx, makeHash256(ctx, hash, sizeof(hash)));
}

}  // namespace

bool registerEmission(JSContext *ctx, JSValue global)
{
    if (!::jshookz::qjs::defineClass(
            JS_GetRuntime(ctx), &emittedTransactionClassId,
            &emittedTransactionClass) ||
        !::jshookz::qjs::installPrototype(ctx, emittedTransactionClassId, {}))
        return false;

    // @binding provider:emit.build
    qjs::OwnedValue build(ctx, JS_NewObject(ctx));
    if (build.isException() ||
        JS_SetPropertyStr(
            ctx, build.get(), "payment",
            JS_NewCFunction(ctx, jsEmitBuildPayment, "payment", 1)) < 0 ||
        JS_SetPropertyStr(
            ctx, build.get(), "hookSet",
            JS_NewCFunction(ctx, jsEmitBuildHookSet, "hookSet", 1)) < 0 ||
        !qjs::freezeObject(ctx, build.get()))
        return false;

    qjs::OwnedValue emit(ctx, JS_NewObject(ctx));
    if (emit.isException())
        return false;
    if (JS_SetPropertyStr(ctx, emit.get(), "build", build.release()) < 0 ||
        JS_SetPropertyStr(ctx, emit.get(), "reserve",
            JS_NewCFunction(ctx, js_emit_reserve, "reserve", 1)) < 0 ||
        JS_SetPropertyStr(ctx, emit.get(), "prepare",
            JS_NewCFunction(ctx, js_emit_prepare, "prepare", 1)) < 0 ||
        JS_SetPropertyStr(ctx, emit.get(), "tx",
            JS_NewCFunction(ctx, js_emit_tx, "tx", 1)) < 0)
        return false;
    return JS_SetPropertyStr(ctx, global, "emit", emit.release()) >= 0;
}

}  // namespace jshookz::provider::bindings
