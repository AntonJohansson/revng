#pragma once

//
// Copyright rev.ng Srls. See LICENSE.md for details.
//

namespace llvm {

class Function;

} // end namespace llvm

class ASTTree;

extern void beautifyAST(llvm::Function &F, ASTTree &CombedAST);
