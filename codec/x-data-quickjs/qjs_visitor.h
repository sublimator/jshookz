//@@start what-it-replaces
// qjs_visitor.h — QuickJS visitor: binary → JSValue directly (no JSON string roundtrip)
//
// Drop-in replacement for JsonVisitor. Instead of building boost::json::value,
// builds QuickJS JSValue objects directly. Objects returned are instances of
// the STObject JS class (empty prototype for now — methods can be added later).
//
// Eliminates: boost::json from decode path, JSON serialization, JS_ParseJSON.
//@@end what-it-replaces

#pragma once

extern "C" {
#include "../../engine/quickjs/quickjs.h"
}

#include "catl/xdata/codecs/codecs.h"
#include "catl/xdata/hex.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/parser.h"
#include "catl/xdata/slice-visitor.h"
#include "catl/xdata/types/issue.h"
#include "catl/xdata/types/number.h"
#include <cstring>
#include <expected>
#include <stack>
#include <utility>
#include <vector>

namespace catl::xdata {

// Forward declarations — defined later in this file
inline JSValue decode_field_value_js(JSContext* ctx, FieldDef const& field,
                                      Slice const& data, Protocol const& protocol);

// ---- STObject JS class with exotic methods ----
//
// Uses QuickJS exotic object protocol to intercept property get/set.
// Opaque data holds a "cache" JS object (plain object with decoded fields).
// get_property → reads from cache
// set_property → writes to cache (later: marks dirty for incremental encode)
//
// This is the scaffolding for lazy parsing: swap the eager cache for
// on-demand decode from _bytes + field offsets.

//@@start field-offset
// 12 bytes on wasm32: ptr(4) + offset(4) + length(4).
// 15 fields × 12 = 180 bytes per STObject. Trivial.
struct FieldOffset {
    const FieldDef* field;     // stable pointer into Protocol
    uint32_t offset;           // byte offset of payload in _bytes (after header + VL prefix)
    uint32_t length;           // byte length of payload (needed: codecs check size for type dispatch)
};
//@@end field-offset

struct STObjectData {
    JSValue cache;       // plain JS object holding decoded fields (lazy: starts empty)
    JSValue bytes;       // owned ArrayBuffer of original serialized data (or JS_UNDEFINED)
    bool has_overrides;  // true if any field was set by JS code
    JSValue root_obj;    // strong ref to root view object (JS_UNDEFINED on root)
    STObjectData* root_data;  // borrowed pointer to root data; self on root
    bool subtree_dirty;  // root-level dirty flag, wired to mutations in M2 step 6
    bool is_array_element;  // true for single-field STObject wrappers inside STArray
    std::vector<JSAtom> deleted;  // tombstoned field atoms, dup'd on insert

    // Field offset map (built once, used for lazy decode)
    std::vector<FieldOffset> offsets;
    const Protocol* protocol;  // non-owning pointer for lazy decode
    const uint8_t* raw_ptr;   // direct pointer into bytes buffer (valid while bytes lives)
    size_t raw_len;

    // Find a field offset by name
    const FieldOffset* find_offset(const char* name) const {
        for (auto& fo : offsets) {
            if (fo.field && fo.field->name == name)
                return &fo;
        }
        return nullptr;
    }

    bool is_deleted(JSAtom atom) const {
        for (auto deleted_atom : deleted) {
            if (deleted_atom == atom)
                return true;
        }
        return false;
    }
};

inline void
mark_dirty(STObjectData* data)
{
    data->has_overrides = true;
    STObjectData* root = data->root_data ? data->root_data : data;
    root->subtree_dirty = true;
}

inline void
add_deleted_atom(JSContext* ctx, STObjectData* data, JSAtom atom)
{
    if (data->is_deleted(atom))
        return;
    data->deleted.push_back(JS_DupAtom(ctx, atom));
}

inline void
remove_deleted_atom(JSContext* ctx, STObjectData* data, JSAtom atom)
{
    for (auto it = data->deleted.begin(); it != data->deleted.end(); ++it) {
        if (*it == atom) {
            JS_FreeAtom(ctx, *it);
            data->deleted.erase(it);
            return;
        }
    }
}

inline int
has_offset_atom(JSContext* ctx, STObjectData* data, JSAtom atom)
{
    const char* name = JS_AtomToCString(ctx, atom);
    if (!name)
        return -1;
    bool has_offset = data->find_offset(name) != nullptr;
    JS_FreeCString(ctx, name);
    return has_offset ? 1 : 0;
}

inline void
throw_array_element_shape_error(JSContext* ctx)
{
    JS_ThrowTypeError(ctx, "STArray element fields must remain object-valued");
}

inline void
attach_child_root_ref(
    JSContext* ctx,
    JSValueConst parent_obj,
    STObjectData* parent_data,
    STObjectData* child_data)
{
    JS_FreeValue(ctx, child_data->root_obj);
    if (JS_IsUndefined(parent_data->root_obj))
        child_data->root_obj = JS_DupValue(ctx, parent_obj);
    else
        child_data->root_obj = JS_DupValue(ctx, parent_data->root_obj);
    child_data->root_data =
        parent_data->root_data ? parent_data->root_data : parent_data;
}

// Forward declarations (need STObjectData and FieldOffset defined above)
inline std::expected<std::vector<FieldOffset>, CodecErrorValue>
build_field_offsets_expected(
    const uint8_t* data, size_t len, const Protocol& proto);
inline std::vector<FieldOffset> build_field_offsets(
    const uint8_t* data, size_t len, const Protocol& proto);
inline std::expected<std::vector<FieldOffset>, CodecErrorValue>
build_array_element_offsets_expected(
    const uint8_t* data, size_t len, const Protocol& proto);
struct STArrayClass;
inline JSValue lazy_decode_field(
    JSContext* ctx,
    JSValueConst owner_obj,
    STObjectData* data,
    const FieldOffset* fo);

struct STObjectClass {
    static JSClassID class_id;
    static JSValue prototype;
    static JSClassExoticMethods exotic;

    // --- Exotic get_own_property: for Object.keys, hasOwnProperty, in operator ---
    static int get_own_property(JSContext* ctx, JSPropertyDescriptor* desc,
                                 JSValueConst obj, JSAtom prop) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(obj, class_id));
        if (!data) return 0;
        if (data->is_deleted(prop))
            return 0;

        // Check cache
        JSValue val = JS_GetProperty(ctx, data->cache, prop);
        if (!JS_IsUndefined(val)) {
            if (desc) {
                desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
                desc->value = val;
                desc->getter = JS_UNDEFINED;
                desc->setter = JS_UNDEFINED;
            } else {
                JS_FreeValue(ctx, val);
            }
            return 1;
        }
        int has_cached = JS_HasProperty(ctx, data->cache, prop);
        if (has_cached < 0) {
            JS_FreeValue(ctx, val);
            return -1;
        }
        if (has_cached) {
            if (desc) {
                desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
                desc->value = val;
                desc->getter = JS_UNDEFINED;
                desc->setter = JS_UNDEFINED;
            } else {
                JS_FreeValue(ctx, val);
            }
            return 1;
        }
        JS_FreeValue(ctx, val);

