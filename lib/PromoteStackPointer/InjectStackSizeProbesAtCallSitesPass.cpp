//
// Copyright (c) rev.ng Srls. See LICENSE.md for details.
//

#include "llvm/IR/IRBuilder.h"

#include "revng/BasicAnalyses/GeneratedCodeBasicInfo.h"
#include "revng/Support/FunctionTags.h"

#include "revng-c/PromoteStackPointer/InjectStackSizeProbesAtCallSitesPass.h"

using namespace llvm;

bool InjectStackSizeProbesAtCallSitesPass::runOnModule(llvm::Module &M) {
  bool Changed = false;
  IRBuilder<> B(M.getContext());

  // Get the stack pointer CSV
  auto &GCBI = getAnalysis<GeneratedCodeBasicInfoWrapperPass>().getGCBI();
  auto *SP = GCBI.spReg();
  auto *SPType = SP->getType()->getPointerElementType();

  // Create marker for recording stack heigh at each call site
  auto *SSACSType = llvm::FunctionType::get(B.getVoidTy(), { SPType }, false);
  auto SSACS = M.getOrInsertFunction("stack_size_at_call_site", SSACSType);
  auto *F = cast<Function>(SSACS.getCallee());
  F->addFnAttr(Attribute::NoUnwind);
  F->addFnAttr(Attribute::WillReturn);
  F->addFnAttr(Attribute::InaccessibleMemOnly);

  for (Function &F : FunctionTags::Isolated.functions(&M)) {
    setInsertPointToFirstNonAlloca(B, F);

    auto *SP0 = B.CreateLoad(SP);

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *MD = I.getMetadata("revng.callerblock.start")) {
          // We found a function call
          Changed = true;
          B.SetInsertPoint(&I);

          // Inject a call to the marker. First argument is sp - sp0
          auto *Call = B.CreateCall(SSACS, B.CreateSub(SP0, B.CreateLoad(SP)));
          Call->setMetadata("revng.callerblock.start", MD);
        }
      }
    }
  }

  return Changed;
}

using MSSACSP = InjectStackSizeProbesAtCallSitesPass;
void MSSACSP::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GeneratedCodeBasicInfoWrapperPass>();
  AU.setPreservesCFG();
}

char InjectStackSizeProbesAtCallSitesPass::ID = 0;

using RegisterMSSACS = RegisterPass<InjectStackSizeProbesAtCallSitesPass>;
static RegisterMSSACS R("measure-stack-size-at-call-sites",
                        "Measure Stack Size At Call Sites Pass");
