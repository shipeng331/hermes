/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc -O0 -dump-ir %s | %FileCheck --match-full-lines %s

function foo(a, b, c) {
    return {a, ...b, c};
}
//CHECK-LABEL: function foo(a, b, c)
//CHECK-NEXT: frame = [a, b, c]
//CHECK-NEXT: %BB0:
//CHECK-NEXT:   %0 = StoreFrameInst %a, [a]
//CHECK-NEXT:   %1 = StoreFrameInst %b, [b]
//CHECK-NEXT:   %2 = StoreFrameInst %c, [c]
//CHECK-NEXT:   %3 = AllocObjectInst 2 : number, empty
//CHECK-NEXT:   %4 = LoadFrameInst [a]
//CHECK-NEXT:   %5 = StoreNewOwnPropertyInst %4, %3 : object, "a" : string, true : boolean
//CHECK-NEXT:   %6 = LoadFrameInst [b]
//CHECK-NEXT:   %7 = TryLoadGlobalPropertyInst globalObject : object, "HermesInternal" : string
//CHECK-NEXT:   %8 = LoadPropertyInst %7, "copyDataProperties" : string
//CHECK-NEXT:   %9 = CallInst %8, undefined : undefined, %3 : object, %6
//CHECK-NEXT:   %10 = LoadFrameInst [c]
//CHECK-NEXT:   %11 = StoreOwnPropertyInst %10, %3 : object, "c" : string, true : boolean
//CHECK-NEXT:   %12 = ReturnInst %3 : object
//CHECK-NEXT: %BB1:
//CHECK-NEXT:   %13 = ReturnInst undefined : undefined
//CHECK-NEXT: function_end