        // Check offsets (field exists but not yet decoded)
        if (!data->offsets.empty()) {
            const char* name = JS_AtomToCString(ctx, prop);
            if (name) {
                const FieldOffset* fo = data->find_offset(name);
                JS_FreeCString(ctx, name);
                if (fo) {
                    if (desc) {
                        // Trigger lazy decode via get_property
                        desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
                        desc->value = get_property(ctx, obj, prop, JS_UNDEFINED);
                        desc->getter = JS_UNDEFINED;
                        desc->setter = JS_UNDEFINED;
                    }
                    return 1;
                }
            }
        }

        return 0;
    }

    // --- Exotic get_own_property_names: for Object.keys, for...in ---
    // Must return offset-map fields in TLV order, then cache-added keys.
    static int get_own_property_names(JSContext* ctx, JSPropertyEnum** ptab,
                                       uint32_t* plen, JSValueConst obj) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(obj, class_id));
        if (!data) { *ptab = nullptr; *plen = 0; return 0; }

        std::vector<JSPropertyEnum> names;
        names.reserve(data->offsets.size());
        auto free_names = [&]() {
            for (auto& name : names)
                JS_FreeAtom(ctx, name.atom);
            names.clear();
        };

        for (auto const& offset : data->offsets) {
            JSAtom atom = JS_NewAtom(ctx, offset.field->name.c_str());
            if (atom == JS_ATOM_NULL) {
                free_names();
                *ptab = nullptr;
                *plen = 0;
                return -1;
            }
            if (data->is_deleted(atom)) {
                JS_FreeAtom(ctx, atom);
                continue;
            }
            names.push_back({1, atom});
        }

        JSPropertyEnum* cache_tab = nullptr;
        uint32_t cache_len = 0;
        if (JS_GetOwnPropertyNames(
                ctx, &cache_tab, &cache_len, data->cache,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
            free_names();
            *ptab = nullptr;
            *plen = 0;
            return -1;
        }

        for (uint32_t i = 0; i < cache_len; ++i) {
            JSAtom atom = cache_tab[i].atom;
            const char* name = JS_AtomToCString(ctx, atom);
            if (!name) {
                for (uint32_t j = 0; j < cache_len; ++j)
                    JS_FreeAtom(ctx, cache_tab[j].atom);
                js_free(ctx, cache_tab);
                free_names();
                *ptab = nullptr;
                *plen = 0;
                return -1;
            }
            bool already_listed = data->find_offset(name) != nullptr;
            JS_FreeCString(ctx, name);
            if (!already_listed && !data->is_deleted(atom))
                names.push_back({1, JS_DupAtom(ctx, atom)});
        }

        for (uint32_t i = 0; i < cache_len; ++i)
            JS_FreeAtom(ctx, cache_tab[i].atom);
        js_free(ctx, cache_tab);

        if (names.empty()) {
            *ptab = nullptr;
            *plen = 0;
            return 0;
        }

        auto* tab = static_cast<JSPropertyEnum*>(
            js_malloc(ctx, sizeof(JSPropertyEnum) * names.size()));
        if (!tab) {
            free_names();
            *ptab = nullptr;
            *plen = 0;
            return -1;
        }

        for (size_t i = 0; i < names.size(); ++i)
            tab[i] = names[i];
        *ptab = tab;
        *plen = names.size();
        return 0;
    }

    // --- Exotic get_property: intercept obj.field and obj[idx] ---
    // Cache hit → return cached value
    // Cache miss + has offsets → lazy decode from bytes, cache result, return
    // Cache miss + no offsets → return undefined
    static JSValue get_property(JSContext* ctx, JSValueConst obj,
                                 JSAtom prop, JSValueConst receiver) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(obj, class_id));
        if (!data) return JS_UNDEFINED;
        if (data->is_deleted(prop))
            return JS_UNDEFINED;

        // Check cache first
        JSValue val = JS_GetProperty(ctx, data->cache, prop);
        if (!JS_IsUndefined(val))
            return val;
        int has_cached = JS_HasProperty(ctx, data->cache, prop);
        if (has_cached < 0) {
            JS_FreeValue(ctx, val);
            return JS_EXCEPTION;
        }
        if (has_cached)
            return val;
        JS_FreeValue(ctx, val);

        // Cache miss — try lazy decode from bytes + offsets
        if (!data->offsets.empty() && data->raw_ptr && data->protocol) {
            const char* name = JS_AtomToCString(ctx, prop);
            if (name) {
                const FieldOffset* fo = data->find_offset(name);
                if (fo && fo->field) {
                    JSValue decoded = lazy_decode_field(ctx, obj, data, fo);
                    if (JS_IsException(decoded)) {
                        JS_FreeCString(ctx, name);
                        return decoded;
                    }
                    JS_SetProperty(ctx, data->cache, prop, JS_DupValue(ctx, decoded));
                    JS_FreeCString(ctx, name);
                    return decoded;
                }
                JS_FreeCString(ctx, name);
            }
        }

        return JS_UNDEFINED;
    }

    // --- Exotic set_property: intercept obj.field = val ---
    static int set_property(JSContext* ctx, JSValueConst obj,
                             JSAtom prop, JSValueConst val,
                             JSValueConst receiver, int flags) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(obj, class_id));
        if (!data) return -1;
        int is_element_field = data->is_array_element ? has_offset_atom(ctx, data, prop) : 0;
        if (is_element_field < 0)
            return -1;
        if (is_element_field && !JS_IsObject(val)) {
            throw_array_element_shape_error(ctx);
            return -1;
        }
        if (JS_SetProperty(ctx, data->cache, prop, JS_DupValue(ctx, val)) < 0)
            return -1;
        remove_deleted_atom(ctx, data, prop);
        mark_dirty(data);
        return 1;
    }

    // --- Exotic delete_property: tombstone existing fields/cache keys ---
    static int delete_property(JSContext* ctx, JSValueConst obj, JSAtom prop) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(obj, class_id));
        if (!data) return -1;
        int is_element_field = data->is_array_element ? has_offset_atom(ctx, data, prop) : 0;
        if (is_element_field < 0)
            return -1;
        if (is_element_field) {
            throw_array_element_shape_error(ctx);
            return -1;
        }
        if (data->is_deleted(prop))
            return 1;

        int has_offset = has_offset_atom(ctx, data, prop);
        if (has_offset < 0)
            return -1;
        int has_cached = JS_HasProperty(ctx, data->cache, prop);
        if (has_cached < 0)
            return -1;
        if (!has_offset && !has_cached)
            return 1;

        if (JS_DeleteProperty(ctx, data->cache, prop, 0) < 0)
            return -1;
        add_deleted_atom(ctx, data, prop);
        mark_dirty(data);
        return 1;
    }

    //@@start define-own-property
    // --- Exotic define_own_property: reject descriptor definitions ---
    // Without this hook, Object.defineProperty / Reflect.defineProperty fall
    // through to an ordinary own property that shadows the lazy field and
    // leaks into encode.
    static int define_own_property(JSContext* ctx, JSValueConst obj,
                                    JSAtom prop, JSValueConst val,
                                    JSValueConst getter, JSValueConst setter,
                                    int flags) {
        JS_ThrowTypeError(
            ctx,
            "STObject views do not support descriptor definitions");
        return -1;
    }
    //@@end define-own-property

    // --- Finalizer: free opaque data ---
    static void finalizer(JSRuntime* rt, JSValue val) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(val, class_id));
        if (data) {
            JS_FreeValueRT(rt, data->cache);
            JS_FreeValueRT(rt, data->bytes);
            JS_FreeValueRT(rt, data->root_obj);
            for (auto atom : data->deleted)
                JS_FreeAtomRT(rt, atom);
            delete data;
        }
    }

    // --- GC mark: tell GC about our references ---
    static void gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(val, class_id));
        if (data) {
            JS_MarkValue(rt, data->cache, mark_func);
            JS_MarkValue(rt, data->bytes, mark_func);
            JS_MarkValue(rt, data->root_obj, mark_func);
            // deleted atoms are refcounted and freed in finalizer, not cycle-GC values.
        }
    }

    // --- Init class ---
    static void init(JSContext* ctx) {
        if (class_id != 0) return;

        //@@start exotic-table
        exotic = {};
        exotic.get_own_property = get_own_property;
        exotic.get_own_property_names = get_own_property_names;
        exotic.delete_property = delete_property;
        exotic.get_property = get_property;
        exotic.set_property = set_property;
        exotic.define_own_property = define_own_property;

        JSClassDef def = {
            .class_name = "STObject",
            .finalizer = finalizer,
            .gc_mark = gc_mark,
            .call = nullptr,
            .exotic = &exotic,
        };
        JS_NewClassID(&class_id);
        JS_NewClass(JS_GetRuntime(ctx), class_id, &def);
        //@@end exotic-table

        prototype = JS_NewObject(ctx);
        // Methods on the prototype (available on all STObjects):
        // JS_SetPropertyStr(ctx, prototype, "toBytes", ...);
        // JS_SetPropertyStr(ctx, prototype, "toHex", ...);
        JS_SetClassProto(ctx, class_id, prototype);
    }

    // --- Create new instance with empty cache ---
    static JSValue new_instance(JSContext* ctx) {
        if (class_id == 0)
            return JS_NewObject(ctx);

        JSValue obj = JS_NewObjectProtoClass(ctx, prototype, class_id);
        if (JS_IsException(obj)) return obj;

        auto* data = new STObjectData{
            .cache = JS_NewObjectProto(ctx, JS_NULL),
            .bytes = JS_UNDEFINED,
            .has_overrides = false,
            .root_obj = JS_UNDEFINED,
            .root_data = nullptr,
            .subtree_dirty = false,
            .is_array_element = false,
        };
        data->root_data = data;
        JS_SetOpaque(obj, data);
        return obj;
    }

    // --- Set a field on the cache (used during decode) ---
    static void set_field(JSContext* ctx, JSValue obj, const char* name, JSValue val) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(obj, class_id));
        if (data) {
            JS_SetPropertyStr(ctx, data->cache, name, val);
        } else {
            // Fallback for non-exotic objects
            JS_SetPropertyStr(ctx, obj, name, val);
        }
    }

    // --- Attach original bytes ---
    static void set_bytes(JSContext* ctx, JSValue obj, JSValue bytes_val) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(obj, class_id));
        if (data) {
            JS_FreeValue(ctx, data->bytes);
            data->bytes = JS_DupValue(ctx, bytes_val);
        }
    }

    //@@start fast-path-encode
    // --- Get original bytes (for fast-path encode) ---
    static JSValue get_bytes(JSContext* ctx, JSValueConst obj) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(obj, class_id));
        if (data && JS_IsUndefined(data->root_obj) &&
            !JS_IsUndefined(data->bytes) && !data->subtree_dirty)
            return JS_DupValue(ctx, data->bytes);
        return JS_UNDEFINED;
    }
    //@@end fast-path-encode

    // --- Check if this is our exotic object ---
    static bool is_stobject(JSValueConst obj) {
        return JS_GetOpaque(obj, class_id) != nullptr;
    }
};

