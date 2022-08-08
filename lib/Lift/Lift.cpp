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
const char *EntryDescStr = "virtual address of the entry point where to start";
opt<unsigned long long> EntryPointAddress("entry",
                                          desc(EntryDescStr),
                                          value_desc("address"),
                                          cat(MainCategory));
alias A1("e",
         desc("Alias for -entry"),
         aliasopt(EntryPointAddress),
         cat(MainCategory));

} // namespace

char LiftPass::ID;

using Register = llvm::RegisterPass<LiftPass>;
static Register X("lift", "Lift Pass", true, true);

struct ExternalFilePaths {
  std::string LibTcg;
  std::string LibHelpers;
  std::string EarlyLinked;
};

static ExternalFilePaths
findExternalFilePaths(const model::Architecture::Values Architecture) {
  // What symbols from the revng namespace are actually used here?
  using namespace revng;

  const std::string ArchName = model::Architecture::getQEMUName(Architecture).str();

  ExternalFilePaths Paths = {};

  const std::string LibTcgName = "/lib/libtcg-" + ArchName + ".so";
  auto OptionalLibTcg = ResourceFinder.findFile(LibTcgName);
  revng_assert(OptionalLibTcg.has_value(), "Cannot find libtinycode");
  Paths.LibTcg = OptionalLibTcg.value();

  const std::string LibHelpersName = "/lib/libtcg-helpers-" + ArchName + ".bc";
  auto OptionalHelpers = ResourceFinder.findFile(LibHelpersName);
  revng_assert(OptionalHelpers.has_value(), "Cannot find tinycode helpers");
  Paths.LibHelpers = OptionalHelpers.value();

  const std::string EarlyLinkedName = "/share/revng/early-linked-" + ArchName + ".ll";
  auto OptionalEarlyLinked = ResourceFinder.findFile(EarlyLinkedName);
  revng_assert(OptionalEarlyLinked.has_value(), "Cannot find early-linked.ll");
  Paths.EarlyLinked = OptionalEarlyLinked.value();

  return Paths;
}

bool LiftPass::runOnModule(llvm::Module &M) {
  const auto &ModelWrapper = getAnalysis<LoadModelWrapperPass>().get();
  const TupleTree<model::Binary> &Model = ModelWrapper.getReadOnlyModel();

  const auto Paths = findExternalFilePaths(Model->Architecture);

  // Look for the library in the system's paths
  void *LibraryHandle = dlopen(Paths.LibTcg.c_str(), RTLD_LAZY | RTLD_NODELETE);
  if (LibraryHandle == nullptr) {
    fprintf(stderr, "Couldn't load the libtcg library: %s\n", dlerror());
    return EXIT_FAILURE;
  }

  // Obtain the address of the libtcg_load entry point
  //using LibTcgLoadFunc = LIBTCG_FUNC_TYPE(libtcg_load);
  auto LibTcgLoad = reinterpret_cast<LIBTCG_FUNC_TYPE(libtcg_load) *>(dlsym(LibraryHandle, "libtcg_load"));
  if (LibTcgLoad == nullptr) {
    fprintf(stderr, "Couldn't find symbol libtcg_load: %s\n", dlerror());
    return EXIT_FAILURE;
  }

  // Load the libtcg interface containing relevant function pointers
  const auto LibTcg = LibTcgLoad();

  // Get access to raw binary data
  RawBinaryView &RawBinary = getAnalysis<LoadBinaryWrapperPass>().get();

  CodeGenerator Generator(RawBinary,
                          &M,
                          Model,
                          Paths.LibHelpers,
                          Paths.EarlyLinked,
                          model::Architecture::x86_64);

  llvm::Optional<uint64_t> EntryPointAddressOptional;
  if (EntryPointAddress.getNumOccurrences() != 0)
    EntryPointAddressOptional = EntryPointAddress;
  Generator.translate(LibTcg, EntryPointAddressOptional);

  dlclose(LibraryHandle);

  return false;
}
