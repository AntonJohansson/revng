/// \file Binary.cpp
/// \brief

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/raw_os_ostream.h"

#include "revng/ADT/GenericGraph.h"
#include "revng/Model/Binary.h"
#include "revng/Model/VerifyHelper.h"
#include "revng/Support/OverflowSafeInt.h"

using namespace llvm;

namespace model {

struct FunctionCFGNodeData {
  FunctionCFGNodeData(MetaAddress Start) : Start(Start) {}
  MetaAddress Start;
};

using FunctionCFGNode = ForwardNode<FunctionCFGNodeData>;

/// Graph data structure to represent the CFG for verification purposes
struct FunctionCFG : public GenericGraph<FunctionCFGNode> {
private:
  MetaAddress Entry;
  std::map<MetaAddress, FunctionCFGNode *> Map;

public:
  FunctionCFG(MetaAddress Entry) : Entry(Entry) {}

public:
  MetaAddress entry() const { return Entry; }
  FunctionCFGNode *entryNode() const { return Map.at(Entry); }

public:
  FunctionCFGNode *get(MetaAddress MA) {
    FunctionCFGNode *Result = nullptr;
    auto It = Map.find(MA);
    if (It == Map.end()) {
      Result = addNode(MA);
      Map[MA] = Result;
    } else {
      Result = It->second;
    }

    return Result;
  }

  bool allNodesAreReachable() const {
    if (Map.size() == 0)
      return true;

    // Ensure all the nodes are reachable from the entry node
    df_iterator_default_set<FunctionCFGNode *> Visited;
    for (auto &Ignore : depth_first_ext(entryNode(), Visited))
      ;
    return Visited.size() == size();
  }