// Static storage
inline JSClassID STObjectClass::class_id = 0;
inline JSValue STObjectClass::prototype = JS_UNDEFINED;
inline JSClassExoticMethods STObjectClass::exotic = {};

// ---- STArray JS class with exotic methods ----
//
// Lazy array of STObjects. Indexed by number (arr[0], arr[1]).
// Backed by the same STObjectData: bytes + offsets.
// Each offset entry is one array element (the wrapper STObject).
// .length returns offset count. Numeric access decodes on demand.

struct STArrayClass {
    static JSClassID class_id;
    static JSValue prototype;
    static JSClassExoticMethods exotic;

    // Helper: is this atom a numeric index? Uses tagged int fast path.
    static bool is_index(JSContext*, JSAtom prop, uint32_t* out) {
        if (JS_AtomIsTaggedInt(prop)) {
            if (out) *out = JS_AtomToTaggedInt(prop);
            return true;
        }
        return false;
    }

    static bool replacement_matches_element_shape(
        JSContext* ctx,
        JSValueConst val,
        const char* expected_name)
    {
        if (!JS_IsObject(val))
            return false;

        JSPropertyEnum* tab = nullptr;
        uint32_t tab_len = 0;
        if (JS_GetOwnPropertyNames(
                ctx, &tab, &tab_len, val,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
            JSValue exc = JS_GetException(ctx);
            JS_FreeValue(ctx, exc);
            return false;
        }

        bool ok = false;
        if (tab_len == 1) {
            const char* key = JS_AtomToCString(ctx, tab[0].atom);
            if (key) {
                if (strcmp(key, expected_name) == 0) {
                    JSValue inner = JS_GetProperty(ctx, val, tab[0].atom);
                    ok = JS_IsObject(inner);
                    JS_FreeValue(ctx, inner);
                }
                JS_FreeCString(ctx, key);
            } else {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);
            }
        }

        for (uint32_t i = 0; i < tab_len; ++i)
            JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);
        return ok;
    }

    // --- get_property: arr[0], arr.length ---
    static JSValue get_property(JSContext* ctx, JSValueConst obj,
                                 JSAtom prop, JSValueConst receiver) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(obj, class_id));
        if (!data) return JS_UNDEFINED;

        // Check "length"
        const char* name = JS_AtomToCString(ctx, prop);
        if (name && strcmp(name, "length") == 0) {
            JS_FreeCString(ctx, name);
            return JS_NewUint32(ctx, data->offsets.size());
        }
        if (name) JS_FreeCString(ctx, name);

        // Numeric index
        uint32_t idx;
        if (is_index(ctx, prop, &idx)) {
            if (idx >= data->offsets.size())
                return JS_UNDEFINED;

            // Check cache first
            JSValue cached = JS_GetPropertyUint32(ctx, data->cache, idx);
            if (!JS_IsUndefined(cached))
                return cached;
            JS_FreeValue(ctx, cached);

            // Each array element is: [field_header][inner object fields][0xE1].
            // Model it as a single-field STObject view over the array bytes;
            // lazy_decode_field will build the inner object on first access.
            auto& fo = data->offsets[idx];
            if (fo.field && data->raw_ptr && data->protocol) {
                JSValue wrapper = STObjectClass::new_instance(ctx);
                if (JS_IsException(wrapper))
                    return wrapper;

                auto* wrapper_data = static_cast<STObjectData*>(
                    JS_GetOpaque(wrapper, STObjectClass::class_id));
                if (wrapper_data) {
                    wrapper_data->bytes = JS_DupValue(ctx, data->bytes);
                    attach_child_root_ref(ctx, obj, data, wrapper_data);
                    wrapper_data->raw_ptr = data->raw_ptr;
                    wrapper_data->raw_len = data->raw_len;
                    wrapper_data->protocol = data->protocol;
                    wrapper_data->offsets.push_back(fo);
                    wrapper_data->is_array_element = true;
                }

                JS_SetPropertyUint32(ctx, data->cache, idx, JS_DupValue(ctx, wrapper));
                return wrapper;
            }
            return JS_UNDEFINED;
        }

        return JS_UNDEFINED;
    }

    // --- get_own_property: for 'in' operator, hasOwnProperty ---
    static int get_own_property(JSContext* ctx, JSPropertyDescriptor* desc,
                                 JSValueConst obj, JSAtom prop) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(obj, class_id));
        if (!data) return 0;

        const char* name = JS_AtomToCString(ctx, prop);
        if (name && strcmp(name, "length") == 0) {
            JS_FreeCString(ctx, name);
            if (desc) {
                desc->flags = 0;  // non-enumerable
                desc->value = JS_NewUint32(ctx, data->offsets.size());
                desc->getter = JS_UNDEFINED;
                desc->setter = JS_UNDEFINED;
            }
            return 1;
        }
        if (name) JS_FreeCString(ctx, name);

        uint32_t idx;
        if (is_index(ctx, prop, &idx) && idx < data->offsets.size()) {
            if (desc) {
                desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
                desc->value = get_property(ctx, obj, prop, JS_UNDEFINED);
                desc->getter = JS_UNDEFINED;
                desc->setter = JS_UNDEFINED;
            }
            return 1;
        }
        return 0;
    }

    // --- get_own_property_names: for Object.keys, for...in ---
    static int get_own_property_names(JSContext* ctx, JSPropertyEnum** ptab,
                                       uint32_t* plen, JSValueConst obj) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(obj, class_id));
        if (!data) { *ptab = nullptr; *plen = 0; return 0; }

        uint32_t count = data->offsets.size();
        if (count == 0) {
            *ptab = nullptr;
            *plen = 0;
            return 0;
        }
        auto* tab = static_cast<JSPropertyEnum*>(
            js_malloc(ctx, sizeof(JSPropertyEnum) * count));
        if (!tab) return -1;

        for (uint32_t i = 0; i < count; i++) {
            tab[i].is_enumerable = 1;
            tab[i].atom = JS_NewAtomUInt32(ctx, i);
        }
        *ptab = tab;
        *plen = count;
        return 0;
    }

    // --- set_property ---
    static int set_property(JSContext* ctx, JSValueConst obj,
                             JSAtom prop, JSValueConst val,
                             JSValueConst receiver, int flags) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(obj, class_id));
        if (!data) return -1;
        uint32_t idx;
        if (is_index(ctx, prop, &idx) && idx < data->offsets.size() &&
            data->offsets[idx].field &&
            replacement_matches_element_shape(
                ctx, val, data->offsets[idx].field->name.c_str())) {
            if (JS_SetProperty(ctx, data->cache, prop, JS_DupValue(ctx, val)) < 0)
                return -1;
            remove_deleted_atom(ctx, data, prop);
            mark_dirty(data);
            return 1;
        }
        JS_ThrowTypeError(
            ctx,
            "STArray views are fixed-shape; only existing elements can be "
            "replaced, and only with objects");
        return -1;
    }

    // --- delete_property ---
    static int delete_property(JSContext* ctx, JSValueConst obj, JSAtom prop) {
        JS_ThrowTypeError(
            ctx,
            "STArray views are fixed-shape; elements cannot be deleted");
        return -1;
    }

    // --- define_own_property: reject descriptor definitions (see STObject) ---
    static int define_own_property(JSContext* ctx, JSValueConst obj,
                                    JSAtom prop, JSValueConst val,
                                    JSValueConst getter, JSValueConst setter,
                                    int flags) {
        JS_ThrowTypeError(
            ctx,
            "STArray views are fixed-shape and do not support descriptor definitions");
        return -1;
    }

    // --- Finalizer + GC mark: same as STObject ---
    static void finalizer(JSRuntime* rt, JSValue val) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(val, class_id));
        if (data) {
            JS_FreeValueRT(rt, data->cache);
            JS_FreeValueRT(rt, data->bytes);
            JS_FreeValueRT(rt, data->root_obj);
            for (auto atom : data->deleted)
                JS_FreeAtomRT(rt, atom);
            delete data;
        }
    }

    static void gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func) {
        auto* data = static_cast<STObjectData*>(JS_GetOpaque(val, class_id));
        if (data) {
            JS_MarkValue(rt, data->cache, mark_func);
            JS_MarkValue(rt, data->bytes, mark_func);
            JS_MarkValue(rt, data->root_obj, mark_func);
            // deleted atoms are refcounted and freed in finalizer, not cycle-GC values.
        }
    }

    // --- Init ---
    static void init(JSContext* ctx) {
        if (class_id != 0) return;

        exotic = {};
        exotic.get_own_property = get_own_property;
        exotic.get_own_property_names = get_own_property_names;
        exotic.delete_property = delete_property;
        exotic.get_property = get_property;
        exotic.set_property = set_property;
        exotic.define_own_property = define_own_property;

        JSClassDef def = {
            .class_name = "STArray",
            .finalizer = finalizer,
            .gc_mark = gc_mark,
            .call = nullptr,
            .exotic = &exotic,
        };
        JS_NewClassID(&class_id);
        JS_NewClass(JS_GetRuntime(ctx), class_id, &def);

        prototype = JS_NewObject(ctx);
        JS_SetClassProto(ctx, class_id, prototype);
    }

    // --- Create instance from parent bytes + element offsets ---
    static JSValue new_from_bytes(JSContext* ctx,
                                   JSValueConst parent_obj,
                                   STObjectData* parent_data,
                                   const uint8_t* arr_data, size_t arr_len,
                                   const Protocol& proto) {
        if (class_id == 0) return JS_NewArray(ctx);  // fallback

        JSValue obj = JS_NewObjectProtoClass(ctx, prototype, class_id);
        if (JS_IsException(obj)) return obj;

        auto* data = new STObjectData{
            .cache = JS_NewObjectProto(ctx, JS_NULL),
            .bytes = JS_DupValue(ctx, parent_data->bytes),
            .has_overrides = false,
            .root_obj = JS_UNDEFINED,
            .root_data = nullptr,
            .subtree_dirty = false,
            .is_array_element = false,
        };
        data->root_data = data;
        attach_child_root_ref(ctx, parent_obj, parent_data, data);
        data->protocol = &proto;
        data->raw_ptr = arr_data;
        data->raw_len = arr_len;

        auto offsets = build_array_element_offsets_expected(
            arr_data, arr_len, proto);
        if (!offsets) {
            JS_FreeValue(ctx, obj);
            return JS_ThrowTypeError(
                ctx,
                "decode_object failed: %s",
                offsets.error().message.c_str());
        }
        data->offsets = std::move(*offsets);

        JS_SetOpaque(obj, data);
        return obj;
    }
};

