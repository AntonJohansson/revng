#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

revng_add_analyses_library_internal(
  revngABI DefaultFunctionPrototype.cpp FunctionType.cpp
  RegisterStateDeductions.cpp)

target_link_libraries(revngABI revngSupport revngModel ${LLVM_LIBRARIES})
