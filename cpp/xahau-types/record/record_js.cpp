#include "record/record_js.hpp"

#include "js.hpp"
#include "object/nominal_payload.hpp"
#include "result.hpp"

#include <catl/xdata/static_protocol.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace jshookz::provider::types {
namespace {

namespace bindings = jshookz::provider::bindings;
namespace qjs = jshookz::provider::qjs;
namespace xdata = catl::xdata;

constexpr std::uint32_t maximumRecordBytes = 1024U * 1024U;
constexpr std::uint32_t maximumRecordFields = 256U;

enum class FieldKind : std::uint8_t
{
    u8,
    u16le,
    u32le,
    u64le,
    xflle,
    bytes,
    hash256,
    accountID,
    padding,
};

struct FieldState
{
    FieldKind kind;
    std::uint32_t width;
};

struct FieldSpec
{
    JSAtom name = JS_ATOM_NULL;
    std::uint32_t offset = 0;
    std::uint32_t width = 0;
    FieldKind kind = FieldKind::padding;
};

struct SchemaState
{
    JSAtom name = JS_ATOM_NULL;
    std::uint32_t byteLength = 0;
    std::uint16_t fieldCount = 0;
    bool scalar = false;
};

JSClassID fieldClassId;
JSClassID scalarSchemaClassId;
JSClassID recordSchemaClassId;

[[nodiscard]] FieldSpec*
schemaFields(SchemaState* state) noexcept
{
    return reinterpret_cast<FieldSpec*>(state + 1);
}

[[nodiscard]] FieldSpec const*
schemaFields(SchemaState const* state) noexcept
{
    return reinterpret_cast<FieldSpec const*>(state + 1);
}

void
fieldFinalizer(JSRuntime* runtime, JSValue value)
{
    auto* state = static_cast<FieldState*>(
        JS_GetOpaque(value, fieldClassId));
    if (state != nullptr)
        js_free_rt(runtime, state);
}

void
freeSchemaState(JSRuntime* runtime, SchemaState* state) noexcept
{
    if (state == nullptr)
        return;
    if (state->name != JS_ATOM_NULL)
        JS_FreeAtomRT(runtime, state->name);
    auto* fields = schemaFields(state);
    for (std::uint16_t index = 0; index < state->fieldCount; ++index) {
        if (fields[index].name != JS_ATOM_NULL)
            JS_FreeAtomRT(runtime, fields[index].name);
    }
    js_free_rt(runtime, state);
}

void
schemaFinalizer(JSRuntime* runtime, JSValue value)
{
    JSClassID const classId = JS_GetClassID(value);
    if (classId != scalarSchemaClassId && classId != recordSchemaClassId)
        return;
    freeSchemaState(
        runtime,
        static_cast<SchemaState*>(JS_GetOpaque(value, classId)));
}

JSClassDef const fieldClass = {
    .class_name = "RecordField",
    .finalizer = fieldFinalizer,
};

JSClassDef const scalarSchemaClass = {
    .class_name = "ScalarSchema",
    .finalizer = schemaFinalizer,
};

JSClassDef const recordSchemaClass = {
    .class_name = "RecordSchema",
    .finalizer = schemaFinalizer,
};

[[nodiscard]] FieldState const*
fieldState(JSValueConst value) noexcept
{
    if (!JS_IsObject(value) || JS_GetClassID(value) != fieldClassId)
        return nullptr;
    auto const* state = static_cast<FieldState const*>(
        JS_GetOpaque(value, fieldClassId));
    if (state == nullptr || state->width == 0 ||
        state->width > maximumRecordBytes)
        return nullptr;
    return state;
}

[[nodiscard]] SchemaState*
schemaState(JSContext* context, JSValueConst value, bool recordOnly = false)
{
    if (!JS_IsObject(value)) {
        JS_ThrowTypeError(context, "record schema: invalid receiver");
        return nullptr;
    }
    JSClassID const classId = JS_GetClassID(value);
    if (classId != recordSchemaClassId &&
        (recordOnly || classId != scalarSchemaClassId)) {
        JS_ThrowTypeError(context, "record schema: invalid receiver");
        return nullptr;
    }
    auto* state = static_cast<SchemaState*>(JS_GetOpaque(value, classId));
    if (state == nullptr || state->byteLength > maximumRecordBytes ||
        state->fieldCount > maximumRecordFields ||
        state->scalar != (classId == scalarSchemaClassId)) {
        JS_ThrowTypeError(context, "record schema: invalid receiver");
        return nullptr;
    }
    return state;
}

[[nodiscard]] JSValue
newField(JSContext* context, FieldKind kind, std::uint32_t width)
{
    if (width == 0 || width > maximumRecordBytes)
        return JS_ThrowRangeError(
            context, "record field width must be between 1 and %u",
            maximumRecordBytes);
    auto* state = static_cast<FieldState*>(
        js_mallocz(context, sizeof(FieldState)));
    if (state == nullptr)
        return JS_ThrowOutOfMemory(context);
    *state = {kind, width};
    JSValue value = JS_NewObjectClass(context, fieldClassId);
    if (JS_IsException(value)) {
        js_free(context, state);
        return value;
    }
    JS_SetOpaque(value, state);
    if (!qjs::freezeObject(context, value)) {
        JS_FreeValue(context, value);
        return JS_EXCEPTION;
    }
    return value;
}

[[nodiscard]] bool
finishError(JSContext* context, JSValueConst error)
{
    return bindings::result_finish(context, error);
}

[[nodiscard]] bool
defineString(
    JSContext* context,
    JSValueConst object,
    char const* name,
    char const* value)
{
    return JS_DefinePropertyValueStr(
               context,
               object,
               name,
               JS_NewString(context, value),
               JS_PROP_ENUMERABLE) >= 0;
}

[[nodiscard]] JSValue
wrongLengthFailure(
    JSContext* context,
    std::uint32_t expected,
    std::uint32_t actual)
{
    qjs::OwnedValue error(
        context, bindings::result_error(context, "parse"));
    if (error.isException())
        return error.release();
    if (!defineString(context, error.get(), "issue", "wrong-length") ||
        JS_DefinePropertyValueStr(
            context,
            error.get(),
            "expectedLength",
            JS_NewUint32(context, expected),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            context,
            error.get(),
            "actualLength",
            JS_NewUint32(context, actual),
            JS_PROP_ENUMERABLE) < 0 ||
        !finishError(context, error.get()))
        return JS_EXCEPTION;
    return bindings::result_failure(context, error.release());
}

[[nodiscard]] JSValue
encodeFailure(JSContext* context, JSAtom field)
{
    qjs::OwnedValue error(
        context, bindings::result_error(context, "encode"));
    if (error.isException())
        return error.release();
    if (!defineString(context, error.get(), "issue", "invalid-value"))
        return JS_EXCEPTION;
    if (field != JS_ATOM_NULL &&
        JS_DefinePropertyValueStr(
            context,
            error.get(),
            "field",
            JS_AtomToString(context, field),
            JS_PROP_ENUMERABLE) < 0)
        return JS_EXCEPTION;
    if (!finishError(context, error.get()))
        return JS_EXCEPTION;
    return bindings::result_failure(context, error.release());
}

[[nodiscard]] JSValue
parseValueFailure(JSContext* context, JSAtom field)
{
    qjs::OwnedValue error(
        context, bindings::result_error(context, "parse"));
    if (error.isException())
        return error.release();
    if (!defineString(
            context,
            error.get(),
            "issue",
            field == JS_ATOM_NULL ? "invalid-value" : "invalid-field"))
        return JS_EXCEPTION;
    if (field != JS_ATOM_NULL &&
        JS_DefinePropertyValueStr(
            context,
            error.get(),
            "field",
            JS_AtomToString(context, field),
            JS_PROP_ENUMERABLE) < 0)
        return JS_EXCEPTION;
    if (!finishError(context, error.get()))
        return JS_EXCEPTION;
    return bindings::result_failure(context, error.release());
}

[[nodiscard]] JSValue
throwInvalidParse(JSContext* context, JSAtom field)
{
    if (field == JS_ATOM_NULL)
        return JS_ThrowTypeError(context, "record schema value is invalid");
    qjs::OwnedValue name(context, JS_AtomToString(context, field));
    if (name.isException())
        return name.release();
    char const* text = JS_ToCString(context, name.get());
    if (text == nullptr)
        return JS_EXCEPTION;
    JSValue error = JS_ThrowTypeError(
        context, "record field '%s' is invalid", text);
    JS_FreeCString(context, text);
    return error;
}

enum class ValueStatus : std::uint8_t
{
    valid,
    invalid,
    exception,
};

[[nodiscard]] ValueStatus
readNumber(
    JSContext* context,
    JSValueConst value,
    std::uint64_t maximum,
    std::uint64_t& output)
{
    output = 0;
    if (!JS_IsNumber(value))
        return ValueStatus::invalid;
    double number = 0;
    if (JS_ToFloat64(context, &number, value) < 0)
        return ValueStatus::exception;
    if (!std::isfinite(number) || std::trunc(number) != number || number < 0 ||
        number > static_cast<double>(maximum))
        return ValueStatus::invalid;
    output = static_cast<std::uint64_t>(number);
    return ValueStatus::valid;
}

[[nodiscard]] ValueStatus
readBigUint64(
    JSContext* context,
    JSValueConst value,
    std::uint64_t& output)
{
    output = 0;
    if (!JS_IsBigInt(context, value))
        return ValueStatus::invalid;
    qjs::OwnedValue rendered(context, JS_ToString(context, value));
    if (rendered.isException())
        return ValueStatus::exception;
    std::size_t length = 0;
    char const* text = JS_ToCStringLen(context, &length, rendered.get());
    if (text == nullptr)
        return ValueStatus::exception;
    bool valid = length != 0;
    std::uint64_t parsed = 0;
    for (std::size_t index = 0; valid && index < length; ++index) {
        char const digit = text[index];
        valid = digit >= '0' && digit <= '9';
        if (!valid)
            break;
        std::uint64_t const part =
            static_cast<std::uint64_t>(digit - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - part) / 10) {
            valid = false;
            break;
        }
        parsed = parsed * 10 + part;
    }
    JS_FreeCString(context, text);
    if (!valid)
        return ValueStatus::invalid;
    output = parsed;
    return ValueStatus::valid;
}