inline JSClassID STArrayClass::class_id = 0;
inline JSValue STArrayClass::prototype = JS_UNDEFINED;
inline JSClassExoticMethods STArrayClass::exotic = {};

// ---- Lazy field decode (needs both STObjectClass and STArrayClass) ----

inline JSValue lazy_decode_field(
    JSContext* ctx,
    JSValueConst owner_obj,
    STObjectData* data,
    const FieldOffset* fo) {
    if (fo->field->meta.type == FieldTypes::STArray) {
        return STArrayClass::new_from_bytes(
            ctx, owner_obj, data,
            data->raw_ptr + fo->offset, fo->length,
            *data->protocol);
    }
    if (fo->field->meta.type == FieldTypes::STObject) {
        JSValue obj = STObjectClass::new_instance(ctx);
        if (!JS_IsException(obj)) {
            auto* inner = static_cast<STObjectData*>(
                JS_GetOpaque(obj, STObjectClass::class_id));
            if (inner) {
                inner->bytes = JS_DupValue(ctx, data->bytes);
                attach_child_root_ref(ctx, owner_obj, data, inner);
                inner->raw_ptr = data->raw_ptr + fo->offset;
                inner->raw_len = fo->length;
                inner->protocol = data->protocol;
                auto offsets = build_field_offsets_expected(
                    inner->raw_ptr, inner->raw_len, *data->protocol);
                if (!offsets) {
                    JS_FreeValue(ctx, obj);
                    return JS_ThrowTypeError(
                        ctx,
                        "decode_object failed: %s",
                        offsets.error().message.c_str());
                }
                inner->offsets = std::move(*offsets);
            }
        }
        return obj;
    }
    // Leaf field
    Slice field_data(data->raw_ptr + fo->offset, fo->length);
    return decode_field_value_js(ctx, *fo->field, field_data, *data->protocol);
}

