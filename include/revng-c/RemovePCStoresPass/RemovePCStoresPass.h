#ifndef REMOVEPCSTORESPASS_H
#define REMOVEPCSTORESPASS_H

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Standard includes
#include <fstream>

// LLVM includes
#include "llvm/Pass.h"

class RemovePCStores : public llvm::FunctionPass {
public:
  static char ID;

public:
  RemovePCStores() : llvm::FunctionPass(ID) { }

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }

};

#endif // REMOVEPCSTORESPASS_H
