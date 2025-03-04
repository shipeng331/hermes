/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSLIB_JSLIBINTERNAL_H
#define HERMES_VM_JSLIB_JSLIBINTERNAL_H

#include "hermes/VM/Callable.h"
#include "hermes/VM/JSDate.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSMapImpl.h"
#include "hermes/VM/JSNativeFunctions.h"
#include "hermes/VM/JSRegExp.h"

namespace hermes {
namespace vm {

/// This function declares a new system constructor (the likes of 'Object' and
/// 'Array') with a specified object to be used as the 'prototype' property.
/// - First, it creates a new NativeFunction object for the constructor, with
///   Function.prototype as the internal prototype.
/// - Initializes the native function's 'prototype' property with the supplied
///   prototype object.
/// - Then, it sets the supplied prototype object's 'constructor' property to
///   the created NativeFunction.
/// - Lastly, it initializes the requested global property with the
///   NativeFunction.
///
/// \param name the name of the global property to initialize (e.g. 'Object')
/// \param nativeFunctionPtr the native function implementing the constructor.
/// \param prototypeObjectHandle the object instance to set in the 'prototype'
///   property of the constructor.
/// \param paramCount the number of declared constructor parameters
/// The second version takes an additional parameter:
/// \param constructorProtoObjectHandle the prototype to set for the
///   constructor function
/// \return the created constructor function.
Handle<NativeConstructor> defineSystemConstructor(
    Runtime *runtime,
    SymbolID name,
    NativeFunctionPtr nativeFunctionPtr,
    Handle<JSObject> prototypeObjectHandle,
    Handle<JSObject> constructorProtoObjectHandle,
    unsigned paramCount,
    NativeConstructor::CreatorFunction *creator,
    CellKind targetKind);

Handle<NativeConstructor> defineSystemConstructor(
    Runtime *runtime,
    SymbolID name,
    NativeFunctionPtr nativeFunctionPtr,
    Handle<JSObject> prototypeObjectHandle,
    unsigned paramCount,
    NativeConstructor::CreatorFunction *creator,
    CellKind targetKind);

template <class NativeClass>
Handle<NativeConstructor> defineSystemConstructor(
    Runtime *runtime,
    SymbolID name,
    NativeFunctionPtr nativeFunctionPtr,
    Handle<JSObject> prototypeObjectHandle,
    unsigned paramCount,
    CellKind targetKind) {
  return defineSystemConstructor(
      runtime,
      name,
      nativeFunctionPtr,
      prototypeObjectHandle,
      paramCount,
      NativeClass::create,
      targetKind);
}

/// Define a method in an object instance.
/// Currently, it's only used to define global %HermesInternal object in
/// createHermesInternalObject(), with different flags, i.e. writable = 0 and
/// configurable = 0.
/// \param objectHandle the instance where the method is defined.
/// \param propertyName the key in objectHandle to insert the method at.
/// \param methodName the value of the function.name property.
/// \param context the context to pass to the native function.
/// \param nativeFunctionPtr the native function implementing the method.
/// \param paramCount the number of declared method parameters
/// \param dpf the flags to set on the newly defined property.
/// \return the new NativeFunction.
CallResult<HermesValue> defineMethod(
    Runtime *runtime,
    Handle<JSObject> objectHandle,
    SymbolID propertyName,
    SymbolID methodName,
    void *context,
    NativeFunctionPtr nativeFunctionPtr,
    unsigned paramCount,
    DefinePropertyFlags dpf);

/// Define a method in an object instance.
/// Currently, it's only used to define global %HermesInternal object in
/// createHermesInternalObject(), with different flags, i.e. writable = 0 and
/// configurable = 0.
/// \param objectHandle the instance where the method is defined.
/// \param name the key in objectHandle to insert the method at.
/// \param context the context to pass to the native function.
/// \param nativeFunctionPtr the native function implementing the method.
/// \param paramCount the number of declared method parameters
/// \param dpf the flags to set on the newly defined property.
/// \return the new NativeFunction.
inline CallResult<HermesValue> defineMethod(
    Runtime *runtime,
    Handle<JSObject> objectHandle,
    SymbolID name,
    void *context,
    NativeFunctionPtr nativeFunctionPtr,
    unsigned paramCount,
    DefinePropertyFlags dpf) {
  return defineMethod(
      runtime,
      objectHandle,
      name,
      name,
      context,
      nativeFunctionPtr,
      paramCount,
      dpf);
}

/// Define a method in an object instance.
/// \param objectHandle the instance where the method is defined.
/// \param name the name of the method.
/// \param context the context to pass to the native function.
/// \param nativeFunctionPtr the native function implementing the method.
/// \param paramCount the number of declared method parameters
///  \return the new NativeFunction.
void defineMethod(
    Runtime *runtime,
    Handle<JSObject> objectHandle,
    SymbolID name,
    void *context,
    NativeFunctionPtr nativeFunctionPtr,
    unsigned paramCount);

/// Define an accessor in an object instance.
/// \param objectHandle the instance where the accessor is defined.
/// \param propertyName the key in the object at which to define the accessor.
/// \param methodName the name of the function.
/// \param context the context to pass to the native functions.
/// \param getterFunc the native function implementing the getter.
/// \param setterFunc the native function implementing the setter.
void defineAccessor(
    Runtime *runtime,
    Handle<JSObject> objectHandle,
    SymbolID propertyName,
    SymbolID methodName,
    void *context,
    NativeFunctionPtr getterFunc,
    NativeFunctionPtr setterFunc,
    bool enumerable,
    bool configurable);

/// Define an accessor in an object instance.
/// \param objectHandle the instance where the accessor is defined.
/// \param name the name of the accessor.
/// \param context the context to pass to the native functions.
/// \param getterFunc the native function implementing the getter.
/// \param setterFunc the native function implementing the setter.
inline void defineAccessor(
    Runtime *runtime,
    Handle<JSObject> objectHandle,
    SymbolID name,
    void *context,
    NativeFunctionPtr getterFunc,
    NativeFunctionPtr setterFunc,
    bool enumerable,
    bool configurable) {
  defineAccessor(
      runtime,
      objectHandle,
      name,
      name,
      context,
      getterFunc,
      setterFunc,
      enumerable,
      configurable);
}

/// Define a property in an object instance.
/// The property is writable, configurable, but not enumerable.
/// \param objectHandle the instance where the property is defined.
/// \param name the name of the property.
/// \param value the value to set to the property.
void defineProperty(
    Runtime *runtime,
    Handle<JSObject> objectHandle,
    SymbolID name,
    Handle<> value);

/// Define a property in an object instance.
/// \param objectHandle the instance where the property is defined.
/// \param name the name of the property.
/// \param value the value to set to the property.
/// \param dpf the flags to set on the newly defined property.
void defineProperty(
    Runtime *runtime,
    Handle<JSObject> objectHandle,
    SymbolID name,
    Handle<> value,
    DefinePropertyFlags dpf);

/// Call the IteratorClose operation following an exception being thrown.
/// \pre runtime->thrownValue_ must be populated with a thrown value.
/// \return ExecutionStatus::EXCEPTION
ExecutionStatus iteratorCloseAndRethrow(
    Runtime *runtime,
    Handle<JSObject> iterator);

/// Create and initialize the global Object constructor. Populate the methods
/// of Object and Object.prototype.
/// \return the global Object constructor.
Handle<JSObject> createObjectConstructor(Runtime *runtime);

/// Built-in Object.prototype.toString.
CallResult<HermesValue> directObjectPrototypeToString(
    Runtime *runtime,
    Handle<> arg);

/// Create and initialize the global Error constructor, as well as all
/// the native error constructors. Populate the instance and prototype methods.
#define ALL_ERROR_TYPE(name) \
  Handle<JSObject> create##name##Constructor(Runtime *runtime);
#include "hermes/VM/NativeErrorTypes.def"

/// Create and initialize the global String constructor. Populate the methods
/// of String and String.prototype.
/// \return the global String constructor.
Handle<JSObject> createStringConstructor(Runtime *runtime);

/// Create and initialize the global Function constructor. Populate the methods
/// of Function and Function.prototype.
/// \return the global Function constructor.
Handle<JSObject> createFunctionConstructor(Runtime *runtime);

/// Create and initialize the global Number constructor. Populate the methods
/// of Number and Number.prototype.
/// \return the global Number constructor.
Handle<JSObject> createNumberConstructor(Runtime *runtime);

/// Create and initialize the global Boolean constructor. Populate the methods
/// of Boolean and Boolean.prototype.
/// \return the global Boolean constructor.
Handle<JSObject> createBooleanConstructor(Runtime *runtime);

/// Create and initialize the global Date constructor. Populate the methods
/// of Date and Date.prototype.
/// \return the global Date constructor.
Handle<JSObject> createDateConstructor(Runtime *runtime);

/// Create and initialize the global Math object, populating its value
/// and function properties.
Handle<JSObject> createMathObject(Runtime *runtime);

/// Create and initialize the global %HermesInternal object, populating its
/// value and function properties.
Handle<JSObject> createHermesInternalObject(Runtime *runtime);

#ifdef HERMES_ENABLE_DEBUGGER

/// Create and initialize the global %DebuggerInternal object, populating its
/// value and function properties.
Handle<JSObject> createDebuggerInternalObject(Runtime *runtime);

#endif // HERMES_ENABLE_DEBUGGER

/// Create and initialize the global JSON object, populating its value
/// and function properties.
Handle<JSObject> createJSONObject(Runtime *runtime);

/// Create and initialize the global RegExp constructor. Populate the methods
/// of RegExp and RegExp.prototype.
/// \return the global RegExp constructor.
Handle<JSObject> createRegExpConstructor(Runtime *runtime);

/// ES6.0 21.2.3.2.3 Runtime Semantics: RegExpCreate ( P, F )
/// Creates a new RegExp with provided pattern \p P, and flags \p F.
CallResult<Handle<JSRegExp>>
regExpCreate(Runtime *runtime, Handle<> P, Handle<> F);

/// ES6.0 21.2.5.2.1
/// Implemented in RegExp.cpp
CallResult<HermesValue>
regExpExec(Runtime *runtime, Handle<JSObject> R, Handle<StringPrimitive> S);

/// Runs the RegExp.prototype.exec() function (ES5.1 15.10.6.2)
/// with a this value of \p regexp, with the argument \p S.
/// \return a new array as the result, null pointer if there were no matches.
CallResult<Handle<JSArray>> directRegExpExec(
    Handle<JSRegExp> regexp,
    Runtime *runtime,
    Handle<StringPrimitive> S);

/// ES6.0 21.1.3.14.1
/// Implemented in RegExp.cpp
/// Transforms a replacement string by substituting $ replacement strings.
/// \p captures can be a null pointer.
CallResult<HermesValue> getSubstitution(
    Runtime *runtime,
    Handle<StringPrimitive> matched,
    Handle<StringPrimitive> str,
    uint32_t position,
    Handle<ArrayStorage> captures,
    Handle<StringPrimitive> replacement);

/// Main logic for String.prototype.split and RegExp.prototype[Symbol.split].
/// Returns an array of splitted strings.
CallResult<HermesValue> splitInternal(
    Runtime *runtime,
    Handle<> string,
    Handle<> limit,
    Handle<> separator);

/// ES6.0 21.2.5.2.3
/// If \p unicode is set and the character at \p index in \S is the start of a
/// surrogate pair, \return index + 2. Otherwise \return index + 1.
/// Note that this function does not allocate.
uint64_t
advanceStringIndex(const StringPrimitive *S, uint64_t index, bool unicode);

/// Create and initialize the global Function constructor. Populate the methods
/// of Function and Function.prototype.
/// \return the global Function constructor.
Handle<JSObject> createArrayConstructor(Runtime *runtime);

Handle<JSObject> createArrayBufferConstructor(Runtime *runtime);

Handle<JSObject> createDataViewConstructor(Runtime *runtime);

Handle<JSObject> createTypedArrayBaseConstructor(Runtime *runtime);

template <typename T, CellKind C>
Handle<JSObject> createTypedArrayConstructor(Runtime *runtime);

/// Create and initialize the global Set constructor. Populate the methods
/// of Set.prototype.
Handle<JSObject> createSetConstructor(Runtime *runtime);

/// Create SetIterator.prototype and populate methods.
Handle<JSObject> createSetIteratorPrototype(Runtime *runtime);

/// Create and initialize the global Map constructor. Populate the methods
/// of Map.prototype.
Handle<JSObject> createMapConstructor(Runtime *runtime);

/// Create MapIterator.prototype and populate methods.
Handle<JSObject> createMapIteratorPrototype(Runtime *runtime);

/// Create the WeakMap constructor and populate methods.
Handle<JSObject> createWeakMapConstructor(Runtime *runtime);

/// Create the WeakSet constructor and populate methods.
Handle<JSObject> createWeakSetConstructor(Runtime *runtime);

/// Create the Symbol constructor and populate methods.
Handle<JSObject> createSymbolConstructor(Runtime *runtime);

/// Create the GeneratorFunction constructor and populate methods.
Handle<JSObject> createGeneratorFunctionConstructor(Runtime *runtime);

/// Create the IteratorPrototype.
void populateIteratorPrototype(Runtime *runtime);

/// Create the ArrayIterator prototype.
void populateArrayIteratorPrototype(Runtime *runtime);

/// Create the StringIterator prototype.
void populateStringIteratorPrototype(Runtime *runtime);

/// Create the %GeneratorPrototype%.
void populateGeneratorPrototype(Runtime *runtime);

/// ES19.2.1.1.1. Create a new function given arguments and a body.
/// \param isGeneratorFunction when true, make a generator with "function*".
CallResult<HermesValue> createDynamicFunction(
    Runtime *runtime,
    NativeArgs args,
    bool isGeneratorFunction);

/// A direct passthrough to call eval() on \p str.
CallResult<HermesValue> directEval(
    Runtime *runtime,
    Handle<StringPrimitive> str,
    const ScopeChain &scopeChain,
    bool singleFunction = false);

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_JSLIB_JSLIBINTERNAL_H
