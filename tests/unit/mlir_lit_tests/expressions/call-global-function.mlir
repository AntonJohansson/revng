//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %revngcliftopt %s

!int32_t = !clift.primitive<SignedKind 4>

!f = !clift.defined<#clift.function<id = 1,
                                    name = "",
                                    return_type = !int32_t,
                                    argument_types = []>>

!g = !clift.defined<#clift.function<id = 2,
                                    name = "",
                                    return_type = !int32_t,
                                    argument_types = [!int32_t]>>

clift.module {
  clift.func @f<!f>() -> !int32_t {
    clift.return {
      %g = clift.use @g : !g
      %i = clift.imm 1 : !int32_t
      %r = clift.call %g(%i) : !g
      clift.yield %r : !int32_t
    }
  }

  clift.func @g<!g>(!int32_t) -> !int32_t
}