[[nodiscard]] JSValue
decodeField(
    JSContext* context,
    FieldSpec const& field,
    std::uint8_t const* bytes,
    bool& invalid)
{
    invalid = false;
    auto const* source = bytes + field.offset;
    switch (field.kind) {
        case FieldKind::u8:
            return JS_NewUint32(context, source[0]);
        case FieldKind::u16le:
            return JS_NewUint32(
                context,
                static_cast<std::uint32_t>(source[0]) |
                    (static_cast<std::uint32_t>(source[1]) << 8));
        case FieldKind::u32le:
            return JS_NewUint32(
                context,
                static_cast<std::uint32_t>(source[0]) |
                    (static_cast<std::uint32_t>(source[1]) << 8) |
                    (static_cast<std::uint32_t>(source[2]) << 16) |
                    (static_cast<std::uint32_t>(source[3]) << 24));
        case FieldKind::u64le: {
            std::uint64_t value = 0;
            for (std::uint32_t index = 0; index < 8; ++index)
                value |= static_cast<std::uint64_t>(source[index]) <<
                    (index * 8);
            return JS_NewBigUint64(context, value);
        }
        case FieldKind::xflle: {
            std::uint64_t word = 0;
            for (std::uint32_t index = 0; index < 8; ++index)
                word |= static_cast<std::uint64_t>(source[index]) <<
                    (index * 8);
            std::int64_t const raw = static_cast<std::int64_t>(word);
            if (!isCanonicalXFLRaw(raw)) {
                invalid = true;
                return JS_UNDEFINED;
            }
            return makeXFLDecimalRaw(context, raw);
        }
        case FieldKind::bytes:
            return makeSTBlobBytes(context, source, field.width);
        case FieldKind::hash256:
            return makeHash256Bytes(context, source, field.width);
        case FieldKind::accountID:
            return makeAccountIDBytes(context, source, field.width);
        case FieldKind::padding:
            return JS_UNDEFINED;
    }
    return JS_ThrowInternalError(context, "unknown record field kind");
}

