#include "provider_internal.hpp"

#include "generated/enum_namespaces.h"

#include <jshookz/qjs.hpp>

#include <array>
#include <span>

namespace jshookz::provider {
namespace {

constexpr int globalPublicationFlags =
    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE;

bool failRegistration(JSContext *context) {
  if (!JS_HasException(context))
    JS_ThrowOutOfMemory(context);
  return false;
}

using generated::EnumNamespaceMember;
using qjs::OwnedValue;

struct NamespaceSpec {
  char const *name;
  std::span<EnumNamespaceMember const> members;
};

constexpr std::array namespaceSpecs = {
#ifdef CONFIG_XAHAU_CONSENSUS_ENTROPY_PROVIDER
    NamespaceSpec{"EntropyTier", generated::ENTROPY_TIER_MEMBERS},
#endif
    NamespaceSpec{"TransactionType", generated::TRANSACTION_TYPE_MEMBERS},
    NamespaceSpec{"LedgerEntryType", generated::LEDGER_ENTRY_TYPE_MEMBERS},
    NamespaceSpec{"TransactionResult", generated::TRANSACTION_RESULT_MEMBERS},
    NamespaceSpec{"HookReturnCode", generated::HOOK_RETURN_CODE_MEMBERS},
};

JSValue makeNamespace(JSContext *context,
                      std::span<EnumNamespaceMember const> members) {
  OwnedValue object(context, JS_NewObject(context));
  if (object.isException()) {
    failRegistration(context);
    return object.release();
  }
  for (auto const &member : members) {
    if (JS_DefinePropertyValueStr(context, object.get(), member.name,
                                  JS_NewInt64(context, member.value),
                                  JS_PROP_ENUMERABLE) < 0) {
      failRegistration(context);
      return JS_EXCEPTION;
    }
  }
  // Every member already has the final frozen descriptor. Avoid the
  // allocation-heavy JavaScript Object.freeze path; preventing extensions
  // completes the frozen ordinary object and is same-runtime OOM retryable.
  if (JS_PreventExtensions(context, object.get()) < 0)
    return JS_EXCEPTION;
  return object.release();
}

void freeAtoms(JSContext *context,
               std::array<JSAtom, namespaceSpecs.size()> &atoms) {
  for (JSAtom atom : atoms) {
    if (atom != JS_ATOM_NULL)
      JS_FreeAtom(context, atom);
  }
}

} // namespace

bool registerEnumNamespaces(JSContext *context) {
  if (context == nullptr)
    return false;

  OwnedValue global(context, JS_GetGlobalObject(context));
  std::array<OwnedValue, namespaceSpecs.size()> objects = {
#ifdef CONFIG_XAHAU_CONSENSUS_ENTROPY_PROVIDER
      OwnedValue(context),
#endif
      OwnedValue(context), OwnedValue(context), OwnedValue(context),
      OwnedValue(context)};
  std::array<JSAtom, namespaceSpecs.size()> atoms{};
  if (global.isException())
    return failRegistration(context);

  for (std::size_t index = 0; index < namespaceSpecs.size(); ++index) {
    atoms[index] = JS_NewAtom(context, namespaceSpecs[index].name);
    if (atoms[index] == JS_ATOM_NULL) {
      freeAtoms(context, atoms);
      return failRegistration(context);
    }
    objects[index] = OwnedValue(
        context, makeNamespace(context, namespaceSpecs[index].members));
    if (objects[index].isException()) {
      freeAtoms(context, atoms);
      return failRegistration(context);
    }
  }

  for (std::size_t index = 0; index < namespaceSpecs.size(); ++index) {
    if (JS_DefinePropertyValue(context, global.get(), atoms[index],
                               objects[index].release(),
                               globalPublicationFlags) < 0) {
      bool const hadException = JS_HasException(context);
      OwnedValue exception(context, hadException ? JS_GetException(context)
                                                 : JS_UNDEFINED);
      for (std::size_t published = 0; published <= index; ++published)
        JS_DeleteProperty(context, global.get(), atoms[published], 0);
      freeAtoms(context, atoms);
      if (hadException)
        JS_Throw(context, exception.release());
      return failRegistration(context);
    }
  }

  freeAtoms(context, atoms);
  return true;
}

} // namespace jshookz::provider
