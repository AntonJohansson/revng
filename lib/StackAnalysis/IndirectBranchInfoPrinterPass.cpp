/// \file IndirectBranchInfoPrinterPass.cpp
/// \brief Serialize the results of the StackAnalysis on disk.

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdio>

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Support/CommandLine.h"

#include "revng/BasicAnalyses/GeneratedCodeBasicInfo.h"
#include "revng/StackAnalysis/IndirectBranchInfoPrinterPass.h"
#include "revng/Support/Assert.h"
#include "revng/Support/IRHelpers.h"

using namespace llvm;
using IBIPP = IndirectBranchInfoPrinterPass;

PreservedAnalyses IBIPP::run(Function &F, FunctionAnalysisManager &FAM) {
  auto &M = *F.getParent();

  for (auto *Call : callers(M.getFunction("indirect_branch_info")))
    if (Call->getParent()->getParent() == &F)
      serialize(Call);

  return PreservedAnalyses::all();
}

void IBIPP::serialize(CallBase *Call) {
  OS << Call->getParent()->getParent()->getName();
  for (unsigned I = 0; I < Call->getNumArgOperands(); ++I) {
    if (isa<ConstantInt>(Call->getArgOperand(I))) {
      OS << "," << cast<ConstantInt>(Call->getArgOperand(I))->getSExtValue();
    } else if (isa<StructType>(Call->getArgOperand(I)->getType())) {
      auto PC = MetaAddress::fromConstant(Call->getArgOperand(I));
      OS << "," << PC.address();
    } else {
      OS << ",unknown";
    }
  }
  OS << "\n";
}