// ---- Build field offset map (skip values, just track positions) ----
//
// Walks serialized bytes recording {field_code, name, offset, length}
// for each top-level field. No value decoding, no allocations beyond
// the vector. Cost: ~50 bytes of header reads for a typical transaction.

inline CodecErrorValue
decode_scan_error(std::string msg)
{
    return {CodecErrorCode::malformed_data, std::move(msg)};
}

inline bool
require_available_checked(
    size_t pos,
    size_t count,
    size_t len,
    const char* what,
    std::string& error)
{
    if (count > len || pos > len - count) {
        error = std::string("truncated ") + what;
        return false;
    }
    return true;
}

inline bool
read_field_header_checked(
    const uint8_t* data,
    size_t len,
    size_t& pos,
    uint32_t& field_code,
    std::string& error)
{
    field_code = 0;
    if (pos >= len) return true;

    uint8_t byte1 = data[pos++];
    uint32_t type = byte1 >> 4;
    uint32_t field = byte1 & 0x0F;

    // A continuation byte is only legal for a value >= 16; below that the
    // value belongs in the nibble. Anything smaller is malformed or
    // non-canonically encoded, and is an error rather than a quiet stop --
    // the format's terminator is the end-of-object marker, not a zero code.
    if (type == 0) {
        if (!require_available_checked(pos, 1, len, "field type", error))
            return false;
        type = data[pos++];
        if (type < 16) {
            error = "non-canonical field type byte " + std::to_string(type);
            return false;
        }
    }

    if (field == 0) {
        if (!require_available_checked(pos, 1, len, "field id", error))
            return false;
        field = data[pos++];
        if (field < 16) {
            error = "non-canonical field id byte " + std::to_string(field);
            return false;
        }
    }

    field_code = (type << 16) | field;
    return true;
}

inline bool
read_vl_length_checked(
    const uint8_t* data,
    size_t len,
    size_t& pos,
    size_t& field_size,
    std::string& error)
{
    if (!require_available_checked(pos, 1, len, "VL prefix", error))
        return false;

    uint8_t byte1 = data[pos++];
    if (byte1 <= 192) {
        field_size = byte1;
        return true;
    }

    if (byte1 <= 240) {
        if (!require_available_checked(pos, 1, len, "VL prefix", error))
            return false;
        uint8_t byte2 = data[pos++];
        field_size = 193 + ((byte1 - 193) * 256) + byte2;
        return true;
    }

    if (byte1 <= 254) {
        if (!require_available_checked(pos, 2, len, "VL prefix", error))
            return false;
        uint8_t byte2 = data[pos++];
        uint8_t byte3 = data[pos++];
        field_size =
            12481 + ((byte1 - 241) * 65536) + (byte2 * 256) + byte3;
        return true;
    }

    error = "invalid VL prefix";
    return false;
}

inline bool
issue_size_checked(
    const uint8_t* data,
    size_t len,
    size_t pos,
    size_t& field_size,
    std::string& error)
{
    if (!require_available_checked(pos, 20, len, "Issue field", error))
        return false;

    const uint8_t* first20 = data + pos;
    if (is_xrp_currency(first20)) {
        field_size = 20;
        return true;
    }

    if (!require_available_checked(pos, 40, len, "Issue field", error))
        return false;

    const uint8_t* second20 = first20 + 20;
    if (is_no_account(second20)) {
        if (!require_available_checked(pos, 44, len, "MPT Issue field", error))
            return false;
        field_size = 44;
        return true;
    }

    field_size = 40;
    return true;
}

inline bool
scan_object_checked(
    const uint8_t* data,
    size_t len,
    const Protocol& proto,
    size_t& pos,
    std::vector<FieldOffset>* offsets,
    std::string& error);

inline bool
scan_pathset_checked(
    const uint8_t* data,
    size_t len,
    size_t& pos,
    std::string& error)
{
    while (pos < len) {
        uint8_t type_byte = data[pos++];
        if (type_byte == PathSet::END_BYTE) return true;
        if (type_byte == PathSet::PATH_SEPARATOR) continue;

        if ((type_byte & PathSet::TYPE_ACCOUNT) &&
            !require_available_checked(pos, 20, len, "PathSet account", error))
            return false;
        if (type_byte & PathSet::TYPE_ACCOUNT) pos += 20;

        if ((type_byte & PathSet::TYPE_CURRENCY) &&
            !require_available_checked(pos, 20, len, "PathSet currency", error))
            return false;
        if (type_byte & PathSet::TYPE_CURRENCY) pos += 20;

        if ((type_byte & PathSet::TYPE_ISSUER) &&
            !require_available_checked(pos, 20, len, "PathSet issuer", error))
            return false;
        if (type_byte & PathSet::TYPE_ISSUER) pos += 20;
    }

    return true;
}

inline bool
scan_xchain_bridge_checked(
    const uint8_t* data,
    size_t len,
    size_t pos,
    size_t& field_size,
    std::string& error)
{
    size_t start = pos;
    size_t issue_size = 0;

    if (!require_available_checked(
            pos, 1, len, "XChainBridge LockingChainDoor", error))
        return false;
    size_t locking_door_len = data[pos++];
    if (!require_available_checked(
            pos, locking_door_len, len, "XChainBridge LockingChainDoor", error))
        return false;
    pos += locking_door_len;

    if (!issue_size_checked(data, len, pos, issue_size, error))
        return false;
    pos += issue_size;

    if (!require_available_checked(
            pos, 1, len, "XChainBridge IssuingChainDoor", error))
        return false;
    size_t issuing_door_len = data[pos++];
    if (!require_available_checked(
            pos, issuing_door_len, len, "XChainBridge IssuingChainDoor", error))
        return false;
    pos += issuing_door_len;

    if (!issue_size_checked(data, len, pos, issue_size, error))
        return false;
    pos += issue_size;

    field_size = pos - start;
    return true;
}

