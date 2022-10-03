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
#include "revng/Pipes/FileContainer.h"
#include "revng/Pipes/Kinds.h"

#include "revng-c/Pipes/Kinds.h"

namespace revng::pipes {

inline char HelpersHeaderFactoryMIMEType[] = "text/plain";
inline char HelpersHeaderFactorySuffix[] = ".h";
inline char HelpersHeaderFactoryName[] = "HelpersHeader";
using HelpersHeaderFileContainer = FileContainer<&kinds::HelpersHeader,
                                                 HelpersHeaderFactoryName,
                                                 HelpersHeaderFactoryMIMEType,
                                                 HelpersHeaderFactorySuffix>;

class HelpersToHeader {
public:
  static constexpr auto Name = "HelpersToHeader";

  std::array<pipeline::ContractGroup, 1> getContract() const {
    using namespace pipeline;
    using namespace revng::kinds;

    return { ContractGroup{ Contract(StackAccessesSegregated,
                                     Exactness::Exact,
                                     0,
                                     HelpersHeader,
                                     1,
                                     InputPreservation::Preserve) } };
  }

  void run(const pipeline::Context &Ctx,
           pipeline::LLVMContainer &IRContainer,
           HelpersHeaderFileContainer &HeaderFile);

  void print(const pipeline::Context &Ctx,
             llvm::raw_ostream &OS,
             llvm::ArrayRef<std::string> ContainerNames) const;
};

} // end namespace revng::pipes
