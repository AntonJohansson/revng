/// \file Lift.cpp
/// \brief

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/Lift/Lift.h"
#include "revng/Support/ResourceFinder.h"

#include "CodeGenerator.h"
#include "qemu/libtcg/libtcg.h"
#include <dlfcn.h>

using namespace llvm::cl;

namespace {
#define DESCRIPTION desc("virtual address of the entry point where to start")
opt<unsigned long long> EntryPointAddress("entry",
                                          DESCRIPTION,
                                          value_desc("address"), cat(MainCategory));
#undef DESCRIPTION
alias A1("e",
         desc("Alias for -entry"),
         aliasopt(EntryPointAddress),
         cat(MainCategory));

} // namespace

char LiftPass::ID;

using Register = llvm::RegisterPass<LiftPass>;
static Register X("lift", "Lift Pass", true, true);

static std::string LibTinycodePath;
static std::string LibHelpersPath;
static std::string EarlyLinkedPath;

static void findFiles(model::Architecture::Values Architecture) {
  using namespace revng;

  std::string ArchName = model::Architecture::getQEMUName(Architecture).str();

  std::string LibtinycodeName = "/lib/libtcg-" + ArchName + ".so";
  auto OptionalLibtinycode = ResourceFinder.findFile(LibtinycodeName);
  revng_assert(OptionalLibtinycode.has_value(), "Cannot find libtinycode");
  LibTinycodePath = OptionalLibtinycode.value();

  std::string LibHelpersName = "/lib/libtcg-helpers-" + ArchName + ".bc";
  auto OptionalHelpers = ResourceFinder.findFile(LibHelpersName);
  revng_assert(OptionalHelpers.has_value(), "Cannot find tinycode helpers");
  LibHelpersPath = OptionalHelpers.value();

  std::string EarlyLinkedName = "/share/revng/early-linked-" + ArchName + ".ll";
  auto OptionalEarlyLinked = ResourceFinder.findFile(EarlyLinkedName);
  revng_assert(OptionalEarlyLinked.has_value(), "Cannot find early-linked.ll");
  EarlyLinkedPath = OptionalEarlyLinked.value();
}

bool LiftPass::runOnModule(llvm::Module &M) {
  const auto &ModelWrapper = getAnalysis<LoadModelWrapperPass>().get();
  const TupleTree<model::Binary> &Model = ModelWrapper.getReadOnlyModel();

  findFiles(Model->Architecture);

  // Look for the library in the system's paths
  void *LibraryHandle = dlopen(LibTinycodePath.c_str(), RTLD_LAZY | RTLD_NODELETE);
  if (LibraryHandle == nullptr) {
    fprintf(stderr, "Couldn't load the PTC library: %s\n", dlerror());
    return EXIT_FAILURE;
  }

  // Obtain the address of the libtcg_load entry point
  auto LibTcgLoad = reinterpret_cast<FUNC_TYPE_NAME(libtcg_load) *>(dlsym(LibraryHandle, "libtcg_load"));
  if (LibTcgLoad == nullptr) {
    fprintf(stderr, "Couldn't find libtcg_load: %s\n", dlerror());
    return EXIT_FAILURE;
  }

  // Load the libtcg interface containing relevant function pointers
  const auto LibTcg = LibTcgLoad();

  // Get access to raw binary data
  RawBinaryView &RawBinary = getAnalysis<LoadBinaryWrapperPass>().get();

  CodeGenerator Generator(RawBinary,
                          &M,
                          Model,
                          LibHelpersPath,
                          EarlyLinkedPath,
                          model::Architecture::x86_64);

  llvm::Optional<uint64_t> EntryPointAddressOptional;
  if (EntryPointAddress.getNumOccurrences() != 0)
    EntryPointAddressOptional = EntryPointAddress;
  Generator.translate(LibTcg, EntryPointAddressOptional);

  dlclose(LibraryHandle);

  return false;
}
