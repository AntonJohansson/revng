#ifndef STACKANALYSIS_H
#define STACKANALYSIS_H

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Standard includes
#include <string>

// LLVM includes
#include "llvm/Pass.h"

// Local libraries includes
#include "revng/BasicAnalyses/FunctionCallIdentification.h"
#include "revng/BasicAnalyses/GeneratedCodeBasicInfo.h"
#include "revng/StackAnalysis/FunctionsSummary.h"

namespace StackAnalysis {

template<bool AnalyzeABI>
class StackAnalysis : public llvm::FunctionPass {

public:
  static char ID;

public:
  StackAnalysis() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<GeneratedCodeBasicInfo>();
  }

  bool runOnFunction(llvm::Function &F) override;

  const std::set<const llvm::GlobalVariable *> &
  getClobbered(llvm::BasicBlock *Function) {
    return GrandResult.Functions[Function].ClobberedRegisters;
  }

  void serialize(std::ostream &Output) { Output << TextRepresentation; }

private:
  FunctionsSummary GrandResult;
  std::string TextRepresentation;
};

template<>
char StackAnalysis<true>::ID;

template<>
char StackAnalysis<false>::ID;

} // namespace StackAnalysis

#endif // STACKANALYSIS_H