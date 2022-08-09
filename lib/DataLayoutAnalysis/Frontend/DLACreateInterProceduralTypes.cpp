//
// Copyright (c) rev.ng Labs Srl. See LICENSE.md for details.
//

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"

#include "revng/Model/Binary.h"
#include "revng/Model/IRHelpers.h"
#include "revng/Support/Assert.h"
#include "revng/Support/Debug.h"
#include "revng/Support/FunctionTags.h"
#include "revng/Support/IRHelpers.h"

#include "revng-c/DataLayoutAnalysis/DLATypeSystem.h"
#include "revng-c/Support/FunctionTags.h"
#include "revng-c/Support/IRHelpers.h"

#include "../FuncOrCallInst.h"
#include "DLATypeSystemBuilder.h"

using namespace dla;
using namespace llvm;

using TSBuilder = DLATypeSystemLLVMBuilder;

bool TSBuilder::createInterproceduralTypes(llvm::Module &M,
                                           const model::Binary &Model) {
  for (const Function &F : M.functions()) {

    auto FTags = FunctionTags::TagsSet::from(&F);
    // Skip intrinsics
    if (F.isIntrinsic())
      continue;

    // Ignore everything that is not isolated or dynamic
    if (not FunctionTags::Isolated.isTagOf(&F)
        and not FunctionTags::DynamicFunction.isTagOf(&F))
      continue;

    revng_assert(not F.isVarArg());

    // Check if a function with the same prototype has already been visited
    const model::Type *Prototype = nullptr;
    if (FunctionTags::Isolated.isTagOf(&F)) {
      const model::Function *ModelFunc = llvmToModelFunction(Model, F);
      Prototype = ModelFunc->Prototype.getConst();
    } else {
      llvm::StringRef SymbolName = F.getName().drop_front(strlen("dynamic_"));

      auto It = Model.ImportedDynamicFunctions.find(SymbolName.str());
      revng_assert(It != Model.ImportedDynamicFunctions.end());
      const model::DynamicFunction &DF = *It;
      const auto &TTR = getPrototype(Model, DF);
      revng_assert(TTR.isValid());
      Prototype = TTR.getConst();
    }
    revng_assert(Prototype);

    FuncOrCallInst FuncWithSameProto;
    auto It = VisitedPrototypes.find(Prototype);
    if (It == VisitedPrototypes.end())
      VisitedPrototypes.insert({ Prototype, &F });
    else
      FuncWithSameProto = It->second;

    // Create the Function's return types
    auto FRetTypes = getOrCreateLayoutTypes(F);
    // Add equality links between return values of function with the same
    // prototype
    if (not FuncWithSameProto.isNull()) {
      auto OtherRetVals = getLayoutTypes(*FuncWithSameProto.getVal());
      revng_assert(FRetTypes.size() == OtherRetVals.size());
      for (auto [N1, N2] : llvm::zip(OtherRetVals, FRetTypes))
        TS.addEqualityLink(N1, N2.first);
    }

    revng_assert(FuncWithSameProto.isNull()
                 or F.arg_size() == FuncWithSameProto.arg_size());

    // Create types for the Function's arguments
    for (const auto &Arg : llvm::enumerate(F.args())) {
      // Arguments can only be integers and pointers
      auto &ArgVal = Arg.value();
      revng_assert(isa<IntegerType>(ArgVal.getType())
                   or isa<PointerType>(ArgVal.getType()));
      auto [ArgNode, _] = getOrCreateLayoutType(&ArgVal);
      revng_assert(ArgNode);

      // If there is already a Function with the same prototype, add equality
      // edges between args
      if (not FuncWithSameProto.isNull()) {
        auto &OtherArg = *(FuncWithSameProto.getArg(Arg.index()));
        auto *OtherArgNode = getLayoutType(&OtherArg);
        revng_assert(OtherArgNode);
        TS.addEqualityLink(ArgNode, OtherArgNode);
      }
    }

    for (const BasicBlock &B : F) {
      for (const Instruction &I : B) {
        if (auto *Call = isCallToIsolatedFunction(&I)) {

          const Function *Callee = getCallee(Call);
          if (not Callee)
            continue;

          unsigned ArgNo = 0U;
          for (const Use &ArgUse : Call->args()) {

            // Create the layout for the call arguments
            const auto *ActualArg = ArgUse.get();
            revng_assert(isa<IntegerType>(ActualArg->getType())
                         or isa<PointerType>(ActualArg->getType()));
            auto ActualTypes = getOrCreateLayoutTypes(*ActualArg);

            // Create the layout for the formal arguments.
            Value *FormalArg = Callee->getArg(ArgNo);
            revng_assert(isa<IntegerType>(FormalArg->getType())
                         or isa<PointerType>(FormalArg->getType()));
            auto FormalTypes = getOrCreateLayoutTypes(*FormalArg);
            revng_assert(1ULL == ActualTypes.size() == FormalTypes.size());

            auto FieldNum = FormalTypes.size();
            for (auto FieldId = 0ULL; FieldId < FieldNum; ++FieldId) {
              TS.addInstanceLink(ActualTypes[FieldId].first,
                                 FormalTypes[FieldId].first,
                                 OffsetExpression{});
              auto *Placeholder = TS.createArtificialLayoutType();
              Placeholder->Size = getPointerSize(Model.Architecture);
              TS.addPointerLink(Placeholder, ActualTypes[FieldId].first);
            }
            ++ArgNo;
          }
        } else if (auto *PHI = dyn_cast<PHINode>(&I)) {
          revng_assert(isa<IntegerType>(PHI->getType())
                       or isa<PointerType>(PHI->getType())
                       or isa<StructType>(PHI->getType()));
          auto PHITypes = getOrCreateLayoutTypes(*PHI);
          for (const Use &Incoming : PHI->incoming_values()) {
            revng_assert(isa<IntegerType>(Incoming->getType())
                         or isa<PointerType>(Incoming->getType())
                         or isa<StructType>(Incoming->getType()));
            auto InTypes = getOrCreateLayoutTypes(*Incoming.get());
            revng_assert(PHITypes.size() == InTypes.size());
            revng_assert((PHITypes.size() == 1ULL)
                         or isa<StructType>(PHI->getType()));
            auto FieldNum = PHITypes.size();
            for (auto FieldId = 0ULL; FieldId < FieldNum; ++FieldId) {
              TS.addInstanceLink(InTypes[FieldId].first,
                                 PHITypes[FieldId].first,
                                 OffsetExpression{});
            }
          }
        } else if (auto *RetI = dyn_cast<ReturnInst>(&I)) {
          if (Value *RetVal = RetI->getReturnValue()) {
            revng_assert(isa<StructType>(RetVal->getType())
                         or isa<IntegerType>(RetVal->getType())
                         or isa<PointerType>(RetVal->getType()));
            auto RetTypes = getOrCreateLayoutTypes(*RetVal);
            revng_assert(RetTypes.size() == FRetTypes.size());
            auto FieldNum = RetTypes.size();
            for (auto FieldId = 0ULL; FieldId < FieldNum; ++FieldId) {
              if (RetTypes[FieldId].first != nullptr) {
                TS.addInstanceLink(RetTypes[FieldId].first,
                                   FRetTypes[FieldId].first,
                                   OffsetExpression{});
                auto *Placeholder = TS.createArtificialLayoutType();
                Placeholder->Size = getPointerSize(Model.Architecture);
                TS.addPointerLink(Placeholder, RetTypes[FieldId].first);
              }
            }
          }
        }
      }
    }
  }

  // Create types for segments
  std::map<model::Segment *, LayoutTypeSystemNode *> SegmentNodeMap;
  for (Function &F : FunctionTags::SegmentRef.functions(&M)) {
    const auto &[StartAddress, VirtualSize] = extractSegmentKeyFromMetadata(F);
    model::Segment Segment = Model.Segments.at({ StartAddress, VirtualSize });
    auto [SegmentTypeNode, _] = getOrCreateLayoutType(&F);
    auto [It, Success] = SegmentNodeMap.insert({ &Segment, SegmentTypeNode });

    // Already inserted? Reference the Node of the current function to the
    // found segment, otherwise emit a placeholder for a new node in order to
    // prevent DLA's middle-end from doing certain optimizations.
    if (Success == false) {
      TS.addEqualityLink(SegmentTypeNode, It->second);
    } else {
      auto *Placeholder = TS.createArtificialLayoutType();
      Placeholder->Size = getPointerSize(Model.Architecture);
      TS.addPointerLink(Placeholder, SegmentTypeNode);
    }

    for (const Use &U : F.uses()) {
      auto *Call = cast<CallInst>(U.getUser());
      auto [SegmentRefCallNode, _] = getOrCreateLayoutType(Call);
      TS.addEqualityLink(SegmentTypeNode, SegmentRefCallNode);
    }
  }

  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyConsistency());
  return TS.getNumLayouts() != 0;
}