inline bool
scan_array_checked(
    const uint8_t* data,
    size_t len,
    const Protocol& proto,
    size_t& pos,
    std::string& error)
{
    while (pos < len) {
        uint32_t field_code = 0;
        if (!read_field_header_checked(data, len, pos, field_code, error))
            return false;
        if (field_code == 0) return true;

        const FieldDef* field = proto.get_field_by_code(field_code);
        if (!field) {
            error = "unknown field code " + std::to_string(field_code);
            return false;
        }
        if (is_array_end_marker(field)) return true;

        if (field->meta.type != FieldTypes::STObject) {
            error = "array element is not an STObject";
            return false;
        }

        if (!scan_object_checked(data, len, proto, pos, nullptr, error))
            return false;
    }

    return true;
}

inline bool
scan_object_checked(
    const uint8_t* data,
    size_t len,
    const Protocol& proto,
    size_t& pos,
    std::vector<FieldOffset>* offsets,
    std::string& error)
{
    while (pos < len) {
        //@@start scan-stops
        uint32_t field_code = 0;
        if (!read_field_header_checked(data, len, pos, field_code, error))
            return false;
        if (field_code == 0) return true;

        const FieldDef* field = proto.get_field_by_code(field_code);
        if (!field) {
            // Validated means whole-buffer. A code we do not recognise is an
            // error, not the object quietly ending here — otherwise a
            // truncation whose tail parses as a header decodes clean and short.
            error = "unknown field code " + std::to_string(field_code);
            return false;
        }

        if (is_object_end_marker(field) || is_array_end_marker(field))
            return true;
        //@@end scan-stops

        size_t data_start = pos;
        size_t field_size = 0;

        if (field->meta.is_vl_encoded) {
            if (!read_vl_length_checked(data, len, pos, field_size, error))
                return false;
            data_start = pos;
            if (!require_available_checked(
                    pos, field_size, len, field->name.c_str(), error))
                return false;
            pos += field_size;
        }
        else if (field->meta.type == FieldTypes::STObject) {
            if (!scan_object_checked(data, len, proto, pos, nullptr, error))
                return false;
            field_size = pos - data_start;
        }
        else if (field->meta.type == FieldTypes::STArray) {
            if (!scan_array_checked(data, len, proto, pos, error))
                return false;
            field_size = pos - data_start;
        }
        else if (field->meta.type == FieldTypes::Amount) {
            if (!require_available_checked(
                    pos, 1, len, field->name.c_str(), error))
                return false;
            field_size = get_amount_size(data[pos]);
            if (!require_available_checked(
                    pos, field_size, len, field->name.c_str(), error))
                return false;
            pos += field_size;
        }
        else if (field->meta.type == FieldTypes::Issue) {
            if (!issue_size_checked(data, len, pos, field_size, error))
                return false;
            pos += field_size;
        }
        else if (field->meta.type == FieldTypes::PathSet) {
            if (!scan_pathset_checked(data, len, pos, error))
                return false;
            field_size = pos - data_start;
        }
        else if (field->meta.type == FieldTypes::XChainBridge) {
            if (!scan_xchain_bridge_checked(data, len, pos, field_size, error))
                return false;
            pos += field_size;
        }
        else {
            field_size = field->meta.type.fixed_size;
            if (field_size == 0) {
                error = "field " + field->name + " has no fixed size";
                return false;
            }
            if (!require_available_checked(
                    pos, field_size, len, field->name.c_str(), error))
                return false;
            pos += field_size;
        }

        if (offsets) {
            offsets->push_back({
                .field = field,
                .offset = static_cast<uint32_t>(data_start),
                .length = static_cast<uint32_t>(field_size),
            });
        }
    }

    return true;
}

inline std::expected<std::vector<FieldOffset>, CodecErrorValue>
build_field_offsets_expected(
    const uint8_t* data,
    size_t len,
    const Protocol& proto)
{
    std::vector<FieldOffset> offsets;
    offsets.reserve(16);

    size_t pos = 0;
    std::string error;
    if (!scan_object_checked(data, len, proto, pos, &offsets, error))
        return std::unexpected(decode_scan_error(error));

    return offsets;
}

inline std::vector<FieldOffset>
build_field_offsets(const uint8_t* data, size_t len, const Protocol& proto)
{
    return decode_or_throw(
        build_field_offsets_expected(data, len, proto));
}

inline std::expected<std::vector<FieldOffset>, CodecErrorValue>
build_array_element_offsets_expected(
    const uint8_t* data,
    size_t len,
    const Protocol& proto)
{
    std::vector<FieldOffset> offsets;

    size_t pos = 0;
    std::string error;
    while (pos < len) {
        uint32_t field_code = 0;
        if (!read_field_header_checked(data, len, pos, field_code, error))
            return std::unexpected(decode_scan_error(error));
        if (field_code == 0) break;

        const FieldDef* field = proto.get_field_by_code(field_code);
        if (!field) {
            return std::unexpected(decode_scan_error(
                "unknown field code " + std::to_string(field_code)));
        }
        if (is_array_end_marker(field)) break;

        if (field->meta.type != FieldTypes::STObject) {
            return std::unexpected(
                decode_scan_error("array element is not an STObject"));
        }

        size_t elem_start = pos;
        if (!scan_object_checked(data, len, proto, pos, nullptr, error))
            return std::unexpected(decode_scan_error(error));
        size_t elem_len = pos - elem_start;

        offsets.push_back({
            .field = field,
            .offset = static_cast<uint32_t>(elem_start),
            .length = static_cast<uint32_t>(elem_len),
        });
    }

    return offsets;
}

// ---- Leaf field decode: Slice → JSValue (no boost::json) ----

