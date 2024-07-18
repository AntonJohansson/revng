//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: not %revngcliftopt %s 2>&1 | FileCheck %s

!int32_t = !clift.primitive<SignedKind 4>

%i = clift.undef : !int32_t

// CHECK: expected Clift function or pointer-to-function type
clift.call %i() : !int32_t