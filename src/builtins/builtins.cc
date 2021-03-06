// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/builtins/builtins.h"
#include "src/api.h"
#include "src/assembler-inl.h"
#include "src/builtins/builtins-descriptors.h"
#include "src/callable.h"
#include "src/code-events.h"
#include "src/compiler/code-assembler.h"
#include "src/ic/ic-state.h"
#include "src/interface-descriptors.h"
#include "src/isolate.h"
#include "src/macro-assembler.h"
#include "src/objects-inl.h"

namespace v8 {
namespace internal {

// Forward declarations for C++ builtins.
#define FORWARD_DECLARE(Name) \
  Object* Builtin_##Name(int argc, Object** args, Isolate* isolate);
BUILTIN_LIST_C(FORWARD_DECLARE)

Builtins::Builtins() : initialized_(false) {
  memset(builtins_, 0, sizeof(builtins_[0]) * builtin_count);
}

Builtins::~Builtins() {}

namespace {
void PostBuildProfileAndTracing(Isolate* isolate, Code* code,
                                const char* name) {
  PROFILE(isolate, CodeCreateEvent(CodeEventListener::BUILTIN_TAG,
                                   AbstractCode::cast(code), name));
#ifdef ENABLE_DISASSEMBLER
  if (FLAG_print_builtin_code) {
    CodeTracer::Scope trace_scope(isolate->GetCodeTracer());
    OFStream os(trace_scope.file());
    os << "Builtin: " << name << "\n";
    code->Disassemble(name, os);
    os << "\n";
  }
#endif
}

typedef void (*MacroAssemblerGenerator)(MacroAssembler*);
typedef void (*CodeAssemblerGenerator)(compiler::CodeAssemblerState*);

Code* BuildWithMacroAssembler(Isolate* isolate,
                              MacroAssemblerGenerator generator,
                              Code::Flags flags, const char* s_name) {
  HandleScope scope(isolate);
  const size_t buffer_size = 32 * KB;
  byte buffer[buffer_size];  // NOLINT(runtime/arrays)
  MacroAssembler masm(isolate, buffer, buffer_size, CodeObjectRequired::kYes);
  DCHECK(!masm.has_frame());
  generator(&masm);
  CodeDesc desc;
  masm.GetCode(&desc);
  Handle<Code> code =
      isolate->factory()->NewCode(desc, flags, masm.CodeObject());
  PostBuildProfileAndTracing(isolate, *code, s_name);
  return *code;
}

Code* BuildAdaptor(Isolate* isolate, Address builtin_address,
                   Builtins::ExitFrameType exit_frame_type, Code::Flags flags,
                   const char* name) {
  HandleScope scope(isolate);
  const size_t buffer_size = 32 * KB;
  byte buffer[buffer_size];  // NOLINT(runtime/arrays)
  MacroAssembler masm(isolate, buffer, buffer_size, CodeObjectRequired::kYes);
  DCHECK(!masm.has_frame());
  Builtins::Generate_Adaptor(&masm, builtin_address, exit_frame_type);
  CodeDesc desc;
  masm.GetCode(&desc);
  Handle<Code> code =
      isolate->factory()->NewCode(desc, flags, masm.CodeObject());
  PostBuildProfileAndTracing(isolate, *code, name);
  return *code;
}

// Builder for builtins implemented in TurboFan with JS linkage.
Code* BuildWithCodeStubAssemblerJS(Isolate* isolate,
                                   CodeAssemblerGenerator generator, int argc,
                                   Code::Flags flags, const char* name) {
  HandleScope scope(isolate);
  Zone zone(isolate->allocator(), ZONE_NAME);
  const int argc_with_recv =
      (argc == SharedFunctionInfo::kDontAdaptArgumentsSentinel) ? 0 : argc + 1;
  compiler::CodeAssemblerState state(isolate, &zone, argc_with_recv, flags,
                                     name);
  generator(&state);
  Handle<Code> code = compiler::CodeAssembler::GenerateCode(&state);
  PostBuildProfileAndTracing(isolate, *code, name);
  return *code;
}

// Builder for builtins implemented in TurboFan with CallStub linkage.
Code* BuildWithCodeStubAssemblerCS(Isolate* isolate,
                                   CodeAssemblerGenerator generator,
                                   CallDescriptors::Key interface_descriptor,
                                   Code::Flags flags, const char* name,
                                   int result_size) {
  HandleScope scope(isolate);
  Zone zone(isolate->allocator(), ZONE_NAME);
  // The interface descriptor with given key must be initialized at this point
  // and this construction just queries the details from the descriptors table.
  CallInterfaceDescriptor descriptor(isolate, interface_descriptor);
  // Ensure descriptor is already initialized.
  DCHECK_LE(0, descriptor.GetRegisterParameterCount());
  compiler::CodeAssemblerState state(isolate, &zone, descriptor, flags, name,
                                     result_size);
  generator(&state);
  Handle<Code> code = compiler::CodeAssembler::GenerateCode(&state);
  PostBuildProfileAndTracing(isolate, *code, name);
  return *code;
}
}  // anonymous namespace

void Builtins::SetUp(Isolate* isolate, bool create_heap_objects) {
  DCHECK(!initialized_);

  // Create a scope for the handles in the builtins.
  HandleScope scope(isolate);

  if (create_heap_objects) {
    int index = 0;
    const Code::Flags kBuiltinFlags = Code::ComputeFlags(Code::BUILTIN);
    Code* code;
#define BUILD_CPP(Name)                                                     \
  code = BuildAdaptor(isolate, FUNCTION_ADDR(Builtin_##Name), BUILTIN_EXIT, \
                      kBuiltinFlags, #Name);                                \
  builtins_[index++] = code;
#define BUILD_API(Name)                                             \
  code = BuildAdaptor(isolate, FUNCTION_ADDR(Builtin_##Name), EXIT, \
                      kBuiltinFlags, #Name);                        \
  builtins_[index++] = code;
#define BUILD_TFJ(Name, Argc, ...)                                     \
  code = BuildWithCodeStubAssemblerJS(isolate, &Generate_##Name, Argc, \
                                      kBuiltinFlags, #Name);           \
  builtins_[index++] = code;
#define BUILD_TFS(Name, InterfaceDescriptor, result_size)                   \
  { InterfaceDescriptor##Descriptor descriptor(isolate); }                  \
  code = BuildWithCodeStubAssemblerCS(isolate, &Generate_##Name,            \
                                      CallDescriptors::InterfaceDescriptor, \
                                      kBuiltinFlags, #Name, result_size);   \
  builtins_[index++] = code;
#define BUILD_TFH(Name, Kind, Extra, InterfaceDescriptor)              \
  { InterfaceDescriptor##Descriptor descriptor(isolate); }             \
  /* Return size for IC builtins/handlers is always 1. */              \
  code = BuildWithCodeStubAssemblerCS(                                 \
      isolate, &Generate_##Name, CallDescriptors::InterfaceDescriptor, \
      Code::ComputeFlags(Code::Kind, Extra), #Name, 1);                \
  builtins_[index++] = code;
#define BUILD_ASM(Name)                                                        \
  code =                                                                       \
      BuildWithMacroAssembler(isolate, Generate_##Name, kBuiltinFlags, #Name); \
  builtins_[index++] = code;

    BUILTIN_LIST(BUILD_CPP, BUILD_API, BUILD_TFJ, BUILD_TFS, BUILD_TFH,
                 BUILD_ASM, BUILD_ASM);

#undef BUILD_CPP
#undef BUILD_API
#undef BUILD_TFJ
#undef BUILD_TFS
#undef BUILD_TFH
#undef BUILD_ASM
    CHECK_EQ(builtin_count, index);
    for (int i = 0; i < builtin_count; i++) {
      Code::cast(builtins_[i])->set_builtin_index(i);
    }

#define SET_PROMISE_REJECTION_PREDICTION(Name) \
  Code::cast(builtins_[k##Name])->set_is_promise_rejection(true);

    BUILTIN_PROMISE_REJECTION_PREDICTION_LIST(SET_PROMISE_REJECTION_PREDICTION)
#undef SET_PROMISE_REJECTION_PREDICTION

#define SET_EXCEPTION_CAUGHT_PREDICTION(Name) \
  Code::cast(builtins_[k##Name])->set_is_exception_caught(true);

    BUILTIN_EXCEPTION_CAUGHT_PREDICTION_LIST(SET_EXCEPTION_CAUGHT_PREDICTION)
#undef SET_EXCEPTION_CAUGHT_PREDICTION

#define SET_CODE_NON_TAGGED_PARAMS(Name) \
  Code::cast(builtins_[k##Name])->set_has_tagged_params(false);
    BUILTINS_WITH_UNTAGGED_PARAMS(SET_CODE_NON_TAGGED_PARAMS)
#undef SET_CODE_NON_TAGGED_PARAMS
  }

  // Mark as initialized.
  initialized_ = true;
}

void Builtins::TearDown() { initialized_ = false; }

void Builtins::IterateBuiltins(ObjectVisitor* v) {
  v->VisitPointers(&builtins_[0], &builtins_[0] + builtin_count);
}

const char* Builtins::Lookup(byte* pc) {
  // may be called during initialization (disassembler!)
  if (initialized_) {
    for (int i = 0; i < builtin_count; i++) {
      Code* entry = Code::cast(builtins_[i]);
      if (entry->contains(pc)) return name(i);
    }
  }
  return NULL;
}

Handle<Code> Builtins::NewFunctionContext(ScopeType scope_type) {
  switch (scope_type) {
    case ScopeType::EVAL_SCOPE:
      return FastNewFunctionContextEval();
    case ScopeType::FUNCTION_SCOPE:
      return FastNewFunctionContextFunction();
    default:
      UNREACHABLE();
  }
  return Handle<Code>::null();
}

Handle<Code> Builtins::NewCloneShallowArray(
    AllocationSiteMode allocation_mode) {
  switch (allocation_mode) {
    case TRACK_ALLOCATION_SITE:
      return FastCloneShallowArrayTrack();
    case DONT_TRACK_ALLOCATION_SITE:
      return FastCloneShallowArrayDontTrack();
    default:
      UNREACHABLE();
  }
  return Handle<Code>::null();
}

Handle<Code> Builtins::NewCloneShallowObject(int length) {
  switch (length) {
    case 0:
      return FastCloneShallowObject0();
    case 1:
      return FastCloneShallowObject1();
    case 2:
      return FastCloneShallowObject2();
    case 3:
      return FastCloneShallowObject3();
    case 4:
      return FastCloneShallowObject4();
    case 5:
      return FastCloneShallowObject5();
    case 6:
      return FastCloneShallowObject6();
    default:
      UNREACHABLE();
  }
  return Handle<Code>::null();
}

Handle<Code> Builtins::NonPrimitiveToPrimitive(ToPrimitiveHint hint) {
  switch (hint) {
    case ToPrimitiveHint::kDefault:
      return NonPrimitiveToPrimitive_Default();
    case ToPrimitiveHint::kNumber:
      return NonPrimitiveToPrimitive_Number();
    case ToPrimitiveHint::kString:
      return NonPrimitiveToPrimitive_String();
  }
  UNREACHABLE();
  return Handle<Code>::null();
}

Handle<Code> Builtins::OrdinaryToPrimitive(OrdinaryToPrimitiveHint hint) {
  switch (hint) {
    case OrdinaryToPrimitiveHint::kNumber:
      return OrdinaryToPrimitive_Number();
    case OrdinaryToPrimitiveHint::kString:
      return OrdinaryToPrimitive_String();
  }
  UNREACHABLE();
  return Handle<Code>::null();
}

// static
Callable Builtins::CallableFor(Isolate* isolate, Name name) {
  switch (name) {
#define CASE(Name, ...)                                                  \
  case k##Name: {                                                        \
    Handle<Code> code(Code::cast(isolate->builtins()->builtins_[name])); \
    auto descriptor = Builtin_##Name##_InterfaceDescriptor(isolate);     \
    return Callable(code, descriptor);                                   \
  }
    BUILTIN_LIST(IGNORE_BUILTIN, IGNORE_BUILTIN, IGNORE_BUILTIN, CASE, CASE,
                 IGNORE_BUILTIN, IGNORE_BUILTIN)
#undef CASE
    default:
      UNREACHABLE();
      return Callable(Handle<Code>::null(), VoidDescriptor(isolate));
  }
}

// static
const char* Builtins::name(int index) {
  switch (index) {
#define CASE(Name, ...) \
  case k##Name:         \
    return #Name;
    BUILTIN_LIST_ALL(CASE)
#undef CASE
    default:
      UNREACHABLE();
      break;
  }
  return "";
}

// static
Address Builtins::CppEntryOf(int index) {
  DCHECK(0 <= index && index < builtin_count);
  switch (index) {
#define CASE(Name, ...) \
  case k##Name:         \
    return FUNCTION_ADDR(Builtin_##Name);
    BUILTIN_LIST_C(CASE)
#undef CASE
    default:
      return nullptr;
  }
  UNREACHABLE();
}

// static
bool Builtins::IsCpp(int index) {
  DCHECK(0 <= index && index < builtin_count);
  switch (index) {
#define CASE(Name, ...) \
  case k##Name:         \
    return true;
#define BUILTIN_LIST_CPP(V)                                       \
  BUILTIN_LIST(V, IGNORE_BUILTIN, IGNORE_BUILTIN, IGNORE_BUILTIN, \
               IGNORE_BUILTIN, IGNORE_BUILTIN, IGNORE_BUILTIN)
    BUILTIN_LIST_CPP(CASE)
#undef BUILTIN_LIST_CPP
#undef CASE
    default:
      return false;
  }
  UNREACHABLE();
}

// static
bool Builtins::IsApi(int index) {
  DCHECK(0 <= index && index < builtin_count);
  switch (index) {
#define CASE(Name, ...) \
  case k##Name:         \
    return true;
#define BUILTIN_LIST_API(V)                                       \
  BUILTIN_LIST(IGNORE_BUILTIN, V, IGNORE_BUILTIN, IGNORE_BUILTIN, \
               IGNORE_BUILTIN, IGNORE_BUILTIN, IGNORE_BUILTIN)
    BUILTIN_LIST_API(CASE);
#undef BUILTIN_LIST_API
#undef CASE
    default:
      return false;
  }
  UNREACHABLE();
}

// static
bool Builtins::HasCppImplementation(int index) {
  DCHECK(0 <= index && index < builtin_count);
  switch (index) {
#define CASE(Name, ...) \
  case k##Name:         \
    return true;
    BUILTIN_LIST_C(CASE)
#undef CASE
    default:
      return false;
  }
  UNREACHABLE();
}

#define DEFINE_BUILTIN_ACCESSOR(Name, ...)                                    \
  Handle<Code> Builtins::Name() {                                             \
    Code** code_address = reinterpret_cast<Code**>(builtin_address(k##Name)); \
    return Handle<Code>(code_address);                                        \
  }
BUILTIN_LIST_ALL(DEFINE_BUILTIN_ACCESSOR)
#undef DEFINE_BUILTIN_ACCESSOR

// static
bool Builtins::AllowDynamicFunction(Isolate* isolate, Handle<JSFunction> target,
                                    Handle<JSObject> target_global_proxy) {
  if (FLAG_allow_unsafe_function_constructor) return true;
  HandleScopeImplementer* impl = isolate->handle_scope_implementer();
  Handle<Context> responsible_context =
      impl->MicrotaskContextIsLastEnteredContext() ? impl->MicrotaskContext()
                                                   : impl->LastEnteredContext();
  // TODO(jochen): Remove this.
  if (responsible_context.is_null()) {
    return true;
  }
  if (*responsible_context == target->context()) return true;
  return isolate->MayAccess(responsible_context, target_global_proxy);
}

}  // namespace internal
}  // namespace v8