inline JSValue
decode_field_value_js(
    JSContext* ctx,
    FieldDef const& field,
    Slice const& data,
    Protocol const& protocol)
{
    auto const& t = field.meta.type;

    if (field.code == codecs::EnumFieldCodes::TransactionType) {
        uint16_t raw = codecs::UInt16Codec::decode_raw(data);
        if (auto name = protocol.get_transaction_type_name(raw))
            return JS_NewStringLen(ctx, name->data(), name->size());
        return JS_NewUint32(ctx, raw);
    }
    if (field.code == codecs::EnumFieldCodes::LedgerEntryType) {
        uint16_t raw = codecs::UInt16Codec::decode_raw(data);
        if (auto name = protocol.get_ledger_entry_type_name(raw))
            return JS_NewStringLen(ctx, name->data(), name->size());
        return JS_NewUint32(ctx, raw);
    }
    if (field.code == codecs::EnumFieldCodes::TransactionResult) {
        int32_t code = static_cast<int32_t>(codecs::UInt8Codec::decode_raw(data));
        for (auto const& [name, c] : protocol.transactionResults()) {
            if (c == code)
                return JS_NewStringLen(ctx, name.data(), name.size());
        }
        return JS_NewInt32(ctx, code);
    }
    if (field.code == codecs::EnumFieldCodes::PermissionValue) {
        uint32_t raw = codecs::UInt32Codec::decode_raw(data);
        for (auto const& [name, code] : protocol.permissions()) {
            if (code == raw)
                return JS_NewStringLen(ctx, name.data(), name.size());
        }
        return JS_NewUint32(ctx, raw);
    }

    // Integer types → JS number
    if (t == FieldTypes::UInt8)
        return JS_NewInt32(ctx, codecs::UInt8Codec::decode_raw(data));
    if (t == FieldTypes::UInt16)
        return JS_NewInt32(ctx, codecs::UInt16Codec::decode_raw(data));
    if (t == FieldTypes::UInt32)
        return JS_NewUint32(ctx, codecs::UInt32Codec::decode_raw(data));
    if (t == FieldTypes::UInt64) {
        // UInt64 → hex string (too large for JS number)
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | data.data()[i];
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)v);
        return JS_NewString(ctx, buf);
    }

    // Hash types → hex string
    if (t == FieldTypes::Hash128 || t == FieldTypes::Hash160 ||
        t == FieldTypes::Hash192 || t == FieldTypes::Hash256 ||
        t == FieldTypes::UInt96 || t == FieldTypes::UInt384 ||
        t == FieldTypes::UInt512) {
        std::string hex = hex_encode(data);
        return JS_NewStringLen(ctx, hex.c_str(), hex.size());
    }

    // Amount → string (native) or object (IOU/MPT)
    if (t == FieldTypes::Amount) {
        if (is_native_amount(data)) {
            auto s = parse_native_drops_string(data);
            return JS_NewString(ctx, s.c_str());
        }
        if (data.size() == 48) {
            // IOU
            auto iou = IOUValue::from_bytes(data.data());
            Slice currency_slice = get_currency_raw(data);
            std::string issuer = base58::encode_account_id(data.data() + 28, 20);

            std::string currency_str =
                codecs::CurrencyCodec::decode_string(currency_slice);

            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "currency",
                JS_NewStringLen(ctx, currency_str.c_str(), currency_str.size()));
            auto val_str = iou.to_string();
            JS_SetPropertyStr(ctx, obj, "value",
                JS_NewStringLen(ctx, val_str.c_str(), val_str.size()));
            JS_SetPropertyStr(ctx, obj, "issuer",
                JS_NewStringLen(ctx, issuer.c_str(), issuer.size()));
            return obj;
        }
        if (data.size() == 33) {
            // MPT
            uint64_t val = 0;
            for (int i = 1; i < 9; ++i) val = (val << 8) | data.data()[i];
            std::string mptid = hex_encode(data.data() + 9, 24);
            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "value",
                JS_NewString(ctx, std::to_string(val).c_str()));
            JS_SetPropertyStr(ctx, obj, "mpt_issuance_id",
                JS_NewStringLen(ctx, mptid.c_str(), mptid.size()));
            return obj;
        }
        std::string hex = hex_encode(data);
        return JS_NewStringLen(ctx, hex.c_str(), hex.size());
    }

    // AccountID → base58 string
    if (t == FieldTypes::AccountID) {
        auto decoded_result = codecs::AccountIDCodec::decode_string_expected(data);
        if (!decoded_result) {
            return JS_ThrowTypeError(
                ctx,
                "decode_object failed: %s",
                decoded_result.error().message.c_str());
        }
        auto const& s = *decoded_result;
        return JS_NewStringLen(ctx, s.c_str(), s.size());
    }

    if (t == FieldTypes::Currency) {
        std::string s = codecs::CurrencyCodec::decode_string(data);
        return JS_NewStringLen(ctx, s.c_str(), s.size());
    }

    if (t == FieldTypes::Issue) {
        if (data.size() < 20) {
            std::string hex = hex_encode(data);
            return JS_NewStringLen(ctx, hex.c_str(), hex.size());
        }
        Slice first20(data.data(), 20);
        if (is_xrp_currency(first20) || data.size() < 40) {
            std::string s = codecs::CurrencyCodec::decode_string(first20);
            return JS_NewStringLen(ctx, s.c_str(), s.size());
        }
        Slice second20(data.data() + 20, 20);
        if (std::memcmp(second20.data(), codecs::NO_ACCOUNT, 20) == 0 &&
            data.size() >= 44)
        {
            uint32_t seq_be = (static_cast<uint32_t>(data.data()[40]) << 24) |
                              (static_cast<uint32_t>(data.data()[41]) << 16) |
                              (static_cast<uint32_t>(data.data()[42]) << 8) |
                              static_cast<uint32_t>(data.data()[43]);
            uint8_t mptid[24];
            std::memcpy(mptid, &seq_be, 4);
            std::memcpy(mptid + 4, data.data(), 20);
            std::string hex = hex_encode(mptid, 24);
            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "mpt_issuance_id",
                JS_NewStringLen(ctx, hex.c_str(), hex.size()));
            return obj;
        }
        std::string currency = codecs::CurrencyCodec::decode_string(first20);
        auto issuer = codecs::AccountIDCodec::decode_string_expected(second20);
        if (!issuer) {
            return JS_ThrowTypeError(
                ctx, "decode_object failed: %s", issuer.error().message.c_str());
        }
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "currency",
            JS_NewStringLen(ctx, currency.c_str(), currency.size()));
        JS_SetPropertyStr(ctx, obj, "issuer",
            JS_NewStringLen(ctx, issuer->c_str(), issuer->size()));
        return obj;
    }

    // Blob → hex string
    if (t == FieldTypes::Blob) {
        std::string hex = hex_encode(data);
        return JS_NewStringLen(ctx, hex.c_str(), hex.size());
    }

    if (t == FieldTypes::Number) {
        std::string s = parse_number(data).to_string();
        return JS_NewStringLen(ctx, s.c_str(), s.size());
    }

    if (t == FieldTypes::Int32)
        return JS_NewInt32(ctx, codecs::Int32Codec::decode_raw(data));
    if (t == FieldTypes::Int64)
        return JS_NewInt64(ctx, codecs::Int64Codec::decode_raw(data));

    if (t == FieldTypes::PathSet) {
        JSValue paths = JS_NewArray(ctx);
        uint32_t path_i = 0;
        JSValue current = JS_NewArray(ctx);
        uint32_t hop_i = 0;
        size_t pos = 0;
        while (pos < data.size()) {
            uint8_t type_byte = data.data()[pos++];
            if (type_byte == PathSet::END_BYTE) {
                if (hop_i > 0)
                    JS_SetPropertyUint32(ctx, paths, path_i++, current);
                else
                    JS_FreeValue(ctx, current);
                return paths;
            }
            if (type_byte == PathSet::PATH_SEPARATOR) {
                if (hop_i > 0) {
                    JS_SetPropertyUint32(ctx, paths, path_i++, current);
                    current = JS_NewArray(ctx);
                    hop_i = 0;
                }
                continue;
            }
            JSValue hop = JS_NewObject(ctx);
            bool any = false;
            if ((type_byte & PathSet::TYPE_ACCOUNT) && pos + 20 <= data.size()) {
                auto acc = codecs::AccountIDCodec::decode_string_expected(
                    Slice(data.data() + pos, 20));
                pos += 20;
                if (acc) {
                    JS_SetPropertyStr(ctx, hop, "account",
                        JS_NewStringLen(ctx, acc->c_str(), acc->size()));
                    any = true;
                }
            }
            if ((type_byte & PathSet::TYPE_CURRENCY) && pos + 20 <= data.size()) {
                std::string cur = codecs::CurrencyCodec::decode_string(
                    Slice(data.data() + pos, 20));
                pos += 20;
                JS_SetPropertyStr(ctx, hop, "currency",
                    JS_NewStringLen(ctx, cur.c_str(), cur.size()));
                any = true;
            }
            if ((type_byte & PathSet::TYPE_ISSUER) && pos + 20 <= data.size()) {
                auto iss = codecs::AccountIDCodec::decode_string_expected(
                    Slice(data.data() + pos, 20));
                pos += 20;
                if (iss) {
                    JS_SetPropertyStr(ctx, hop, "issuer",
                        JS_NewStringLen(ctx, iss->c_str(), iss->size()));
                    any = true;
                }
            }
            if (any)
                JS_SetPropertyUint32(ctx, current, hop_i++, hop);
            else
                JS_FreeValue(ctx, hop);
        }
        if (hop_i > 0)
            JS_SetPropertyUint32(ctx, paths, path_i, current);
        else
            JS_FreeValue(ctx, current);
        return paths;
    }

    if (t == FieldTypes::XChainBridge) {
        size_t pos = 0;
        JSValue obj = JS_NewObject(ctx);

        auto take_account = [&](char const* key) -> bool {
            if (pos >= data.size() ||
                pos + 1 + data.data()[pos] > data.size())
            {
                JS_FreeValue(ctx, obj);
                JS_ThrowTypeError(
                    ctx,
                    "decode_object failed: truncated XChainBridge %s",
                    key);
                return false;
            }
            size_t vl = data.data()[pos++];
            auto acc = codecs::AccountIDCodec::decode_string_expected(
                Slice(data.data() + pos, vl));
            pos += vl;
            if (!acc) {
                JS_FreeValue(ctx, obj);
                JS_ThrowTypeError(
                    ctx,
                    "decode_object failed: %s",
                    acc.error().message.c_str());
                return false;
            }
            JS_SetPropertyStr(ctx, obj, key,
                JS_NewStringLen(ctx, acc->c_str(), acc->size()));
            return true;
        };

        auto take_issue = [&](char const* key) -> bool {
            size_t issue_size = 0;
            std::string error;
            if (!issue_size_checked(
                    data.data(), data.size(), pos, issue_size, error))
            {
                JS_FreeValue(ctx, obj);
                JS_ThrowTypeError(
                    ctx, "decode_object failed: %s", error.c_str());
                return false;
            }
            FieldDef issue_field = field;
            issue_field.meta.type = FieldTypes::Issue;
            JSValue issue = decode_field_value_js(
                ctx, issue_field,
                Slice(data.data() + pos, issue_size), protocol);
            if (JS_IsException(issue)) {
                JS_FreeValue(ctx, obj);
                return false;
            }
            pos += issue_size;
            JS_SetPropertyStr(ctx, obj, key, issue);
            return true;
        };

        if (!take_account("LockingChainDoor"))
            return JS_EXCEPTION;
        if (!take_issue("LockingChainIssue"))
            return JS_EXCEPTION;
        if (!take_account("IssuingChainDoor"))
            return JS_EXCEPTION;
        if (!take_issue("IssuingChainIssue"))
            return JS_EXCEPTION;
        return obj;
    }

    // Vector256 → array of hex strings
    if (t == FieldTypes::Vector256) {
        JSValue arr = JS_NewArray(ctx);
        size_t count = data.size() / 32;
        for (size_t i = 0; i < count; ++i) {
            std::string hex = hex_encode(data.data() + i * 32, 32);
            JS_SetPropertyUint32(ctx, arr, i,
                JS_NewStringLen(ctx, hex.c_str(), hex.size()));
        }
        return arr;
    }

    std::string hex = hex_encode(data);
    return JS_NewStringLen(ctx, hex.c_str(), hex.size());
}