[[nodiscard]] ValueStatus
encodeField(
    JSContext* context,
    FieldSpec const& field,
    JSValueConst value,
    std::uint8_t* output)
{
    std::uint64_t integer = 0;
    switch (field.kind) {
        case FieldKind::u8: {
            auto const status = readNumber(context, value, 0xffU, integer);
            if (status != ValueStatus::valid)
                return status;
            output[0] = static_cast<std::uint8_t>(integer);
            return ValueStatus::valid;
        }
        case FieldKind::u16le: {
            auto const status = readNumber(context, value, 0xffffU, integer);
            if (status != ValueStatus::valid)
                return status;
            output[0] = static_cast<std::uint8_t>(integer);
            output[1] = static_cast<std::uint8_t>(integer >> 8);
            return ValueStatus::valid;
        }
        case FieldKind::u32le: {
            auto const status = readNumber(
                context, value, std::numeric_limits<std::uint32_t>::max(),
                integer);
            if (status != ValueStatus::valid)
                return status;
            for (std::uint32_t index = 0; index < 4; ++index)
                output[index] = static_cast<std::uint8_t>(integer >> (index * 8));
            return ValueStatus::valid;
        }
        case FieldKind::u64le: {
            auto const status = readBigUint64(context, value, integer);
            if (status != ValueStatus::valid)
                return status;
            for (std::uint32_t index = 0; index < 8; ++index)
                output[index] = static_cast<std::uint8_t>(integer >> (index * 8));
            return ValueStatus::valid;
        }
        case FieldKind::xflle: {
            std::int64_t raw = 0;
            if (!readXFLDecimalRaw(value, &raw))
                return ValueStatus::invalid;
            std::uint64_t const word = static_cast<std::uint64_t>(raw);
            for (std::uint32_t index = 0; index < 8; ++index)
                output[index] =
                    static_cast<std::uint8_t>(word >> (index * 8));
            return ValueStatus::valid;
        }
        case FieldKind::bytes: {
            NominalPayloadView payload{};
            if (!detail::readSTBlobNominalPayload(value, payload) ||
                payload.size != field.width)
                return ValueStatus::invalid;
            if (field.width != 0)
                std::memcpy(output, payload.data, field.width);
            return ValueStatus::valid;
        }
        case FieldKind::hash256: {
            std::uint8_t scratch[8] = {};
            NominalPayloadView payload{};
            if (!readNominalPayload(
                    context,
                    value,
                    xdata::MaterializerKind::hash256,
                    scratch,
                    payload) ||
                payload.size != field.width)
                return ValueStatus::invalid;
            std::memcpy(output, payload.data, field.width);
            return ValueStatus::valid;
        }
        case FieldKind::accountID: {
            std::uint8_t account[20] = {};
            if (field.width != sizeof(account) ||
                !readAccountIDBytes(context, value, account))
                return ValueStatus::invalid;
            std::memcpy(output, account, sizeof(account));
            return ValueStatus::valid;
        }
        case FieldKind::padding:
            std::memset(output, 0, field.width);
            return ValueStatus::valid;
    }
    return ValueStatus::invalid;
}

