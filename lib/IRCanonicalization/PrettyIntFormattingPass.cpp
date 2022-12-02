//
// Copyright rev.ng Srls. See LICENSE.md for details.
//

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "llvm/IR/Attributes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Pass.h"

#include "revng/Model/IRHelpers.h"
#include "revng/Support/OpaqueFunctionsPool.h"

#include "revng-c/InitModelTypes/InitModelTypes.h"
#include "revng-c/Support/FunctionTags.h"
#include "revng-c/Support/IRHelpers.h"

enum class IntFormatting : uint32_t {
  NONE, // no formatting
  HEX,
  CHAR,
  BOOL
};

struct FormatInt {
  IntFormatting Formatting;
  llvm::Instruction *Instruction;
  llvm::Use *Use;
};

static std::optional<FormatInt>
getIntFormat(llvm::Instruction &I, llvm::Use &U);

struct PrettyIntFormatting : public llvm::FunctionPass {
public:
  static char ID;

  PrettyIntFormatting() : llvm::FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;
};

bool PrettyIntFormatting::runOnFunction(llvm::Function &F) {
  if (!FunctionTags::TagsSet::from(&F).contains(FunctionTags::Isolated)) {
    return false;
  }

  OpaqueFunctionsPool<llvm::Type *> HexIntegerPool(F.getParent(), false);
  initHexPrintPool(HexIntegerPool);

  OpaqueFunctionsPool<llvm::Type *> CharIntegerPool(F.getParent(), false);
  initCharPrintPool(CharIntegerPool);

  OpaqueFunctionsPool<llvm::Type *> BoolIntegerPool(F.getParent(), false);
  initBoolPrintPool(BoolIntegerPool);

  std::vector<FormatInt> IntsToBeFormatted;

  for (llvm::Instruction &I : llvm::instructions(F)) {
    for (llvm::Use &U : I.operands()) {
      if (auto formatting = getIntFormat(I, U); formatting) {
        IntsToBeFormatted.push_back(*formatting);
      }
    }
  }

  llvm::IRBuilder<> Builder(F.getContext());
  for (const auto &[Format, Instruction, Operand] : IntsToBeFormatted) {
    llvm::Value *Val = Operand->get();

    auto *OpVal = Instruction->getOperand(Operand->getOperandNo());
    llvm::Type *IntType = OpVal->getType();

    auto PrettyFunction = [&, Format = Format]() -> llvm::Function * {
      switch (Format) {
      case IntFormatting::HEX:
        return HexIntegerPool.get(IntType, IntType, { IntType }, "print_hex");
      case IntFormatting::CHAR:
        return CharIntegerPool.get(IntType, IntType, { IntType }, "print_char");
      case IntFormatting::BOOL:
        return BoolIntegerPool.get(IntType, IntType, { IntType }, "print_bool");
      case IntFormatting::NONE:
      default:
        return nullptr;
      }
      return nullptr;
    }();

    if (PrettyFunction) {
      Builder.SetInsertPoint(Instruction);
      llvm::Value *Call = Builder.CreateCall(PrettyFunction, { Val });
      Instruction->setOperand(Operand->getOperandNo(), Call);
    }
  }

  return true;
}

std::optional<FormatInt> getIntFormat(llvm::Instruction &I, llvm::Use &U) {
  auto &Context = I.getContext();

  // We cannot print properly characters when they are part of switch
  // instruction, because cases in LLVM switch cannot have variables inside.
  if (I.getOpcode() == llvm::Instruction::Switch) {
    return std::nullopt;
  }

  // We skip AssignmentMarkers as we require constant bool as a second argument.
  // Replacing that constant with something make some assertions failing.
  if (isCallToTagged(&I, FunctionTags::AssignmentMarker)) {
    return std::nullopt;
  }

  // Some intrinsic calls require ConstantInt as an argument so we are not able
  // to pass there any decorated value.
  if (auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&I)) {
    if (Intrinsic->getIntrinsicID() == llvm::Intrinsic::abs) {
      return std::nullopt;
    }
  }

  // We want to print ints in hex format when they are left operand of shifts or
  // operands of and/or/xor instructions.
  if (isa<llvm::ConstantInt>(U)) {
    if (I.getOpcode() == llvm::Instruction::Shl
        || I.getOpcode() == llvm::Instruction::AShr
        || I.getOpcode() == llvm::Instruction::LShr) {
      if (U.getOperandNo() == 0) {
        return FormatInt{ IntFormatting::HEX, &I, &U };
      }
    } else if (I.getOpcode() == llvm::Instruction::And
               || I.getOpcode() == llvm::Instruction::Or
               || I.getOpcode() == llvm::Instruction::Xor) {
      return FormatInt{ IntFormatting::HEX, &I, &U };
    }

    if (U->getType() == llvm::IntegerType::getInt8Ty(Context)) {
      return FormatInt{ IntFormatting::CHAR, &I, &U };
    }

    if (U->getType() == llvm::IntegerType::getInt1Ty(Context)) {
      return FormatInt{ IntFormatting::BOOL, &I, &U };
    }
  }

  return std::nullopt;
}

char PrettyIntFormatting::ID = 0;

llvm::RegisterPass<PrettyIntFormatting> X("pretty-int-formatting",
                                          "Wraps integers with decorator "
                                          "functions which informs backend "
                                          "about literal type that should be "
                                          "used",
                                          false,
                                          false);