// ---- QjsVisitor: builds JSValue tree directly ----

class QjsVisitor
{
public:
    explicit QjsVisitor(JSContext* ctx, const Protocol& protocol)
        : ctx_(ctx), protocol_(protocol)
    {
    }

    bool visit_object_start(const FieldPath& path, const FieldSlice& fs) {
        // Root object and named inner objects get STObject prototype
        JSValue obj = (path.empty())
            ? STObjectClass::new_instance(ctx_)
            : JS_NewObject(ctx_);
        stack_.push(obj);
        return true;
    }

    void visit_object_end(const FieldPath& path, const FieldSlice& fs) {
        if (stack_.empty()) return;

        JSValue completed = stack_.top();
        stack_.pop();

        if (path.empty() && stack_.empty()) {
            result_ = completed;
            return;
        }

        if (!stack_.empty()) {
            const auto& field = fs.get_field();
            JSValue parent = stack_.top();

            if (JS_IsArray(ctx_, parent)) {
                // Array element: wrap in {FieldName: obj}
                if (field.meta.type == FieldTypes::STObject) {
                    JSValue wrapper = JS_NewObject(ctx_);
                    JS_SetPropertyStr(ctx_, wrapper, field.name.c_str(), completed);
                    uint32_t len;
                    JSValue len_val = JS_GetPropertyStr(ctx_, parent, "length");
                    JS_ToUint32(ctx_, &len, len_val);
                    JS_FreeValue(ctx_, len_val);
                    JS_SetPropertyUint32(ctx_, parent, len, wrapper);
                } else {
                    uint32_t len;
                    JSValue len_val = JS_GetPropertyStr(ctx_, parent, "length");
                    JS_ToUint32(ctx_, &len, len_val);
                    JS_FreeValue(ctx_, len_val);
                    JS_SetPropertyUint32(ctx_, parent, len, completed);
                }
            } else {
                JS_SetPropertyStr(ctx_, parent, field.name.c_str(), completed);
            }
        } else {
            JS_FreeValue(ctx_, completed);
        }
    }

    bool visit_array_start(const FieldPath& path, const FieldSlice& fs) {
        stack_.push(JS_NewArray(ctx_));
        return true;
    }

    void visit_array_end(const FieldPath& path, const FieldSlice& fs) {
        if (stack_.empty()) return;

        JSValue completed = stack_.top();
        stack_.pop();

        if (!stack_.empty()) {
            const auto& field = fs.get_field();
            JSValue parent = stack_.top();
            if (JS_IsObject(parent)) {
                JS_SetPropertyStr(ctx_, parent, field.name.c_str(), completed);
            }
        } else {
            JS_FreeValue(ctx_, completed);
        }
    }

    void visit_field(const FieldPath& path, const FieldSlice& fs) {
        const auto& field = fs.get_field();

        if (stack_.empty()) {
            stack_.push(STObjectClass::new_instance(ctx_));
        }

        JSValue field_value = decode_field_value_js(ctx_, field, fs.data, protocol_);
        JSValue parent = stack_.top();

        if (JS_IsObject(parent)) {
            // Use set_field for STObjects (writes to internal cache)
            // Falls back to JS_SetPropertyStr for plain objects
            STObjectClass::set_field(ctx_, parent, field.name.c_str(), field_value);
        }
    }

    JSValue get_result() {
        if (!stack_.empty()) {
            JSValue v = stack_.top();
            stack_.pop();
            return v;  // Caller takes ownership
        }
        if (!JS_IsUndefined(result_)) return result_;  // Caller takes ownership
        return JS_NewObject(ctx_);
    }

private:
    JSContext* ctx_;
    const Protocol& protocol_;
    std::stack<JSValue> stack_;
    JSValue result_ = JS_UNDEFINED;
};

}  // namespace catl::xdata
