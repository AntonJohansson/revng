#pragma once

//
// Copyright (c) rev.ng Labs Srl. See LICENSE.md for details.
//

#include "llvm/Pass.h"

#include "revng/Support/FunctionTags.h"

extern FunctionTags::Tag StackOffsetMarker;

/// Wrap stack accesses into StackOffsetMarker-tagged calls
///
/// stack_offset is an identity-like function with a second argument
/// representing the range of offsets from SP0 that the pointer passed in as the
/// first argument can assume.
///
/// \note This pass looks for all the users of `revng_init_local_sp`, therefore
/// it needs to be run *after* `PromoteStackPointerPass`
struct InstrumentStackAccessesPass : public llvm::ModulePass {
public:
  static char ID;

  InstrumentStackAccessesPass() : llvm::ModulePass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

  bool runOnModule(llvm::Module &M) override;
};