[[nodiscard]] JSValue
parseSchemaBytes(
    JSContext* context,
    SchemaState const& schema,
    std::uint8_t const* data,
    std::uint32_t length,
    bool safe)
{
    if (length != schema.byteLength) {
        if (safe)
            return wrongLengthFailure(
                context, schema.byteLength, length);
        return JS_ThrowTypeError(
            context,
            "record schema expected %u bytes (got %u)",
            schema.byteLength,
            length);
    }

    auto const* fields = schemaFields(&schema);
    if (schema.scalar) {
        bool invalid = false;
        JSValue value = decodeField(
            context, fields[0], data, invalid);
        if (invalid)
            return safe
                ? parseValueFailure(context, JS_ATOM_NULL)
                : throwInvalidParse(context, JS_ATOM_NULL);
        return safe ? bindings::result_success(context, value) : value;
    }

    qjs::OwnedValue value(context, JS_NewObjectProto(context, JS_NULL));
    if (value.isException())
        return value.release();
    for (std::uint16_t index = 0; index < schema.fieldCount; ++index) {
        auto const& field = fields[index];
        if (field.name == JS_ATOM_NULL)
            continue;
        bool invalid = false;
        JSValue decoded = decodeField(
            context, field, data, invalid);
        if (invalid)
            return safe
                ? parseValueFailure(context, field.name)
                : throwInvalidParse(context, field.name);
        if (JS_IsException(decoded) ||
            JS_DefinePropertyValue(
                context,
                value.get(),
                field.name,
                decoded,
                JS_PROP_ENUMERABLE) < 0)
            return JS_EXCEPTION;
    }
    if (!qjs::freezeObject(context, value.get()))
        return JS_EXCEPTION;
    return safe
        ? bindings::result_success(context, value.release())
        : value.release();
}

[[nodiscard]] JSValue
parseSchema(
    JSContext* context,
    SchemaState const& schema,
    JSValueConst input,
    bool safe)
{
    auto bytes = qjs::ByteView::getBinding(
        context,
        input,
        "BinarySchema.safeParse",
        0,
        qjs::BytePolicy::bytesLikeOrSTBlob);
    if (!bytes)
        return qjs::byteInputTypeError(
            context,
            safe ? "BinarySchema.safeParse()" : "record schema parse()",
            qjs::BytePolicy::bytesLikeOrSTBlob);
    return parseSchemaBytes(
        context,
        schema,
        bytes.data(),
        static_cast<std::uint32_t>(bytes.size()),
        safe);
}

[[nodiscard]] JSValue
throwInvalidEncode(JSContext* context, JSAtom field)
{
    if (field == JS_ATOM_NULL)
        return JS_ThrowTypeError(context, "record schema value is invalid");
    qjs::OwnedValue name(context, JS_AtomToString(context, field));
    if (name.isException())
        return name.release();
    char const* text = JS_ToCString(context, name.get());
    if (text == nullptr)
        return JS_EXCEPTION;
    JSValue error = JS_ThrowTypeError(
        context, "record field '%s' is invalid", text);
    JS_FreeCString(context, text);
    return error;
}

[[nodiscard]] JSValue
encodeSchema(
    JSContext* context,
    SchemaState const& schema,
    JSValueConst input,
    bool safe)
{
    std::uint8_t* output = nullptr;
    qjs::OwnedValue blob(
        context,
        makeSTBlobUninitialized(context, schema.byteLength, &output));
    if (blob.isException())
        return blob.release();
    if (schema.byteLength != 0)
        std::memset(output, 0, schema.byteLength);

    auto const* fields = schemaFields(&schema);
    JSAtom invalidField = JS_ATOM_NULL;
    ValueStatus status = ValueStatus::valid;
    if (schema.scalar) {
        status = encodeField(context, fields[0], input, output);
    } else if (!JS_IsObject(input)) {
        status = ValueStatus::invalid;
    } else {
        for (std::uint16_t index = 0; index < schema.fieldCount; ++index) {
            auto const& field = fields[index];
            if (field.name == JS_ATOM_NULL)
                continue;
            qjs::OwnedValue fieldValue(
                context, JS_GetProperty(context, input, field.name));
            if (fieldValue.isException())
                return fieldValue.release();
            status = encodeField(
                context,
                field,
                fieldValue.get(),
                output + field.offset);
            if (status != ValueStatus::valid) {
                invalidField = field.name;
                break;
            }
        }
    }

    if (status == ValueStatus::exception)
        return JS_EXCEPTION;
    if (status == ValueStatus::invalid)
        return safe
            ? encodeFailure(context, invalidField)
            : throwInvalidEncode(context, invalidField);
    return safe
        ? bindings::result_success(context, blob.release())
        : blob.release();
}

// @binding provider:ScalarSchema.name
// @binding provider:RecordSchema.name
[[nodiscard]] JSValue
schemaName(JSContext* context, JSValueConst thisValue)
{
    auto* state = schemaState(context, thisValue);
    return state == nullptr
        ? JS_EXCEPTION
        : JS_AtomToString(context, state->name);
}

// @binding provider:BinarySchema.byteLength
// @binding provider:ScalarSchema.byteLength
// @binding provider:RecordSchema.byteLength
[[nodiscard]] JSValue
schemaByteLength(JSContext* context, JSValueConst thisValue)
{
    auto* state = schemaState(context, thisValue);
    return state == nullptr
        ? JS_EXCEPTION
        : JS_NewUint32(context, state->byteLength);
}

// @binding provider:BinarySchema.safeParse
// @binding provider:ScalarSchema.safeParse
// @binding provider:RecordSchema.safeParse
[[nodiscard]] JSValue
schemaSafeParse(
    JSContext* context,
    JSValueConst thisValue,
    int argc,
    JSValueConst* argv)
{
    auto* state = schemaState(context, thisValue);
    if (state == nullptr)
        return JS_EXCEPTION;
    return parseSchema(
        context,
        *state,
        argc > 0 ? argv[0] : JS_UNDEFINED,
        true);
}

// @binding provider:ScalarSchema.parse
// @binding provider:RecordSchema.parse
[[nodiscard]] JSValue
schemaParse(
    JSContext* context,
    JSValueConst thisValue,
    int argc,
    JSValueConst* argv)
{
    auto* state = schemaState(context, thisValue);
    if (state == nullptr)
        return JS_EXCEPTION;
    return parseSchema(
        context,
        *state,
        argc > 0 ? argv[0] : JS_UNDEFINED,
        false);
}

