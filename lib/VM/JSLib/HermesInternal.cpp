/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSLibInternal.h"

#include "hermes/BCGen/HBC/BytecodeFileFormat.h"
#include "hermes/Support/Base64vlq.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/JSWeakMapImpl.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StringView.h"

#include <random>

namespace hermes {
namespace vm {

/// \return a SymbolID  for a given C string \p s.
static inline CallResult<Handle<SymbolID>> symbolForCStr(
    Runtime *rt,
    const char *s) {
  return rt->getIdentifierTable().getSymbolHandle(rt, ASCIIRef{s, strlen(s)});
}

// ES7 24.1.1.3
CallResult<HermesValue>
hermesInternalDetachArrayBuffer(void *, Runtime *runtime, NativeArgs args) {
  auto buffer = args.dyncastArg<JSArrayBuffer>(0);
  if (!buffer) {
    return runtime->raiseTypeError(
        "Cannot use detachArrayBuffer on something which "
        "is not an ArrayBuffer foo");
  }
  buffer->detach(&runtime->getHeap());
  // "void" return
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue>
hermesInternalGetEpilogues(void *, Runtime *runtime, NativeArgs args) {
  // Create outer array with one element per module.
  auto eps = runtime->getEpilogues();
  auto outerLen = eps.size();
  auto outerResult = JSArray::create(runtime, outerLen, outerLen);

  if (outerResult == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto outer = toHandle(runtime, std::move(*outerResult));
  if (outer->setStorageEndIndex(outer, runtime, outerLen) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // Set each element to a Uint8Array holding the epilogue for that module.
  for (unsigned i = 0; i < outerLen; ++i) {
    auto innerLen = eps[i].size();
    if (innerLen != 0) {
      auto result = JSTypedArray<uint8_t, CellKind::Uint8ArrayKind>::allocate(
          runtime, innerLen);
      if (result == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      auto ta = result.getValue();
      std::memcpy(ta->begin(runtime), eps[i].begin(), innerLen);
      JSArray::unsafeSetExistingElementAt(
          *outer, runtime, i, ta.getHermesValue());
    }
  }
  return HermesValue::encodeObjectValue(*outer);
}

/// Set the parent of an object failing silently on any error.
CallResult<HermesValue>
silentObjectSetPrototypeOf(void *, Runtime *runtime, NativeArgs args) {
  JSObject *O = dyn_vmcast<JSObject>(args.getArg(0));
  if (!O)
    return HermesValue::encodeUndefinedValue();

  JSObject *parent;
  HermesValue V = args.getArg(1);
  if (V.isNull())
    parent = nullptr;
  else if (V.isObject())
    parent = vmcast<JSObject>(V);
  else
    return HermesValue::encodeUndefinedValue();

  (void)JSObject::setParent(O, runtime, parent);

  // Ignore exceptions.
  runtime->clearThrownValue();

  return HermesValue::encodeUndefinedValue();
}

/// Used for testing, determines how many live values
/// are in the given WeakMap or WeakSet.
CallResult<HermesValue>
hermesInternalGetWeakSize(void *, Runtime *runtime, NativeArgs args) {
  auto M = args.dyncastArg<JSWeakMap>(0);
  if (M) {
    return HermesValue::encodeNumberValue(JSWeakMap::debugGetSize(*M));
  }

  auto S = args.dyncastArg<JSWeakSet>(0);
  if (S) {
    return HermesValue::encodeNumberValue(JSWeakSet::debugGetSize(*S));
  }

  return runtime->raiseTypeError(
      "getWeakSize can only be called on a WeakMap/WeakSet");
}

/// \return an object containing various instrumented statistics.
CallResult<HermesValue>
hermesInternalGetInstrumentedStats(void *, Runtime *runtime, NativeArgs args) {
  GCScope gcScope(runtime);
  auto resultHandle = toHandle(runtime, JSObject::create(runtime));
  // Printing the values would be unstable, so prevent that.
  if (runtime->shouldStabilizeInstructionCount())
    return resultHandle.getHermesValue();
  MutableHandle<> tmpHandle{runtime};

  namespace P = Predefined;
/// Adds a property to \c resultHandle. \p KEY provides its name as a \c
/// Predefined enum value, and its value is rooted in \p VALUE.  If property
/// definition fails, the exceptional execution status will be propogated to the
/// outer function.
#define SET_PROP(KEY, VALUE)                                   \
  do {                                                         \
    GCScopeMarkerRAII marker{gcScope};                         \
    tmpHandle = HermesValue::encodeDoubleValue(VALUE);         \
    auto status = JSObject::defineNewOwnProperty(              \
        resultHandle,                                          \
        runtime,                                               \
        Predefined::getSymbolID(KEY),                          \
        PropertyFlags::defaultNewNamedPropertyFlags(),         \
        tmpHandle);                                            \
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) { \
      return ExecutionStatus::EXCEPTION;                       \
    }                                                          \
  } while (false)

  auto &stats = runtime->getRuntimeStats();

  // Ensure that the timers measuring the current execution are up to date.
  stats.flushPendingTimers();

  SET_PROP(P::js_hostFunctionTime, stats.hostFunction.wallDuration);
  SET_PROP(P::js_hostFunctionCPUTime, stats.hostFunction.cpuDuration);
  SET_PROP(P::js_hostFunctionCount, stats.hostFunction.count);

  SET_PROP(P::js_evaluateJSTime, stats.evaluateJS.wallDuration);
  SET_PROP(P::js_evaluateJSCPUTime, stats.evaluateJS.cpuDuration);
  SET_PROP(P::js_evaluateJSCount, stats.evaluateJS.count);

  SET_PROP(P::js_incomingFunctionTime, stats.incomingFunction.wallDuration);
  SET_PROP(P::js_incomingFunctionCPUTime, stats.incomingFunction.cpuDuration);
  SET_PROP(P::js_incomingFunctionCount, stats.incomingFunction.count);
  SET_PROP(P::js_VMExperiments, runtime->getVMExperimentFlags());

  auto makeHermesTime = [](double host, double eval, double incoming) {
    return eval - host + incoming;
  };

  SET_PROP(
      P::js_hermesTime,
      makeHermesTime(
          stats.hostFunction.wallDuration,
          stats.evaluateJS.wallDuration,
          stats.incomingFunction.wallDuration));
  SET_PROP(
      P::js_hermesCPUTime,
      makeHermesTime(
          stats.hostFunction.cpuDuration,
          stats.evaluateJS.cpuDuration,
          stats.incomingFunction.cpuDuration));

  if (stats.shouldSample) {
    SET_PROP(
        P::js_hermesThreadMinorFaults,
        makeHermesTime(
            stats.hostFunction.sampled.threadMinorFaults,
            stats.evaluateJS.sampled.threadMinorFaults,
            stats.incomingFunction.sampled.threadMinorFaults));
    SET_PROP(
        P::js_hermesThreadMajorFaults,
        makeHermesTime(
            stats.hostFunction.sampled.threadMajorFaults,
            stats.evaluateJS.sampled.threadMajorFaults,
            stats.incomingFunction.sampled.threadMajorFaults));
  }

  auto &heap = runtime->getHeap();
  SET_PROP(P::js_numGCs, heap.getNumGCs());
  SET_PROP(P::js_gcCPUTime, heap.getGCCPUTime());
  SET_PROP(P::js_gcTime, heap.getGCTime());

#undef SET_PROP

/// Adds a property to \c resultHandle. \p KEY provides its name as a C string,
/// and its value is rooted in \p VALUE.  If property definition fails, the
/// exceptional execution status will be propogated to the outer function.
#define SET_PROP_NEW(KEY, VALUE)                               \
  do {                                                         \
    GCScopeMarkerRAII marker{gcScope};                         \
    auto keySym = symbolForCStr(runtime, KEY);                 \
    if (LLVM_UNLIKELY(keySym == ExecutionStatus::EXCEPTION)) { \
      return ExecutionStatus::EXCEPTION;                       \
    }                                                          \
    tmpHandle = HermesValue::encodeDoubleValue(VALUE);         \
    auto status = JSObject::defineNewOwnProperty(              \
        resultHandle,                                          \
        runtime,                                               \
        **keySym,                                              \
        PropertyFlags::defaultNewNamedPropertyFlags(),         \
        tmpHandle);                                            \
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) { \
      return ExecutionStatus::EXCEPTION;                       \
    }                                                          \
  } while (false)

  {
    GCBase::HeapInfo info;
    heap.getHeapInfo(info);
    SET_PROP_NEW("js_totalAllocatedBytes", info.totalAllocatedBytes);
    SET_PROP_NEW("js_allocatedBytes", info.allocatedBytes);
    SET_PROP_NEW("js_heapSize", info.heapSize);
    SET_PROP_NEW("js_mallocSizeEstimate", info.mallocSizeEstimate);
    SET_PROP_NEW("js_vaSize", info.va);
  }

  if (stats.shouldSample) {
    SET_PROP_NEW(
        "js_hermesVolCtxSwitches",
        makeHermesTime(
            stats.hostFunction.sampled.volCtxSwitches,
            stats.evaluateJS.sampled.volCtxSwitches,
            stats.incomingFunction.sampled.volCtxSwitches));
    SET_PROP_NEW(
        "js_hermesInvolCtxSwitches",
        makeHermesTime(
            stats.hostFunction.sampled.involCtxSwitches,
            stats.evaluateJS.sampled.involCtxSwitches,
            stats.incomingFunction.sampled.involCtxSwitches));
    // Sampled because it doesn't vary much, not because it's expensive to get.
    SET_PROP_NEW("js_pageSize", oscompat::page_size());
  }

/// Adds a property to \c resultHandle. \p KEY and \p VALUE provide its name and
/// value as a C string and ASCIIRef respectively. If property definition fails,
/// the exceptional execution status will be propogated to the outer function.
#define SET_PROP_STR(KEY, VALUE)                               \
  do {                                                         \
    auto keySym = symbolForCStr(runtime, KEY);                 \
    if (LLVM_UNLIKELY(keySym == ExecutionStatus::EXCEPTION)) { \
      return ExecutionStatus::EXCEPTION;                       \
    }                                                          \
    auto valStr = StringPrimitive::create(runtime, VALUE);     \
    if (LLVM_UNLIKELY(valStr == ExecutionStatus::EXCEPTION)) { \
      return ExecutionStatus::EXCEPTION;                       \
    }                                                          \
    tmpHandle = *valStr;                                       \
    auto status = JSObject::defineNewOwnProperty(              \
        resultHandle,                                          \
        runtime,                                               \
        **keySym,                                              \
        PropertyFlags::defaultNewNamedPropertyFlags(),         \
        tmpHandle);                                            \
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) { \
      return ExecutionStatus::EXCEPTION;                       \
    }                                                          \
  } while (false)

  if (runtime->getRuntimeStats().shouldSample) {
    size_t bytecodePagesResident = 0;
    size_t bytecodePagesResidentRuns = 0;
    for (auto &module : runtime->getRuntimeModules()) {
      auto buf = module.getBytecode()->getRawBuffer();
      if (buf.size()) {
        llvm::SmallVector<int, 64> runs;
        int pages = oscompat::pages_in_ram(buf.data(), buf.size(), &runs);
        if (pages >= 0) {
          bytecodePagesResident += pages;
          bytecodePagesResidentRuns += runs.size();
        }
      }
    }
    SET_PROP_NEW("js_bytecodePagesResident", bytecodePagesResident);
    SET_PROP_NEW("js_bytecodePagesResidentRuns", bytecodePagesResidentRuns);

    // Stats for the module with most accesses.
    uint32_t bytecodePagesAccessed = 0;
    uint32_t bytecodeSize = 0;
    JenkinsHash bytecodePagesTraceHash = 0;
    double bytecodeIOus = 0;
    // Sample a small number of (position in access order, page id) pairs
    // encoded as a base64vlq stream.
    static constexpr unsigned NUM_SAMPLES = 32;
    std::string sample;
    for (auto &module : runtime->getRuntimeModules()) {
      auto tracker = module.getBytecode()->getPageAccessTracker();
      if (tracker) {
        auto ids = tracker->getPagesAccessed();
        if (ids.size() <= bytecodePagesAccessed)
          continue;
        bytecodePagesAccessed = ids.size();
        bytecodeSize = module.getBytecode()->getRawBuffer().size();
        bytecodePagesTraceHash = 0;
        for (auto id : ids) {
          // char16_t is at least 16 bits unsigned, so the quality of this hash
          // might degrade if accessing bytecode above 2^16 * 4 kB = 256 MB.
          bytecodePagesTraceHash = updateJenkinsHash(
              bytecodePagesTraceHash, static_cast<char16_t>(id));
        }
        bytecodeIOus = 0;
        for (auto us : tracker->getMicros()) {
          bytecodeIOus += us;
        }
        sample.clear();
        llvm::raw_string_ostream str(sample);
        std::random_device rng;
        for (unsigned sampleIdx = 0; sampleIdx < NUM_SAMPLES; ++sampleIdx) {
          int32_t accessOrderPos = rng() % ids.size();
          base64vlq::encode(str, accessOrderPos);
          base64vlq::encode(str, ids[accessOrderPos]);
        }
      }
    }
    if (bytecodePagesAccessed) {
      SET_PROP_NEW("js_bytecodePagesAccessed", bytecodePagesAccessed);
      SET_PROP_NEW("js_bytecodeSize", bytecodeSize);
      SET_PROP_NEW("js_bytecodePagesTraceHash", bytecodePagesTraceHash);
      SET_PROP_NEW("js_bytecodeIOTime", bytecodeIOus / 1e6);
      SET_PROP_STR(
          "js_bytecodePagesTraceSample",
          ASCIIRef(sample.data(), sample.size()));
    }
  }

  return resultHandle.getHermesValue();

#undef SET_PROP_NEW
}

/// \return an object mapping keys to runtime property values.
CallResult<HermesValue>
hermesInternalGetRuntimeProperties(void *, Runtime *runtime, NativeArgs args) {
  GCScope gcScope(runtime);
  auto resultHandle = toHandle(runtime, JSObject::create(runtime));
  MutableHandle<> tmpHandle{runtime};

  /// Add a property \p value keyed under \p key to resultHandle.
  /// \return an ExecutionStatus.
  auto addProperty = [&](Handle<> value, const char *key) {
    auto keySym = symbolForCStr(runtime, key);
    if (LLVM_UNLIKELY(keySym == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return JSObject::defineNewOwnProperty(
        resultHandle,
        runtime,
        **keySym,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        value);
  };

  tmpHandle = HermesValue::encodeDoubleValue(::hermes::hbc::BYTECODE_VERSION);
  if (LLVM_UNLIKELY(
          addProperty(tmpHandle, "Bytecode Version") ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  tmpHandle = HermesValue::encodeBoolValue(runtime->builtinsAreFrozen());
  if (LLVM_UNLIKELY(
          addProperty(tmpHandle, "Builtins Frozen") ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  tmpHandle = HermesValue::encodeNumberValue(runtime->getVMExperimentFlags());
  if (LLVM_UNLIKELY(
          addProperty(tmpHandle, "VM Experiments") ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return resultHandle.getHermesValue();
}

/// ES6.0 12.2.9.3 Runtime Semantics: GetTemplateObject ( templateLiteral )
/// Given a template literal, return a template object that looks like this:
/// [cookedString0, cookedString1, ..., raw: [rawString0, rawString1]].
/// This object is frozen, as well as the 'raw' object nested inside.
/// We only pass the parts from the template literal that are needed to
/// construct this object. That is, the raw strings and cooked strings.
/// Arguments: \p templateObjID is the unique id associated with the template
/// object. \p dup is a boolean, when it is true, cooked strings are the same as
/// raw strings. Then raw strings are passed. Finally cooked strings are
/// optionally passed if \p dup is true.
CallResult<HermesValue>
hermesInternalGetTemplateObject(void *, Runtime *runtime, NativeArgs args) {
  if (LLVM_UNLIKELY(args.getArgCount() < 3)) {
    return runtime->raiseTypeError("At least three arguments expected");
  }
  if (LLVM_UNLIKELY(!args.getArg(0).isNumber())) {
    return runtime->raiseTypeError("First argument should be a number");
  }
  if (LLVM_UNLIKELY(!args.getArg(1).isBool())) {
    return runtime->raiseTypeError("Second argument should be a bool");
  }

  GCScope gcScope{runtime};

  // Try finding the template object in the template object cache.
  uint32_t templateObjID = args.getArg(0).getNumberAs<uint32_t>();
  auto savedCB = runtime->getStackFrames().begin()->getSavedCodeBlock();
  if (LLVM_UNLIKELY(!savedCB)) {
    return runtime->raiseTypeError("Cannot be called from native code");
  }
  RuntimeModule *runtimeModule = savedCB->getRuntimeModule();
  JSObject *cachedTemplateObj =
      runtimeModule->findCachedTemplateObject(templateObjID);
  if (cachedTemplateObj) {
    return HermesValue::encodeObjectValue(cachedTemplateObj);
  }

  bool dup = args.getArg(1).getBool();
  if (LLVM_UNLIKELY(!dup && args.getArgCount() % 2 == 1)) {
    return runtime->raiseTypeError(
        "There must be the same number of raw and cooked strings.");
  }
  uint32_t count = dup ? args.getArgCount() - 2 : args.getArgCount() / 2 - 1;

  // Create template object and raw object.
  auto arrRes = JSArray::create(runtime, count, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto rawObj = runtime->makeHandle<JSObject>(arrRes->getHermesValue());
  auto arrRes2 = JSArray::create(runtime, count, 0);
  if (LLVM_UNLIKELY(arrRes2 == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto templateObj = runtime->makeHandle<JSObject>(arrRes2->getHermesValue());

  // Set cooked and raw strings as elements in template object and raw object,
  // respectively.
  DefinePropertyFlags dpf{};
  dpf.setWritable = 1;
  dpf.setConfigurable = 1;
  dpf.setEnumerable = 1;
  dpf.setValue = 1;
  dpf.writable = 0;
  dpf.configurable = 0;
  dpf.enumerable = 1;
  MutableHandle<> idx{runtime};
  MutableHandle<> rawValue{runtime};
  MutableHandle<> cookedValue{runtime};
  uint32_t cookedBegin = dup ? 2 : 2 + count;
  auto marker = gcScope.createMarker();
  for (uint32_t i = 0; i < count; ++i) {
    idx = HermesValue::encodeNumberValue(i);

    cookedValue = args.getArg(cookedBegin + i);
    auto putRes = JSObject::defineOwnComputedPrimitive(
        templateObj, runtime, idx, dpf, cookedValue);
    assert(
        putRes != ExecutionStatus::EXCEPTION && *putRes &&
        "Failed to set cooked value to template object.");

    rawValue = args.getArg(2 + i);
    putRes = JSObject::defineOwnComputedPrimitive(
        rawObj, runtime, idx, dpf, rawValue);
    assert(
        putRes != ExecutionStatus::EXCEPTION && *putRes &&
        "Failed to set raw value to raw object.");

    gcScope.flushToMarker(marker);
  }
  // Make 'length' property on the raw object read-only.
  DefinePropertyFlags readOnlyDPF{};
  readOnlyDPF.setWritable = 1;
  readOnlyDPF.setConfigurable = 1;
  readOnlyDPF.writable = 0;
  readOnlyDPF.configurable = 0;
  auto readOnlyRes = JSObject::defineOwnProperty(
      rawObj,
      runtime,
      Predefined::getSymbolID(Predefined::length),
      readOnlyDPF,
      Runtime::getUndefinedValue(),
      PropOpFlags().plusThrowOnError());
  if (LLVM_UNLIKELY(readOnlyRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(!*readOnlyRes)) {
    return runtime->raiseTypeError(
        "Failed to set 'length' property on the raw object read-only.");
  }
  JSObject::preventExtensions(rawObj.get());

  // Set raw object as a read-only non-enumerable property of the template
  // object.
  PropertyFlags constantPF{};
  constantPF.writable = 0;
  constantPF.configurable = 0;
  constantPF.enumerable = 0;
  auto putNewRes = JSObject::defineNewOwnProperty(
      templateObj,
      runtime,
      Predefined::getSymbolID(Predefined::raw),
      constantPF,
      rawObj);
  if (LLVM_UNLIKELY(putNewRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // Make 'length' property on the template object read-only.
  readOnlyRes = JSObject::defineOwnProperty(
      templateObj,
      runtime,
      Predefined::getSymbolID(Predefined::length),
      readOnlyDPF,
      Runtime::getUndefinedValue(),
      PropOpFlags().plusThrowOnError());
  if (LLVM_UNLIKELY(readOnlyRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(!*readOnlyRes)) {
    return runtime->raiseTypeError(
        "Failed to set 'length' property on the raw object read-only.");
  }
  JSObject::preventExtensions(templateObj.get());

  // Cache the template object.
  runtimeModule->cacheTemplateObject(templateObjID, templateObj);

  return templateObj.getHermesValue();
}

/// If the first argument is not an object, throw a type error with the second
/// argument as a message.
///
/// \code
///   HermesInternal.ensureObject = function(value, errorMessage) {...}
/// \endcode
CallResult<HermesValue>
hermesInternalEnsureObject(void *, Runtime *runtime, NativeArgs args) {
  if (LLVM_LIKELY(args.getArg(0).isObject()))
    return HermesValue::encodeUndefinedValue();

  return runtime->raiseTypeError(args.getArgHandle(1));
}

/// Throw a type error with the argument as a message.
///
/// \code
///   HermesInternal.throwTypeError = function(errorMessage) {...}
/// \endcode
CallResult<HermesValue>
hermesInternalThrowTypeError(void *, Runtime *runtime, NativeArgs args) {
  return runtime->raiseTypeError(args.getArgHandle(0));
}

/// Set the isDelegated flag on the GeneratorInnerFunction which calls
/// this function.
/// \pre the caller must be an interpreted GeneratorInnerFunction
/// \return `undefined`
CallResult<HermesValue>
hermesInternalGeneratorSetDelegated(void *, Runtime *runtime, NativeArgs args) {
  auto *gen = dyn_vmcast_or_null<GeneratorInnerFunction>(
      runtime->getCurrentFrame().getPreviousFrame().getCalleeClosure());
  if (!gen) {
    return runtime->raiseTypeError(
        "generatorSetDelegated can only be called as part of yield*");
  }
  gen->setIsDelegated(true);
  return HermesValue::encodeUndefinedValue();
}

/// \code
///   HermesInternal.copyDataProperties =
///         function (target, source, excludedItems) {}
/// \endcode
///
/// Copy all enumerable own properties of object \p source, that are not also
/// properties of \p excludedItems, into \p target, which must be an object, and
/// return \p target. If \p excludedItems is not specified, it is assumed
/// to be empty.
CallResult<HermesValue>
hermesInternalCopyDataProperties(void *, Runtime *runtime, NativeArgs args) {
  GCScope gcScope{runtime};

  Handle<JSObject> target = args.dyncastArg<JSObject>(0);
  // To be safe, ignore non-objects.
  if (!target)
    return HermesValue::encodeUndefinedValue();

  Handle<> untypedSource = args.getArgHandle(1);
  if (untypedSource->isNull() || untypedSource->isUndefined())
    return target.getHermesValue();

  Handle<JSObject> source = untypedSource->isObject()
      ? Handle<JSObject>::vmcast(untypedSource)
      : Handle<JSObject>::vmcast(
            runtime->makeHandle(*toObject(runtime, untypedSource)));
  Handle<JSObject> excludedItems = args.dyncastArg<JSObject>(2);

  MutableHandle<> nameHandle{runtime};
  MutableHandle<> valueHandle{runtime};

  // Process all named properties/symbols.
  bool success = JSObject::forEachOwnPropertyWhile(
      source,
      runtime,
      // indexedCB.
      [&source, &target, &excludedItems, &nameHandle, &valueHandle](
          Runtime *runtime, uint32_t index, ComputedPropertyDescriptor desc) {
        if (!desc.flags.enumerable)
          return true;

        nameHandle = HermesValue::encodeNumberValue(index);

        if (excludedItems) {
          ComputedPropertyDescriptor xdesc;
          auto cr = JSObject::getOwnComputedPrimitiveDescriptor(
              excludedItems, runtime, nameHandle, xdesc);
          if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
            return false;
          if (*cr)
            return true;
        }

        valueHandle = JSObject::getOwnIndexed(*source, runtime, index);

        if (LLVM_UNLIKELY(
                JSObject::defineOwnComputedPrimitive(
                    target,
                    runtime,
                    nameHandle,
                    DefinePropertyFlags::getDefaultNewPropertyFlags(),
                    valueHandle) == ExecutionStatus::EXCEPTION)) {
          return false;
        }

        return true;
      },
      // namedCB.
      [&source, &target, &excludedItems, &valueHandle](
          Runtime *runtime, SymbolID sym, NamedPropertyDescriptor desc) {
        if (!desc.flags.enumerable)
          return true;
        if (InternalProperty::isInternal(sym))
          return true;

        // Skip excluded items.
        if (excludedItems) {
          auto cr = JSObject::hasNamedOrIndexed(excludedItems, runtime, sym);
          assert(
              cr != ExecutionStatus::EXCEPTION &&
              "hasNamedOrIndex failed, which can only happen with a proxy, "
              "but excludedItems should never be a proxy");
          if (*cr)
            return true;
        }

        auto cr =
            JSObject::getNamedPropertyValue_RJS(source, runtime, source, desc);
        if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
          return false;

        valueHandle = *cr;

        if (LLVM_UNLIKELY(
                JSObject::defineOwnProperty(
                    target,
                    runtime,
                    sym,
                    DefinePropertyFlags::getDefaultNewPropertyFlags(),
                    valueHandle) == ExecutionStatus::EXCEPTION)) {
          return false;
        }

        return true;
      });

  if (LLVM_UNLIKELY(!success))
    return ExecutionStatus::EXCEPTION;

  return target.getHermesValue();
}

/// \code
///   HermesInternal.copyRestArgs = function (from) {}
/// \endcode
/// Copy the callers parameters starting from index \c from (where the first
/// parameter is index 0) into a JSArray.
CallResult<HermesValue>
hermesInternalCopyRestArgs(void *, Runtime *runtime, NativeArgs args) {
  GCScopeMarkerRAII marker{runtime};

  // Obtain the caller's stack frame.
  auto frames = runtime->getStackFrames();
  auto it = frames.begin();
  ++it;
  // Check for the extremely unlikely case where there is no caller frame.
  if (LLVM_UNLIKELY(it == frames.end()))
    return HermesValue::encodeUndefinedValue();

  // "from" should be a number.
  if (!args.getArg(0).isNumber())
    return HermesValue::encodeUndefinedValue();
  uint32_t from = truncateToUInt32(args.getArg(0).getNumber());

  uint32_t argCount = it->getArgCount();
  uint32_t length = from <= argCount ? argCount - from : 0;

  auto cr = JSArray::create(runtime, length, length);
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  auto array = toHandle(runtime, std::move(*cr));
  JSArray::setStorageEndIndex(array, runtime, length);

  for (uint32_t i = 0; i != length; ++i) {
    array->unsafeSetExistingElementAt(
        array.get(), runtime, i, it->getArgRef(from));
    ++from;
  }

  return array.getHermesValue();
}

/// \code
///   HermesInternal.arraySpread = function(target, source, nextIndex) {}
/// /endcode
/// ES9.0 12.2.5.2
/// Iterate the iterable source (as if using a for-of) and copy the values from
/// the spread source into the target array, starting at `nextIndex`.
/// \return the next empty index in the array to use for additional properties.
CallResult<HermesValue>
hermesInternalArraySpread(void *, Runtime *runtime, NativeArgs args) {
  Handle<JSArray> target = args.dyncastArg<JSArray>(0);
  // To be safe, check for non-arrays.
  if (!target) {
    return runtime->raiseTypeError(
        "HermesInternal.arraySpread requires an array target");
  }

  // 3. Let iteratorRecord be ? GetIterator(spreadObj).
  auto iteratorRecordRes = getIterator(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(iteratorRecordRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  IteratorRecord iteratorRecord = *iteratorRecordRes;

  MutableHandle<> nextValue{runtime};
  MutableHandle<> nextIndex{runtime, args.getArg(2)};

  // 4. Repeat,
  // TODO: Add a fast path when the source is an array.
  for (GCScopeMarkerRAII marker{runtime}; /* nothing */; marker.flush()) {
    // a. Let next be ? IteratorStep(iteratorRecord).
    auto nextRes = iteratorStep(runtime, iteratorRecord);
    if (LLVM_UNLIKELY(nextRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    Handle<JSObject> next = *nextRes;

    // b. If next is false, return nextIndex.
    if (!next) {
      return nextIndex.getHermesValue();
    }
    // c. Let nextValue be ? IteratorValue(next).
    auto nextItemRes = JSObject::getNamed_RJS(
        next, runtime, Predefined::getSymbolID(Predefined::value));
    if (LLVM_UNLIKELY(nextItemRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    nextValue = *nextItemRes;

    // d. Let status be CreateDataProperty(array,
    //    ToString(ToUint32(nextIndex)), nextValue).
    // e. Assert: status is true.
    if (LLVM_UNLIKELY(
            JSArray::putComputed_RJS(target, runtime, nextIndex, nextValue) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // f. Let nextIndex be nextIndex + 1.
    nextIndex = HermesValue::encodeNumberValue(nextIndex->getNumber() + 1);
  }

  return nextIndex.getHermesValue();
}

/// \code
///   HermesInternal.apply = function(fn, argArray, thisVal(opt)) {}
/// /endcode
/// Faster version of Function.prototype.apply which does not use its `this`
/// argument.
/// `argArray` must be a JSArray with no getters.
/// Equivalent to fn.apply(thisVal, argArray) if thisVal is provided.
/// If thisVal is not provided, equivalent to running `new fn` and passing the
/// arguments in argArray.
CallResult<HermesValue>
hermesInternalApply(void *, Runtime *runtime, NativeArgs args) {
  GCScopeMarkerRAII marker{runtime};

  Handle<Callable> fn = args.dyncastArg<Callable>(0);
  if (LLVM_UNLIKELY(!fn)) {
    return runtime->raiseTypeErrorForValue(
        args.getArgHandle(0), " is not a function");
  }

  Handle<JSArray> argArray = args.dyncastArg<JSArray>(1);
  if (LLVM_UNLIKELY(!argArray)) {
    return runtime->raiseTypeError("args must be an array");
  }

  uint32_t len = JSArray::getLength(*argArray);

  bool isConstructor = args.getArgCount() == 2;

  MutableHandle<> thisVal{runtime};
  if (isConstructor) {
    auto thisValRes = Callable::createThisForConstruct(fn, runtime);
    if (LLVM_UNLIKELY(thisValRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    thisVal = *thisValRes;
  } else {
    thisVal = args.getArg(2);
  }

  ScopedNativeCallFrame newFrame{
      runtime, len, *fn, isConstructor, thisVal.getHermesValue()};
  for (uint32_t i = 0; i < len; ++i) {
    newFrame->getArgRef(i) = argArray->at(runtime, i);
  }
  return isConstructor ? Callable::construct(fn, runtime, thisVal)
                       : Callable::call(fn, runtime);
}

#ifdef HERMESVM_PLATFORM_LOGGING
static void logGCStats(Runtime *runtime, const char *msg) {
  // The GC stats can exceed the android logcat length limit, of
  // 1024 bytes.  Break it up.
  std::string stats;
  {
    llvm::raw_string_ostream os(stats);
    runtime->printHeapStats(os);
  }
  auto copyRegionFrom = [&stats](size_t from) -> size_t {
    size_t rBrace = stats.find("},", from);
    if (rBrace == std::string::npos) {
      std::string portion = stats.substr(from);
      hermesLog("HermesVM", "%s", portion.c_str());
      return stats.size();
    }

    // Add 2 for the length of the search string, to get to the end.
    const size_t to = rBrace + 2;
    std::string portion = stats.substr(from, to - from);
    hermesLog("HermesVM", "%s", portion.c_str());
    return to;
  };

  hermesLog("HermesVM", "%s:", msg);
  for (size_t ind = 0; ind < stats.size(); ind = copyRegionFrom(ind))
    ;
}
#endif

CallResult<HermesValue>
hermesInternalTTIReached(void *, Runtime *runtime, NativeArgs args) {
  runtime->ttiReached();
#ifdef HERMESVM_LLVM_PROFILE_DUMP
  __llvm_profile_dump();
  throw jsi::JSINativeException("TTI reached; profiling done");
#endif
#ifdef HERMESVM_PLATFORM_LOGGING
  logGCStats(runtime, "TTI call");
#endif
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue>
hermesInternalTTRCReached(void *, Runtime *runtime, NativeArgs args) {
  // Currently does nothing, but could change in the future.
  return HermesValue::encodeUndefinedValue();
}

/// HermesInternal.exportAll(exports, source) will copy exported named
/// properties from `source` to `exports`, defining them on `exports` as
/// non-configurable.
/// Note that the default exported property on `source` is ignored,
/// as are non-enumerable properties on `source`.
CallResult<HermesValue>
hermesInternalExportAll(void *, Runtime *runtime, NativeArgs args) {
  Handle<JSObject> exports = args.dyncastArg<JSObject>(0);
  if (LLVM_UNLIKELY(!exports)) {
    return runtime->raiseTypeError(
        "exportAll() exports argument must be object");
  }

  Handle<JSObject> source = args.dyncastArg<JSObject>(1);
  if (LLVM_UNLIKELY(!source)) {
    return runtime->raiseTypeError(
        "exportAll() source argument must be object");
  }

  MutableHandle<> propertyHandle{runtime};

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.configurable = 0;

  CallResult<bool> defineRes{ExecutionStatus::EXCEPTION};

  // Iterate the named properties excluding those which use Symbols.
  bool result = HiddenClass::forEachPropertyWhile(
      runtime->makeHandle(source->getClass(runtime)),
      runtime,
      [&source, &exports, &propertyHandle, &dpf, &defineRes](
          Runtime *runtime, SymbolID id, NamedPropertyDescriptor desc) {
        if (!desc.flags.enumerable)
          return true;

        if (id == Predefined::getSymbolID(Predefined::defaultExport)) {
          return true;
        }

        propertyHandle = JSObject::getNamedSlotValue(*source, runtime, desc);
        defineRes = JSObject::defineOwnProperty(
            exports, runtime, id, dpf, propertyHandle);
        if (LLVM_UNLIKELY(defineRes == ExecutionStatus::EXCEPTION)) {
          return false;
        }

        return true;
      });
  if (LLVM_UNLIKELY(!result)) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeUndefinedValue();
}

#ifdef HERMESVM_EXCEPTION_ON_OOM
/// Gets the current call stack as a JS String value.  Intended (only)
/// to allow testing of Runtime::callStack() from JS code.
CallResult<HermesValue>
hermesInternalGetCallStack(void *, Runtime *runtime, NativeArgs args) {
  std::string stack = runtime->getCallStackNoAlloc();
  return StringPrimitive::create(runtime, ASCIIRef(stack.data(), stack.size()));
}
#endif // HERMESVM_EXCEPTION_ON_OOM

#ifdef HERMESVM_USE_JS_LIBRARY_IMPLEMENTATION
/// \code
///   HermesInternal.executeCall = function (func, thisArg, ...params) {}
/// \endcode
/// Invoke func with `this` context set to thisArg, and parameters param1,
/// param2, and param3
CallResult<HermesValue>
hermesInternalExecuteCall(void *, Runtime *runtime, NativeArgs args) {
  auto func = args.dyncastArg<Callable>(0);
  if (!func) {
    return runtime->raiseTypeError("Non-callable value passed to executeCall");
  }

  const unsigned offset = 2; // number of args before the params
  unsigned numParam = args.getArgCount() - offset;
  ScopedNativeCallFrame newFrame{runtime,
                                 numParam,
                                 func.getHermesValue(),
                                 HermesValue::encodeUndefinedValue(),
                                 args.getArg(1)};
  if (LLVM_UNLIKELY(newFrame.overflowed()))
    return runtime->raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);

  for (unsigned i = 0; i < numParam; i++) {
    newFrame->getArgRef(i) = args.getArg(i + offset);
  }
  return Callable::call(func, runtime);
}

///   HermesInternal.getSubstitution
///     = function (matched,str, position, captures, replacement) {}
/// \encode
/// Returns true if func is a valid constructor, false otherwise.
CallResult<HermesValue>
hermesInternalGetSubstitution(void *, Runtime *runtime, NativeArgs args) {
  auto matched = args.dyncastArg<StringPrimitive>(0);
  auto str = args.dyncastArg<StringPrimitive>(1);
  auto replacement = args.dyncastArg<StringPrimitive>(4);
  if (!matched || !str || !replacement) {
    return runtime->raiseTypeError(
        "First, second, and fifth arguments should be strings");
  }

  auto posArg = args.getArg(2);
  if (!posArg.isNumber()) {
    return runtime->raiseTypeError("Third argument should be a number");
  }
  uint32_t position = posArg.getNumberAs<uint32_t>();

  Handle<JSArray> capturesArg = args.dyncastArg<JSArray>(3);
  if (!capturesArg) {
    return runtime->raiseTypeError("Fourth argument should be an array");
  }

  uint32_t capturesLen = JSArray::getLength(*capturesArg);
  auto arrRes = ArrayStorage::create(runtime, capturesLen /* capacity */);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  MutableHandle<ArrayStorage> captures{runtime,
                                       vmcast<ArrayStorage>(arrRes.getValue())};

  for (uint i = 0; i < capturesLen; ++i) {
    GCScopeMarkerRAII marker{runtime};
    ArrayStorage::push_back(
        captures, runtime, capturesArg->handleAt(runtime, i));
  }

  return getSubstitution(
      runtime, matched, str, position, captures, replacement);
}

/// \code
///   HermesInternal.isConstructor = function (func) {}
/// \endcode
/// Returns true if func is a valid constructor, false otherwise.
CallResult<HermesValue>
hermesInternalIsConstructor(void *, Runtime *runtime, NativeArgs args) {
  return HermesValue::encodeBoolValue(isConstructor(runtime, args.getArg(0)));
}

///   HermesInternal.isRegExp = function (pattern) {}
/// \encode
/// Returns true if pattern is a valid RegExp, false otherwise.
CallResult<HermesValue>
hermesInternalIsRegExp(void *, Runtime *runtime, NativeArgs args) {
  auto boolRes = isRegExp(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(boolRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeBoolValue(boolRes.getValue());
}

/// \code
///   HermesInternal.jsArraySetElementAt = function (array, index, val) {}
/// \endcode
/// Set array[index] to val without triggering the setter.
CallResult<HermesValue>
hermesInternalJSArraySetElementAt(void *, Runtime *runtime, NativeArgs args) {
  JSArray::setElementAt(
      args.dyncastArg<ArrayImpl>(0),
      runtime,
      args.getArg(1).getDouble(),
      args.getArgHandle(2));
  return HermesValue::encodeUndefinedValue();
}

/// \code
///   HermesInternal.regExpCreate = function (pattern, flags) {}
/// \endcode
/// Create and return a RegExp object based on the given pattern and flags
CallResult<HermesValue>
hermesInternalRegExpCreate(void *, Runtime *runtime, NativeArgs args) {
  auto res = regExpCreate(runtime, args.getArgHandle(0), args.getArgHandle(1));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return res->getHermesValue();
}

/// \code
///   HermesInternal.regExpExec = function (regexp, str) {}
/// \encode
/// Returns the result of executing RegExp object regexp on string str.
CallResult<HermesValue>
hermesInternalRegExpExec(void *, Runtime *runtime, NativeArgs args) {
  auto regexp = args.dyncastArg<JSObject>(0);
  auto str = args.dyncastArg<StringPrimitive>(1);
  if (!regexp || !str) {
    return runtime->raiseTypeError(
        "Arguments of regExpExec should be an Object and a String");
  }
  return regExpExec(runtime, regexp, str);
}

/// \code
///   HermesInternal.searchString =
///     function (source, substr, reverse, startOffset, endOffset) {}
/// \endcode
/// Search for the first (or last if reverse is true) occurrence of substr
/// inside of source from the given offsets. Return the starting index of the
/// first (or last is reverse is true) match or -1 if there is no match.
/// TypeError if either argument is not a string.
CallResult<HermesValue>
hermesInternalSearchString(void *, Runtime *runtime, NativeArgs args) {
  auto source = args.dyncastArg<StringPrimitive>(0);
  auto substr = args.dyncastArg<StringPrimitive>(1);
  if (!source || !substr) {
    return runtime->raiseTypeError("Non-string value passed to searchString");
  }

  bool reverse = false;
  if (args.getArg(2).isBool()) {
    reverse = args.getArg(2).getBool();
  }

  uint32_t startOffset = 0;
  if (args.getArg(3).isNumber()) {
    startOffset = args.getArg(3).getNumberAs<uint32_t>();
  }
  uint32_t endOffset = 0;
  if (args.getArg(4).isNumber()) {
    endOffset = args.getArg(4).getNumberAs<uint32_t>();
  }

  auto sourceView = StringPrimitive::createStringView(runtime, source);
  auto substrView = StringPrimitive::createStringView(runtime, substr);
  double ret = -1;

  if (!reverse) {
    auto endIter = sourceView.end() - endOffset;
    auto foundIter = std::search(
        sourceView.begin() + startOffset,
        endIter,
        substrView.begin(),
        substrView.end());
    if (foundIter != endIter || substrView.empty()) {
      ret = foundIter - sourceView.begin();
    }
  } else {
    auto endIter = sourceView.rend() - endOffset;
    auto foundIter = std::search(
        sourceView.rbegin() + startOffset,
        endIter,
        substrView.rbegin(),
        substrView.rend());
    if (foundIter != endIter || substrView.empty()) {
      ret = sourceView.rend() - foundIter;
    }
  }
  return HermesValue::encodeDoubleValue(ret);
}

/// \code
///   HermesInternal.toInteger = function (arg) {}
/// \endcode
/// Converts arg to an integer
CallResult<HermesValue>
hermesInternalToInteger(void *, Runtime *runtime, NativeArgs args) {
  return toInteger(runtime, args.getArgHandle(0));
}

/// \code
///   HermesInternal.toLength function (arg) {}
/// \endcode
/// Converts arg to an integer suitable for use as the length of an array-like
/// object. Return value is an an integer in the range[0, 2^53 - 1].
CallResult<HermesValue>
hermesInternalToLength(void *, Runtime *runtime, NativeArgs args) {
  return toLength(runtime, args.getArgHandle(0));
}

/// \code
///   HermesInternal.toObject function (arg) {}
/// \endcode
/// Converts arg to an object if possible.
/// TypeError if arg is null or undefined.
CallResult<HermesValue>
hermesInternalToObject(void *, Runtime *runtime, NativeArgs args) {
  return toObject(runtime, args.getArgHandle(0));
}

/// \code
///   HermesInternal.toObject function (arg) {}
/// \endcode
/// Converts arg to a string if possible.
/// TypeError if arg is a symbol.
CallResult<HermesValue>
hermesInternalToString(void *, Runtime *runtime, NativeArgs args) {
  auto res = toString_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return res->getHermesValue();
}
#endif // HERMESVM_USE_JS_LIBRARY_IMPLEMENTATION

Handle<JSObject> createHermesInternalObject(Runtime *runtime) {
  Handle<JSObject> intern = toHandle(runtime, JSObject::create(runtime));

  DefinePropertyFlags constantDPF{};
  constantDPF.setEnumerable = 1;
  constantDPF.setWritable = 1;
  constantDPF.setConfigurable = 1;
  constantDPF.setValue = 1;
  constantDPF.enumerable = 0;
  constantDPF.writable = 0;
  constantDPF.configurable = 0;

  auto defineInternMethod =
      [&](Predefined::Str symID, NativeFunctionPtr func, uint8_t count = 0) {
        (void)defineMethod(
            runtime,
            intern,
            Predefined::getSymbolID(symID),
            nullptr /* context */,
            func,
            count,
            constantDPF);
      };

  auto defineInternMethodAndSymbol =
      [&](const char *name, NativeFunctionPtr func, uint8_t count = 0) {
        ASCIIRef ref = createASCIIRef(name);
        Handle<SymbolID> symHandle = runtime->ignoreAllocationFailure(
            runtime->getIdentifierTable().getSymbolHandle(runtime, ref));
        (void)defineMethod(
            runtime,
            intern,
            *symHandle,
            nullptr /* context */,
            func,
            count,
            constantDPF);
      };

  // suppress unused-variable warning
  (void)defineInternMethodAndSymbol;

  // HermesInternal function properties
  namespace P = Predefined;
  defineInternMethod(P::detachArrayBuffer, hermesInternalDetachArrayBuffer, 1);
  defineInternMethod(P::getEpilogues, hermesInternalGetEpilogues);
  defineInternMethod(P::silentSetPrototypeOf, silentObjectSetPrototypeOf, 2);
  defineInternMethod(P::getWeakSize, hermesInternalGetWeakSize);
  defineInternMethod(
      P::getInstrumentedStats, hermesInternalGetInstrumentedStats);
  defineInternMethod(
      P::getRuntimeProperties, hermesInternalGetRuntimeProperties);
  defineInternMethod(P::getTemplateObject, hermesInternalGetTemplateObject);
  defineInternMethod(P::ensureObject, hermesInternalEnsureObject, 2);
  defineInternMethod(P::throwTypeError, hermesInternalThrowTypeError, 1);
  defineInternMethod(
      P::generatorSetDelegated, hermesInternalGeneratorSetDelegated, 1);
  defineInternMethod(
      P::copyDataProperties, hermesInternalCopyDataProperties, 3);
  defineInternMethod(P::copyRestArgs, hermesInternalCopyRestArgs, 1);
  defineInternMethod(P::arraySpread, hermesInternalArraySpread, 2);
  defineInternMethod(P::apply, hermesInternalApply, 2);
  defineInternMethod(P::ttiReached, hermesInternalTTIReached);
  defineInternMethod(P::ttrcReached, hermesInternalTTRCReached);
  defineInternMethod(P::exportAll, hermesInternalExportAll);
  defineInternMethod(P::exponentiationOperator, mathPow);
#ifdef HERMESVM_USE_JS_LIBRARY_IMPLEMENTATION
  defineInternMethodAndSymbol("executeCall", hermesInternalExecuteCall);
  defineInternMethodAndSymbol("getSubstitution", hermesInternalGetSubstitution);
  defineInternMethodAndSymbol("isConstructor", hermesInternalIsConstructor);
  defineInternMethodAndSymbol("isRegExp", hermesInternalIsRegExp);
  defineInternMethodAndSymbol(
      "jsArraySetElementAt", hermesInternalJSArraySetElementAt);
  defineInternMethodAndSymbol("regExpCreate", hermesInternalRegExpCreate);
  defineInternMethodAndSymbol("regExpExec", hermesInternalRegExpExec);
  defineInternMethodAndSymbol("searchString", hermesInternalSearchString);
  defineInternMethodAndSymbol("toInteger", hermesInternalToInteger);
  defineInternMethodAndSymbol("toLength", hermesInternalToLength);
  defineInternMethodAndSymbol("toObject", hermesInternalToObject);
  defineInternMethodAndSymbol("toString", hermesInternalToString);
#endif // HERMESVM_USE_JS_LIBRARY_IMPLEMENTATION
#ifdef HERMESVM_EXCEPTION_ON_OOM
  defineInternMethodAndSymbol("getCallStack", hermesInternalGetCallStack, 0);
#endif // HERMESVM_EXCEPTION_ON_OOM

  // Define the 'requireFast' function, which takes a number argument.
  (void)defineMethod(
      runtime,
      intern,
      Predefined::getSymbolID(Predefined::requireFast),
      nullptr,
      requireFast,
      1,
      constantDPF);

  // Make a copy of the original String.prototype.concat implementation that we
  // can use internally.
  // TODO: we can't make HermesInternal.concat a static builtin method now
  // because this method should be called with a meaningful `this`, but
  // CallBuiltin instruction does not support it.
  auto propRes = JSObject::getNamed_RJS(
      runtime->makeHandle<JSObject>(runtime->stringPrototype),
      runtime,
      Predefined::getSymbolID(Predefined::concat));
  assert(
      propRes != ExecutionStatus::EXCEPTION && !propRes->isUndefined() &&
      "Failed to get String.prototype.concat.");
  auto putRes = JSObject::defineOwnProperty(
      intern,
      runtime,
      Predefined::getSymbolID(Predefined::concat),
      constantDPF,
      runtime->makeHandle(*propRes));
  assert(
      putRes != ExecutionStatus::EXCEPTION && *putRes &&
      "Failed to set HermesInternal.concat.");
  (void)putRes;

  JSObject::preventExtensions(*intern);

  return intern;
}

} // namespace vm
} // namespace hermes