  bool hasOnlyInvalidExits() const {
    for (auto &[Address, Node] : Map)
      if (Address.isValid() and not Node->hasSuccessors())
        return false;
    return true;
  }
};

static FunctionCFG getGraph(const Binary &Binary, const Function &F) {
  using namespace FunctionEdgeType;

  FunctionCFG Graph(F.Entry);
  for (const BasicBlock &Block : F.CFG) {
    auto *Source = Graph.get(Block.Start);

    for (const auto &Edge : Block.Successors) {
      switch (Edge->Type) {
      case DirectBranch:
      case FakeFunctionCall:
      case FakeFunctionReturn:
      case Return:
      case BrokenReturn:
      case IndirectTailCall:
      case LongJmp:
      case Unreachable:
        Source->addSuccessor(Graph.get(Edge->Destination));
        break;

      case FunctionCall:
      case IndirectCall: {
        auto *CE = cast<model::CallEdge>(Edge.get());
        if (hasAttribute(Binary, *CE, model::FunctionAttribute::NoReturn))
          Source->addSuccessor(Graph.get(MetaAddress::invalid()));
        else
          Source->addSuccessor(Graph.get(Block.End));
        break;
      }

      case Killer:
        Source->addSuccessor(Graph.get(MetaAddress::invalid()));
        break;

      case Invalid:
      case Count:
        revng_abort();
        break;
      }
    }
  }

  return Graph;
}

model::TypePath
Binary::getPrimitiveType(PrimitiveTypeKind::Values V, uint8_t ByteSize) {
  PrimitiveType Temporary(V, ByteSize);
  Type::Key PrimitiveKey{ TypeKind::Primitive, Temporary.ID };
  auto It = Types.find(PrimitiveKey);

  // If we couldn't find it, create it
  if (It == Types.end()) {
    auto *NewPrimitiveType = new PrimitiveType(V, ByteSize);
    It = Types.insert(UpcastablePointer<model::Type>(NewPrimitiveType)).first;
  }

  return getTypePath(It->get());
}

model::TypePath
Binary::getPrimitiveType(PrimitiveTypeKind::Values V, uint8_t ByteSize) const {
  PrimitiveType Temporary(V, ByteSize);
  Type::Key PrimitiveKey{ TypeKind::Primitive, Temporary.ID };
  return getTypePath(Types.at(PrimitiveKey).get());
}

TypePath Binary::recordNewType(UpcastablePointer<Type> &&T) {
  auto It = Types.insert(T).first;
  return getTypePath(It->get());
}

void Binary::dumpCFG(const Function &F) const {
  FunctionCFG CFG = getGraph(*this, F);
  raw_os_ostream Stream(dbg);
  WriteGraph(Stream, &CFG);
}

bool Binary::verifyTypes() const {
  return verifyTypes(false);
}

bool Binary::verifyTypes(bool Assert) const {
  VerifyHelper VH(Assert);
  return verifyTypes(VH);
}

bool Binary::verifyTypes(VerifyHelper &VH) const {
  // All types on their own should verify
  std::set<Identifier> Names;
  for (auto &Type : Types) {
    // Verify the type
    if (not Type.get()->verify(VH))
      return VH.fail();

    // Ensure the names are unique
    auto Name = Type->name();
    if (not Names.insert(Name).second)
      return VH.fail(Twine("Multiple types with the following name: ") + Name);
  }

  return true;
}

void Binary::dump() const {
  serialize(dbg, *this);
}

std::string Binary::toString() const {
  std::string S;
  llvm::raw_string_ostream OS(S);
  serialize(OS, *this);
  return S;
}

bool Binary::verify() const {
  return verify(false);
}

bool Binary::verify(bool Assert) const {
  VerifyHelper VH(Assert);
  return verify(VH);
}

bool Binary::verify(VerifyHelper &VH) const {
  // Prepare for checking symbol names. We will populate and check this against
  // functions, dynamic functions, types and enum entries
  std::set<Identifier> Symbols;
  auto CheckCustomName = [&VH, &Symbols, this](const Identifier &CustomName) {
    if (CustomName.empty())
      return true;

    return VH.maybeFail(Symbols.insert(CustomName).second,
                        "Duplicate name: " + CustomName.str().str(),
                        *this);
  };

  for (const Function &F : Functions) {
    // Verify individual functions
    if (not F.verify(VH))
      return VH.fail();

    if (not CheckCustomName(F.CustomName))
      return VH.fail("Duplicate name", F);

    // Populate graph
    FunctionCFG Graph = getGraph(*this, F);

    // Ensure all the nodes are reachable from the entry node
    if (not Graph.allNodesAreReachable())
      return VH.fail();

    // Ensure the only node with no successors is invalid
    if (not Graph.hasOnlyInvalidExits())
      return VH.fail();

    // Check function calls
    for (const BasicBlock &Block : F.CFG) {
      for (const auto &Edge : Block.Successors) {

        if (Edge->Type == model::FunctionEdgeType::FunctionCall) {
          // We're in a direct call, get the callee
          const auto *Call = dyn_cast<CallEdge>(Edge.get());

          if (not Call->DynamicFunction.empty()) {
            // It's a dynamic call

            if (Call->Destination.isValid()) {
              return VH.fail("Destination must be invalid for dynamic function "
                             "calls");
            }

            auto It = ImportedDynamicFunctions.find(Call->DynamicFunction);

            // If missing, fail
            if (It == ImportedDynamicFunctions.end())
              return VH.fail("Can't find callee \"" + Call->DynamicFunction
                             + "\"");
          } else {
            // Regular call
            auto It = Functions.find(Call->Destination);

            // If missing, fail
            if (It == Functions.end())
              return VH.fail("Can't find callee");
          }
        }
      }
    }
  }

  // Verify DynamicFunctions
  for (const DynamicFunction &DF : ImportedDynamicFunctions) {
    if (not DF.verify(VH))
      return VH.fail();

    if (not CheckCustomName(DF.CustomName))
      return VH.fail();
  }

  for (auto &Type : Types) {
    if (not CheckCustomName(Type->CustomName))
      return VH.fail();

    if (auto *Enum = dyn_cast<EnumType>(Type.get()))
      for (auto &Entry : Enum->Entries)
        if (not CheckCustomName(Entry.CustomName))
          return VH.fail();
  }

  //
  // Verify the type system
  //
  return verifyTypes(VH);
}

Identifier Function::name() const {
  using llvm::Twine;
  if (not CustomName.empty()) {
    return CustomName;
  } else {
    // TODO: this prefix needs to be reserved
    auto AutomaticName = (Twine("function_") + Entry.toString()).str();
    return Identifier::fromString(AutomaticName);
  }
}

static const model::TypePath &
prototypeOr(const model::TypePath &Prototype, const model::TypePath &Default) {
  if (Prototype.isValid())
    return Prototype;

  revng_assert(Default.isValid());
  return Default;
}

const model::TypePath &Function::prototype(const model::Binary &Root) const {
  return prototypeOr(Prototype, Root.DefaultPrototype);
}

Identifier DynamicFunction::name() const {
  using llvm::Twine;
  if (not CustomName.empty()) {
    return CustomName;
  } else {
    // TODO: this prefix needs to be reserved
    auto AutomaticName = (Twine("dynamic_function_") + OriginalName).str();
    return Identifier::fromString(AutomaticName);
  }
}

const model::TypePath &
DynamicFunction::prototype(const model::Binary &Root) const {
  return prototypeOr(Prototype, Root.DefaultPrototype);
}

bool Relocation::verify() const {
  return verify(false);
}

bool Relocation::verify(bool Assert) const {
  VerifyHelper VH(Assert);
  return verify(VH);
}

bool Relocation::verify(VerifyHelper &VH) const {
  if (Type == model::RelocationType::Invalid)
    return VH.fail("Invalid relocation", *this);

  return true;
}

bool Section::verify() const {
  return verify(false);
}

bool Section::verify(bool Assert) const {
  VerifyHelper VH(Assert);
  return verify(VH);
}

bool Section::verify(VerifyHelper &VH) const {
  auto EndAddress = StartAddress + Size;
  if (not EndAddress.isValid())
    return VH.fail("Computing the end address leads to overflow");

  return true;
}

bool Segment::verify() const {
  return verify(false);
}

bool Segment::verify(bool Assert) const {
  VerifyHelper VH(Assert);
  return verify(VH);
}

bool Segment::verify(VerifyHelper &VH) const {
  using OverflowSafeInt = OverflowSafeInt<uint64_t>;

  if (FileSize > VirtualSize)
    return VH.fail("FileSize cannot be larger than VirtualSize", *this);

  auto EndOffset = OverflowSafeInt(StartOffset) + FileSize;
  if (not EndOffset)
    return VH.fail("Computing the segment end offset leads to overflow", *this);

  auto EndAddress = StartAddress + VirtualSize;
  if (not EndAddress.isValid())
    return VH.fail("Computing the end address leads to overflow", *this);

  for (const model::Section &Section : Sections) {
    if (not Section.verify(VH))
      return VH.fail("Invalid section", Section);

    if (not contains(Section.StartAddress)
        or (VirtualSize > 0 and not contains(Section.endAddress() - 1))) {
      return VH.fail("The segment contains a section out of its boundaries",
                     Section);
    }

    if (Section.ContainsCode and not IsExecutable) {
      return VH.fail("A Section is marked as containing code but the "
                     "containing segment is not executable",
                     *this);
    }
  }

  for (const model::Relocation &Relocation : Relocations) {
    if (not Relocation.verify(VH))
      return VH.fail("Invalid relocation", Relocation);

    if (not contains(Relocation.Address)
        or not contains(Relocation.endAddress())) {
      return VH.fail("The segment contains a relocation out of its boundaries",
                     Relocation);
    }
  }

  return true;
}

void Function::dump() const {
  serialize(dbg, *this);
}

bool Function::verify() const {
  return verify(false);
}

bool Function::verify(bool Assert) const {
  VerifyHelper VH(Assert);
  return verify(VH);
}

bool Function::verify(VerifyHelper &VH) const {
  if (Type == FunctionType::Fake or Type == FunctionType::Invalid)
    return VH.maybeFail(CFG.size() == 0);

  // Verify blocks
  if (CFG.size() > 0) {
    bool HasEntry = false;
    for (const BasicBlock &Block : CFG) {

      if (Block.Start == Entry) {
        if (HasEntry)
          return VH.fail();
        HasEntry = true;
      }

      for (const auto &Edge : Block.Successors)
        if (not Edge->verify(VH))
          return VH.fail();
    }

    if (not HasEntry) {
      return VH.fail("The function CFG does not contain a block starting at "
                     "the entry point",
                     *this);
    }
  }

  if (Prototype.isValid()) {
    // The function has a prototype
    if (not Prototype.get()->verify(VH))
      return VH.fail("Function prototype does not verify", *this);

    const model::Type *FunctionType = Prototype.get();
    if (not(isa<RawFunctionType>(FunctionType)
            or isa<CABIFunctionType>(FunctionType))) {
      return VH.fail("Function prototype is not a RawFunctionType or "
                     "CABIFunctionType",
                     *this);
    }
  }

  return true;
}

void DynamicFunction::dump() const {
  serialize(dbg, *this);
}

bool DynamicFunction::verify() const {
  return verify(false);
}

bool DynamicFunction::verify(bool Assert) const {
  VerifyHelper VH(Assert);
  return verify(VH);
}

bool DynamicFunction::verify(VerifyHelper &VH) const {
  // Ensure we have a name
  if (OriginalName.size() == 0)
    return VH.fail("Dynamic functions must have a OriginalName", *this);

  // Prototype is valid
  if (Prototype.isValid()) {
    if (not Prototype.get()->verify(VH))
      return VH.fail();

    const model::Type *FunctionType = Prototype.get();
    if (not(isa<RawFunctionType>(FunctionType)
            or isa<CABIFunctionType>(FunctionType))) {
      return VH.fail("The prototype is neither a RawFunctionType nor a "
                     "CABIFunctionType",
                     *this);
    }
  }

  return true;
}

void FunctionEdge::dump() const {
  serialize(dbg, *this);
}

bool FunctionEdge::verify() const {
  return verify(false);
}

bool FunctionEdge::verify(bool Assert) const {
  VerifyHelper VH(Assert);
  return verify(VH);
}

static bool verifyFunctionEdge(VerifyHelper &VH, const FunctionEdgeBase &E) {
  using namespace model::FunctionEdgeType;

  switch (E.Type) {
  case Invalid:
  case Count:
    return VH.fail();

  case DirectBranch:
  case FakeFunctionCall:
  case FakeFunctionReturn:
    if (E.Destination.isInvalid())
      return VH.fail();
    break;
  case FunctionCall: {
    const auto &Call = cast<const CallEdge>(E);
    if (not(E.Destination.isValid() == Call.DynamicFunction.empty()))
      return VH.fail();
  } break;

  case IndirectCall:
  case Return:
  case BrokenReturn:
  case IndirectTailCall:
  case LongJmp:
  case Killer:
  case Unreachable:
    if (E.Destination.isValid())
      return VH.fail();
    break;
  }

  return true;
}

bool FunctionEdgeBase::verify() const {
  return verify(false);
}

bool FunctionEdgeBase::verify(bool Assert) const {
  VerifyHelper VH(Assert);
  return verify(VH);
}

bool FunctionEdgeBase::verify(VerifyHelper &VH) const {
  if (auto *Edge = dyn_cast<CallEdge>(this))
    return VH.maybeFail(Edge->verify(VH));
  else if (auto *Edge = dyn_cast<FunctionEdge>(this))
    return VH.maybeFail(Edge->verify(VH));
  else
    revng_abort("Invalid FunctionEdgeBase instance");

  return false;
}

void FunctionEdgeBase::dump() const {
  serialize(dbg, *this);
}

bool FunctionEdge::verify(VerifyHelper &VH) const {
  if (auto *Call = dyn_cast<CallEdge>(this))
    return VH.maybeFail(Call->verify(VH));
  else
    return verifyFunctionEdge(VH, *this);
}

void CallEdge::dump() const {
  serialize(dbg, *this);
}

bool CallEdge::verify() const {
  return verify(false);
}

bool CallEdge::verify(bool Assert) const {
  VerifyHelper VH(Assert);
  return verify(VH);
}

bool CallEdge::verify(VerifyHelper &VH) const {
  if (Type == model::FunctionEdgeType::FunctionCall) {
    // We're in a direct function call (either dynamic or not)
    bool IsDynamic = not DynamicFunction.empty();
    bool HasDestination = Destination.isValid();
    if (not HasDestination and not IsDynamic)
      return VH.fail("Direct call is missing Destination");
    else if (HasDestination and IsDynamic)
      return VH.fail("Dynamic function calls cannot have a valid Destination");

    bool HasPrototype = Prototype.isValid();
    if (HasPrototype)
      return VH.fail("Direct function calls must not have a prototype");
  } else {
    // We're in an indirect call site
    if (not Prototype.isValid() or not Prototype.get()->verify(VH))
      return VH.fail("Indirect call has must have a valid prototype");
  }

  return VH.maybeFail(verifyFunctionEdge(VH, *this));
}

Identifier BasicBlock::name() const {
  using llvm::Twine;
  if (not CustomName.empty())
    return CustomName;
  else
    return Identifier(std::string("bb_") + Start.toString());
}

void BasicBlock::dump() const {
  serialize(dbg, *this);
}

bool BasicBlock::verify() const {
  return verify(false);
}

bool BasicBlock::verify(bool Assert) const {
  VerifyHelper VH(Assert);
  return verify(VH);
}

bool BasicBlock::verify(VerifyHelper &VH) const {
  if (Start.isInvalid() or End.isInvalid() or not CustomName.verify(VH))
    return VH.fail();

  for (auto &Edge : Successors)
    if (not Edge->verify(VH))
      return VH.fail();

  return true;
}

namespace RelocationType {

Values fromELFRelocation(model::Architecture::Values Architecture,
                         unsigned char ELFRelocation) {
  using namespace llvm::ELF;
  switch (Architecture) {
  case model::Architecture::x86:
    switch (ELFRelocation) {
    case R_386_RELATIVE:
    case R_386_32:
      return AddAbsoluteAddress32;

    case R_386_JUMP_SLOT:
    case R_386_GLOB_DAT:
      return WriteAbsoluteAddress32;

    case R_386_COPY:
      // TODO: use
    default:
      return Invalid;
    }

  case model::Architecture::x86_64:
    switch (ELFRelocation) {
    case R_X86_64_RELATIVE:
      return AddAbsoluteAddress64;

    case R_X86_64_JUMP_SLOT:
    case R_X86_64_GLOB_DAT:
    case R_X86_64_64:
      return WriteAbsoluteAddress64;

    case R_X86_64_32:
      return WriteAbsoluteAddress32;

    case R_X86_64_COPY:
      // TODO: use
    default:
      return Invalid;
    }

  case model::Architecture::arm:
    switch (ELFRelocation) {
    case R_ARM_RELATIVE:
      return AddAbsoluteAddress32;

    case R_ARM_JUMP_SLOT:
    case R_ARM_GLOB_DAT:
      return WriteAbsoluteAddress32;

    case R_ARM_COPY:
      // TODO: use
    default:
      return Invalid;
    }

  case model::Architecture::aarch64:
    return Invalid;

  case model::Architecture::mips:
  case model::Architecture::mipsel:
    switch (ELFRelocation) {
    case R_MIPS_IMPLICIT_RELATIVE:
      return AddAbsoluteAddress32;

    case R_MIPS_JUMP_SLOT:
    case R_MIPS_GLOB_DAT:
      return WriteAbsoluteAddress32;

    case R_MIPS_COPY:
      // TODO: use
    default:
      return Invalid;
    }

  case model::Architecture::systemz:
    switch (ELFRelocation) {
    case R_390_GLOB_DAT:
      return WriteAbsoluteAddress64;

    case R_390_COPY:
      // TODO: use
    default:
      return Invalid;
    }

  default:
    revng_abort();
  }
}

bool isELFRelocationBaseRelative(model::Architecture::Values Architecture,
                                 unsigned char ELFRelocation) {
  using namespace llvm::ELF;
  switch (Architecture) {
  case model::Architecture::x86:
    switch (ELFRelocation) {
    case R_386_RELATIVE:
      return true;

    case R_386_32:
    case R_386_JUMP_SLOT:
    case R_386_GLOB_DAT:
      return false;

    case R_386_COPY:
      // TODO: use

    default:
      return Invalid;
    }

  case model::Architecture::x86_64:
    switch (ELFRelocation) {
    case R_X86_64_RELATIVE:
      return true;

    case R_X86_64_JUMP_SLOT:
    case R_X86_64_GLOB_DAT:
    case R_X86_64_64:
    case R_X86_64_32:
      return false;

    case R_X86_64_COPY:
      // TODO: use

    default:
      return Invalid;
    }

  case model::Architecture::arm:
    switch (ELFRelocation) {
    case R_ARM_RELATIVE:
      return true;

    case R_ARM_JUMP_SLOT:
    case R_ARM_GLOB_DAT:
      return false;

    case R_ARM_COPY:
      // TODO: use
    default:
      return Invalid;
    }

  case model::Architecture::aarch64:
    return Invalid;

  case model::Architecture::mips:
  case model::Architecture::mipsel:
    switch (ELFRelocation) {
    case R_MIPS_IMPLICIT_RELATIVE:
      return true;

    case R_MIPS_JUMP_SLOT:
    case R_MIPS_GLOB_DAT:
      return false;

    case R_MIPS_COPY:
      // TODO: use
    default:
      return Invalid;
    }

  case model::Architecture::systemz:
    switch (ELFRelocation) {
    case R_390_GLOB_DAT:
      return false;

    case R_390_COPY:
      // TODO: use
    default:
      return Invalid;
    }

  default:
    revng_abort();
  }
}

} // namespace RelocationType

} // namespace model

template<>
struct llvm::DOTGraphTraits<model::FunctionCFG *>
  : public DefaultDOTGraphTraits {
  DOTGraphTraits(bool Simple = false) : DefaultDOTGraphTraits(Simple) {}

  static std::string
  getNodeLabel(const model::FunctionCFGNode *Node, const model::FunctionCFG *) {
    return Node->Start.toString();
  }

  static std::string getNodeAttributes(const model::FunctionCFGNode *Node,
                                       const model::FunctionCFG *Graph) {
    if (Node->Start == Graph->entry()) {
      return "shape=box,peripheries=2";
    }

    return "";
  }
};