// @binding provider:ScalarSchema.safeEncode
// @binding provider:RecordSchema.safeEncode
[[nodiscard]] JSValue
schemaSafeEncode(
    JSContext* context,
    JSValueConst thisValue,
    int argc,
    JSValueConst* argv)
{
    auto* state = schemaState(context, thisValue);
    if (state == nullptr)
        return JS_EXCEPTION;
    return encodeSchema(
        context,
        *state,
        argc > 0 ? argv[0] : JS_UNDEFINED,
        true);
}

// @binding provider:ScalarSchema.encode
// @binding provider:RecordSchema.encode
[[nodiscard]] JSValue
schemaEncode(
    JSContext* context,
    JSValueConst thisValue,
    int argc,
    JSValueConst* argv)
{
    auto* state = schemaState(context, thisValue);
    if (state == nullptr)
        return JS_EXCEPTION;
    return encodeSchema(
        context,
        *state,
        argc > 0 ? argv[0] : JS_UNDEFINED,
        false);
}

// @binding provider:RecordSchema.patch
[[nodiscard]] JSValue
schemaPatch(
    JSContext* context,
    JSValueConst thisValue,
    int argc,
    JSValueConst* argv)
{
    auto* state = schemaState(context, thisValue, true);
    if (state == nullptr)
        return JS_EXCEPTION;
    JSValueConst sourceValue = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst replacements = argc > 1 ? argv[1] : JS_UNDEFINED;
    auto source = qjs::ByteView::getBinding(
        context,
        sourceValue,
        "RecordSchema.patch",
        0,
        qjs::BytePolicy::bytesLikeOrSTBlob);
    if (!source)
        return qjs::byteInputTypeError(
            context,
            "RecordSchema.patch()",
            qjs::BytePolicy::bytesLikeOrSTBlob);
    if (source.size() != state->byteLength)
        return wrongLengthFailure(
            context, state->byteLength, source.size());
    if (!JS_IsObject(replacements))
        return JS_ThrowTypeError(
            context, "RecordSchema.patch() expects replacement fields");

    std::uint8_t* output = nullptr;
    qjs::OwnedValue blob(
        context,
        makeSTBlobUninitialized(context, state->byteLength, &output));
    if (blob.isException())
        return blob.release();
    if (state->byteLength != 0)
        std::memcpy(output, source.data(), state->byteLength);

    auto const* fields = schemaFields(state);
    for (std::uint16_t index = 0; index < state->fieldCount; ++index) {
        auto const& field = fields[index];
        if (field.name == JS_ATOM_NULL)
            continue;
        int const present = JS_HasProperty(context, replacements, field.name);
        if (present < 0)
            return JS_EXCEPTION;
        if (present == 0)
            continue;
        qjs::OwnedValue value(
            context, JS_GetProperty(context, replacements, field.name));
        if (value.isException())
            return value.release();
        ValueStatus const status = encodeField(
            context,
            field,
            value.get(),
            output + field.offset);
        if (status == ValueStatus::exception)
            return JS_EXCEPTION;
        if (status == ValueStatus::invalid)
            return throwInvalidEncode(context, field.name);
    }
    return bindings::result_success(context, blob.release());
}

// @binding provider:RecordField.byteLength
[[nodiscard]] JSValue
fieldByteLength(JSContext* context, JSValueConst thisValue)
{
    auto const* state = fieldState(thisValue);
    return state == nullptr
        ? JS_ThrowTypeError(context, "RecordField.byteLength: invalid receiver")
        : JS_NewUint32(context, state->width);
}

[[nodiscard]] bool
readNameAtom(
    JSContext* context,
    JSValueConst value,
    char const* operation,
    JSAtom& output)
{
    output = JS_ATOM_NULL;
    if (!JS_IsString(value)) {
        JS_ThrowTypeError(context, "%s expects a string name", operation);
        return false;
    }
    std::size_t length = 0;
    char const* text = JS_ToCStringLen(context, &length, value);
    if (text == nullptr)
        return false;
    JS_FreeCString(context, text);
    if (length == 0) {
        JS_ThrowTypeError(context, "%s name must not be empty", operation);
        return false;
    }
    output = JS_ValueToAtom(context, value);
    return output != JS_ATOM_NULL;
}

[[nodiscard]] JSValue
newSchema(
    JSContext* context,
    JSAtom name,
    std::uint32_t byteLength,
    bool scalar,
    FieldSpec const* specs,
    std::uint16_t fieldCount)
{
    std::size_t const allocationSize =
        sizeof(SchemaState) + sizeof(FieldSpec) * fieldCount;
    auto* state = static_cast<SchemaState*>(
        js_mallocz(context, allocationSize));
    if (state == nullptr) {
        if (name != JS_ATOM_NULL)
            JS_FreeAtom(context, name);
        for (std::uint16_t index = 0; index < fieldCount; ++index) {
            if (specs[index].name != JS_ATOM_NULL)
                JS_FreeAtom(context, specs[index].name);
        }
        return JS_ThrowOutOfMemory(context);
    }
    state->name = name;
    state->byteLength = byteLength;
    state->fieldCount = fieldCount;
    state->scalar = scalar;
    if (fieldCount != 0)
        std::memcpy(
            schemaFields(state), specs, sizeof(FieldSpec) * fieldCount);

    JSClassID const classId = scalar
        ? scalarSchemaClassId
        : recordSchemaClassId;
    JSValue value = JS_NewObjectClass(context, classId);
    if (JS_IsException(value)) {
        freeSchemaState(JS_GetRuntime(context), state);
        return value;
    }
    JS_SetOpaque(value, state);
    if (!qjs::freezeObject(context, value)) {
        JS_FreeValue(context, value);
        return JS_EXCEPTION;
    }
    return value;
}

