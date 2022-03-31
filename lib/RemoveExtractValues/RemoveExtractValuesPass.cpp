//
// Copyright rev.ng Srls. See LICENSE.md for details.
//

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "revng/Support/Assert.h"
#include "revng/Support/FunctionTags.h"
#include "revng/Support/IRHelpers.h"
#include "revng/Support/OpaqueFunctionsPool.h"

#include "revng-c/RemoveExtractValues/RemoveExtractValuesPass.h"
#include "revng-c/Support/FunctionTags.h"

using namespace llvm;

char RemoveExtractValues::ID = 0;
using Reg = RegisterPass<RemoveExtractValues>;
static Reg X("remove-extractvalues",
             "Substitute extractvalues with opaque calls so that they don't "
             "get optimized",
             true,
             true);

bool RemoveExtractValues::runOnFunction(llvm::Function &F) {
  using namespace llvm;

  // Collect all ExtractValues
  SmallVector<ExtractValueInst *, 16> ToReplace;
  for (auto &BB : F)
    for (auto &I : BB)
      if (auto *ExtractVal = llvm::dyn_cast<llvm::ExtractValueInst>(&I))
        ToReplace.push_back(ExtractVal);

  if (ToReplace.empty())
    return false;

  // Create a pool of functions with the same behavior: we will need a different
  // function for each different struct
  OpaqueFunctionsPool<llvm::Type *>
    OpaqueEVPool(F.getParent(), /* PurgeOnDestruction */ false);
  initOpaqueEVPool(OpaqueEVPool);

  llvm::LLVMContext &LLVMCtx = F.getContext();
  IRBuilder<> Builder(LLVMCtx);
  for (ExtractValueInst *I : ToReplace) {
    Builder.SetInsertPoint(I);

    // Collect arguments of the ExtractValue
    SmallVector<Value *, 8> ArgValues = { I->getAggregateOperand() };
    for (auto Idx : I->indices()) {
      auto *IndexVal = ConstantInt::get(IntegerType::getInt64Ty(LLVMCtx), Idx);
      ArgValues.push_back(IndexVal);
    }

    // Get or generate the function
    auto *EVFunctionType = getOpaqueEVFunctionType(F.getContext(), I);
    auto *ExtractValueFunction = OpaqueEVPool.get(EVFunctionType,
                                                  EVFunctionType,
                                                  "OpaqueExtractvalue");

    // Emit a call to the new function
    CallInst *InjectedCall = Builder.CreateCall(ExtractValueFunction,
                                                ArgValues);

    I->replaceAllUsesWith(InjectedCall);
    InjectedCall->copyMetadata(*I);
    I->eraseFromParent();
  }

  return true;
}
