#pragma once

//
// Copyright (c) rev.ng Labs Srl. See LICENSE.md for details.
//

#include <array>
#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/Pipeline/Context.h"
#include "revng/Pipeline/Contract.h"
#include "revng/Pipes/FunctionStringMap.h"
#include "revng/Pipes/Kinds.h"
#include "revng/Pipes/StringBufferContainer.h"

#include "revng-c/Pipes/Kinds.h"

namespace revng::pipes {

inline constexpr char DecompiledMIMEType[] = "text/x.c+ptml";
inline constexpr char DecompiledSuffix[] = ".c";
inline constexpr char DecompiledName[] = "DecompiledCCode";
using DecompiledFileContainer = StringBufferContainer<&kinds::DecompiledToC,
                                                      DecompiledName,
                                                      DecompiledMIMEType,
                                                      DecompiledSuffix>;

class DecompiledYAMLToC {
public:
  static constexpr auto Name = "DecompiledYAMLToC";

  std::array<pipeline::ContractGroup, 1> getContract() const {
    using namespace pipeline;
    using namespace revng::kinds;

    return { ContractGroup({ Contract(DecompiledToYAML,
                                      0,
                                      DecompiledToC,
                                      1,
                                      InputPreservation::Preserve) }) };
  }

  void run(const pipeline::Context &Ctx,
           const DecompiledCCodeInYAMLStringMap &DecompiledFunctionsContainer,
           DecompiledFileContainer &OutCFile);

  void print(const pipeline::Context &Ctx,
             llvm::raw_ostream &OS,
             llvm::ArrayRef<std::string> ContainerNames) const;
};

} // end namespace revng::pipes