// @binding provider:cell
[[nodiscard]] JSValue
cellFactory(
    JSContext* context,
    JSValueConst,
    int argc,
    JSValueConst* argv)
{
    JSValueConst nameValue = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst fieldValue = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSAtom name = JS_ATOM_NULL;
    if (!readNameAtom(context, nameValue, "cell()", name))
        return JS_EXCEPTION;
    auto const* field = fieldState(fieldValue);
    if (field == nullptr || field->kind == FieldKind::padding) {
        JS_FreeAtom(context, name);
        return JS_ThrowTypeError(
            context, "cell() expects one non-padding RecordField");
    }
    FieldSpec spec{
        .name = JS_ATOM_NULL,
        .offset = 0,
        .width = field->width,
        .kind = field->kind,
    };
    return newSchema(context, name, field->width, true, &spec, 1);
}

[[nodiscard]] bool
readIndex(
    JSContext* context,
    JSValueConst value,
    std::uint32_t maximum,
    std::uint32_t& output)
{
    std::uint64_t wide = 0;
    if (JS_ToIndex(context, &wide, value) < 0)
        return false;
    if (wide > maximum) {
        JS_ThrowRangeError(context, "record value exceeds limit %u", maximum);
        return false;
    }
    output = static_cast<std::uint32_t>(wide);
    return true;
}

void
freeSpecs(JSContext* context, FieldSpec* specs, std::uint32_t count) noexcept
{
    if (specs == nullptr)
        return;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (specs[index].name != JS_ATOM_NULL)
            JS_FreeAtom(context, specs[index].name);
    }
    js_free(context, specs);
}

// @binding provider:record
[[nodiscard]] JSValue
recordFactory(
    JSContext* context,
    JSValueConst,
    int argc,
    JSValueConst* argv)
{
    JSValueConst nameValue = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst lengthValue = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst entriesValue = argc > 2 ? argv[2] : JS_UNDEFINED;

    JSAtom schemaName = JS_ATOM_NULL;
    if (!readNameAtom(context, nameValue, "record()", schemaName))
        return JS_EXCEPTION;
    std::uint32_t declaredLength = 0;
    if (!readIndex(
            context,
            lengthValue,
            maximumRecordBytes,
            declaredLength)) {
        JS_FreeAtom(context, schemaName);
        return JS_EXCEPTION;
    }
    int const isArray = JS_IsArray(context, entriesValue);
    if (isArray < 0) {
        JS_FreeAtom(context, schemaName);
        return JS_EXCEPTION;
    }
    if (isArray == 0) {
        JS_FreeAtom(context, schemaName);
        return JS_ThrowTypeError(context, "record() expects an entry array");
    }
    qjs::OwnedValue countValue(
        context, JS_GetPropertyStr(context, entriesValue, "length"));
    std::uint32_t count = 0;
    if (countValue.isException() ||
        !readIndex(
            context,
            countValue.get(),
            maximumRecordFields,
            count)) {
        JS_FreeAtom(context, schemaName);
        return JS_EXCEPTION;
    }
    auto* specs = count == 0
        ? nullptr
        : static_cast<FieldSpec*>(
              js_mallocz(context, sizeof(FieldSpec) * count));
    if (count != 0 && specs == nullptr) {
        JS_FreeAtom(context, schemaName);
        return JS_ThrowOutOfMemory(context);
    }

    std::uint32_t cursor = 0;
    std::uint32_t built = 0;
    for (; built < count; ++built) {
        qjs::OwnedValue entry(
            context, JS_GetPropertyUint32(context, entriesValue, built));
        if (entry.isException())
            break;
        auto const* bareField = fieldState(entry.get());
        if (bareField != nullptr) {
            if (bareField->kind != FieldKind::padding) {
                JS_ThrowTypeError(
                    context,
                    "record() bare entries must be padding fields");
                break;
            }
            specs[built] = {
                .name = JS_ATOM_NULL,
                .offset = cursor,
                .width = bareField->width,
                .kind = bareField->kind,
            };
        } else {
            int const tuple = JS_IsArray(context, entry.get());
            if (tuple <= 0) {
                if (tuple == 0)
                    JS_ThrowTypeError(
                        context, "record() entry must be a field tuple");
                break;
            }
            qjs::OwnedValue tupleLengthValue(
                context, JS_GetPropertyStr(context, entry.get(), "length"));
            std::uint32_t tupleLength = 0;
            if (tupleLengthValue.isException() ||
                !readIndex(context, tupleLengthValue.get(), 3, tupleLength))
                break;
            if (tupleLength != 2 && tupleLength != 3) {
                JS_ThrowTypeError(
                    context, "record() field tuple needs 2 or 3 elements");
                break;
            }
            qjs::OwnedValue fieldNameValue(
                context, JS_GetPropertyUint32(context, entry.get(), 0));
            qjs::OwnedValue fieldValue(
                context, JS_GetPropertyUint32(context, entry.get(), 1));
            if (fieldNameValue.isException() || fieldValue.isException())
                break;
            JSAtom fieldName = JS_ATOM_NULL;
            if (!readNameAtom(
                    context,
                    fieldNameValue.get(),
                    "record() field",
                    fieldName))
                break;
            auto const* field = fieldState(fieldValue.get());
            if (field == nullptr || field->kind == FieldKind::padding) {
                JS_FreeAtom(context, fieldName);
                JS_ThrowTypeError(
                    context,
                    "record() named entry expects a non-padding RecordField");
                break;
            }
            bool duplicate = false;
            for (std::uint32_t prior = 0; prior < built; ++prior)
                duplicate = duplicate || specs[prior].name == fieldName;
            if (duplicate) {
                JS_FreeAtom(context, fieldName);
                JS_ThrowTypeError(
                    context, "record() field names must be unique");
                break;
            }
            if (tupleLength == 3) {
                qjs::OwnedValue layout(
                    context, JS_GetPropertyUint32(context, entry.get(), 2));
                if (layout.isException()) {
                    JS_FreeAtom(context, fieldName);
                    break;
                }
                if (!JS_IsObject(layout.get())) {
                    JS_FreeAtom(context, fieldName);
                    JS_ThrowTypeError(
                        context, "record() layout claim must be an object");
                    break;
                }
                qjs::OwnedValue expectedValue(
                    context,
                    JS_GetPropertyStr(context, layout.get(), "expectOffset"));
                std::uint32_t expected = 0;
                if (expectedValue.isException() ||
                    !readIndex(
                        context,
                        expectedValue.get(),
                        maximumRecordBytes,
                        expected)) {
                    JS_FreeAtom(context, fieldName);
                    break;
                }
                if (expected != cursor) {
                    JS_FreeAtom(context, fieldName);
                    JS_ThrowRangeError(
                        context,
                        "record() expected offset %u but derived %u",
                        expected,
                        cursor);
                    break;
                }
            }
            specs[built] = {
                .name = fieldName,
                .offset = cursor,
                .width = field->width,
                .kind = field->kind,
            };
        }

        if (specs[built].width > declaredLength -
                (cursor > declaredLength ? declaredLength : cursor)) {
            JS_ThrowRangeError(
                context, "record() fields exceed declared byte length");
            break;
        }
        cursor += specs[built].width;
    }

    if (built != count || JS_HasException(context)) {
        freeSpecs(context, specs, built + (built < count ? 1U : 0U));
        JS_FreeAtom(context, schemaName);
        return JS_EXCEPTION;
    }
    if (cursor != declaredLength) {
        freeSpecs(context, specs, count);
        JS_FreeAtom(context, schemaName);
        return JS_ThrowRangeError(
            context,
            "record() derived extent %u does not equal declared length %u",
            cursor,
            declaredLength);
    }

    JSValue schema = newSchema(
        context,
        schemaName,
        declaredLength,
        false,
        specs,
        static_cast<std::uint16_t>(count));
    for (std::uint32_t index = 0; index < count; ++index)
        specs[index].name = JS_ATOM_NULL;
    freeSpecs(context, specs, count);
    return schema;
}

[[nodiscard]] JSValue
recordU8(JSContext* context, JSValueConst, int, JSValueConst*)
{
    return newField(context, FieldKind::u8, 1);
}

[[nodiscard]] JSValue
recordU16le(JSContext* context, JSValueConst, int, JSValueConst*)
{
    return newField(context, FieldKind::u16le, 2);
}

[[nodiscard]] JSValue
recordU32le(JSContext* context, JSValueConst, int, JSValueConst*)
{
    return newField(context, FieldKind::u32le, 4);
}

[[nodiscard]] JSValue
recordU64le(JSContext* context, JSValueConst, int, JSValueConst*)
{
    return newField(context, FieldKind::u64le, 8);
}

[[nodiscard]] JSValue
recordXflle(JSContext* context, JSValueConst, int, JSValueConst*)
{
    return newField(context, FieldKind::xflle, 8);
}

[[nodiscard]] JSValue
recordBytes(
    JSContext* context,
    JSValueConst,
    int argc,
    JSValueConst* argv)
{
    std::uint32_t width = 0;
    if (!readIndex(
            context,
            argc > 0 ? argv[0] : JS_UNDEFINED,
            maximumRecordBytes,
            width))
        return JS_EXCEPTION;
    return newField(context, FieldKind::bytes, width);
}

[[nodiscard]] JSValue
recordHash(
    JSContext* context,
    JSValueConst,
    int argc,
    JSValueConst* argv)
{
    std::uint32_t width = 0;
    if (!readIndex(
            context,
            argc > 0 ? argv[0] : JS_UNDEFINED,
            32,
            width))
        return JS_EXCEPTION;
    if (width != 32)
        return JS_ThrowRangeError(
            context, "record.hash() currently supports only 32 bytes");
    return newField(context, FieldKind::hash256, width);
}

[[nodiscard]] JSValue
recordAccountID(JSContext* context, JSValueConst, int, JSValueConst*)
{
    return newField(context, FieldKind::accountID, 20);
}

[[nodiscard]] JSValue
recordPadding(
    JSContext* context,
    JSValueConst,
    int argc,
    JSValueConst* argv)
{
    std::uint32_t width = 0;
    if (!readIndex(
            context,
            argc > 0 ? argv[0] : JS_UNDEFINED,
            maximumRecordBytes,
            width))
        return JS_EXCEPTION;
    return newField(context, FieldKind::padding, width);
}

JSCFunctionListEntry const fieldPrototype[] = {
    JS_CGETSET_DEF("byteLength", fieldByteLength, nullptr),
};

JSCFunctionListEntry const scalarSchemaPrototype[] = {
    JS_CGETSET_DEF("name", schemaName, nullptr),
    JS_CGETSET_DEF("byteLength", schemaByteLength, nullptr),
    JS_CFUNC_DEF("safeParse", 1, schemaSafeParse),
    JS_CFUNC_DEF("parse", 1, schemaParse),
    JS_CFUNC_DEF("safeEncode", 1, schemaSafeEncode),
    JS_CFUNC_DEF("encode", 1, schemaEncode),
};

JSCFunctionListEntry const recordSchemaPrototype[] = {
    JS_CGETSET_DEF("name", schemaName, nullptr),
    JS_CGETSET_DEF("byteLength", schemaByteLength, nullptr),
    JS_CFUNC_DEF("safeParse", 1, schemaSafeParse),
    JS_CFUNC_DEF("parse", 1, schemaParse),
    JS_CFUNC_DEF("safeEncode", 1, schemaSafeEncode),
    JS_CFUNC_DEF("encode", 1, schemaEncode),
    JS_CFUNC_DEF("patch", 2, schemaPatch),
};

JSCFunctionListEntry const recordFactories[] = {
    // @binding provider:record.u8
    JS_CFUNC_DEF("u8", 0, recordU8),
    // @binding provider:record.u16le
    JS_CFUNC_DEF("u16le", 0, recordU16le),
    // @binding provider:record.u32le
    JS_CFUNC_DEF("u32le", 0, recordU32le),
    // @binding provider:record.u64le
    JS_CFUNC_DEF("u64le", 0, recordU64le),
    // @binding provider:record.xflle
    JS_CFUNC_DEF("xflle", 0, recordXflle),
    // @binding provider:record.bytes
    JS_CFUNC_DEF("bytes", 1, recordBytes),
    // @binding provider:record.hash
    JS_CFUNC_DEF("hash", 1, recordHash),
    // @binding provider:record.accountID
    JS_CFUNC_DEF("accountID", 0, recordAccountID),
    // @binding provider:record.padding
    JS_CFUNC_DEF("padding", 1, recordPadding),
};

[[nodiscard]] bool
publishFunction(
    JSContext* context,
    JSValueConst global,
    char const* name,
    JSCFunction* function,
    int arity,
    std::span<JSCFunctionListEntry const> properties = {})
{
    qjs::OwnedValue value(
        context, JS_NewCFunction(context, function, name, arity));
    if (value.isException() ||
        (!properties.empty() &&
         !qjs::installFunctions(context, value.get(), properties)) ||
        !qjs::freezeObject(context, value.get()))
        return false;
    return JS_DefinePropertyValueStr(
               context,
               global,
               name,
               value.release(),
               JS_PROP_ENUMERABLE) >= 0;
}

}  // namespace

bool
registerRecordSchemas(JSContext* context, JSValueConst global)
{
    return registerHiddenClass(
               context,
               &fieldClassId,
               &fieldClass,
               fieldPrototype) &&
        registerHiddenClass(
               context,
               &scalarSchemaClassId,
               &scalarSchemaClass,
               scalarSchemaPrototype) &&
        registerHiddenClass(
               context,
               &recordSchemaClassId,
               &recordSchemaClass,
               recordSchemaPrototype) &&
        publishFunction(context, global, "cell", cellFactory, 2) &&
        publishFunction(
               context,
               global,
               "record",
               recordFactory,
               3,
               recordFactories) &&
        !JS_HasException(context);
}

bool readBinaryCodecByteLength(
    JSContext* context,
    JSValueConst codec,
    std::uint32_t* byteLength) noexcept
{
    if (byteLength != nullptr)
        *byteLength = 0;
    if (context == nullptr || byteLength == nullptr)
        return false;
    if (auto const* field = fieldState(codec); field != nullptr) {
        if (field->kind == FieldKind::padding) {
            JS_ThrowTypeError(
                context, "binary codec: padding cannot decode a value");
            return false;
        }
        *byteLength = field->width;
        return true;
    }
    auto const* schema = schemaState(context, codec);
    if (schema == nullptr)
        return false;
    *byteLength = schema->byteLength;
    return true;
}

JSValue safeParseBinaryCodecBytes(
    JSContext* context,
    JSValueConst codec,
    std::uint8_t const* bytes,
    std::uint32_t length)
{
    if (length != 0 && bytes == nullptr)
        return JS_ThrowInternalError(
            context, "binary codec host bytes are unavailable");

    if (auto const* field = fieldState(codec); field != nullptr) {
        if (field->kind == FieldKind::padding)
            return JS_ThrowTypeError(
                context, "binary codec: padding cannot decode a value");
        if (length != field->width)
            return wrongLengthFailure(context, field->width, length);
        FieldSpec const spec{
            .name = JS_ATOM_NULL,
            .offset = 0,
            .width = field->width,
            .kind = field->kind,
        };
        bool invalid = false;
        JSValue decoded = decodeField(context, spec, bytes, invalid);
        if (invalid)
            return parseValueFailure(context, JS_ATOM_NULL);
        if (JS_IsException(decoded))
            return decoded;
        return bindings::result_success(context, decoded);
    }

    auto const* schema = schemaState(context, codec);
    if (schema == nullptr)
        return JS_EXCEPTION;
    return parseSchemaBytes(context, *schema, bytes, length, true);
}

}  // namespace jshookz::provider::types
