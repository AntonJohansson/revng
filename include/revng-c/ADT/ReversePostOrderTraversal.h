#ifndef REVNGC_ADT_REVERSEPOSTORDERTRAVERSAL_H
#define REVNGC_ADT_REVERSEPOSTORDERTRAVERSAL_H

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// LLVM includes
#include "llvm/ADT/PostOrderIterator.h"

template<class GraphT,
         class GT = llvm::GraphTraits<GraphT>,
         class SetType = std::set<typename llvm::GraphTraits<GraphT>::NodeRef>>
class ReversePostOrderTraversalExt {
  using NodeRef = typename GT::NodeRef;
  using NodeVec = std::vector<NodeRef>;

  NodeVec Blocks; // Block list in normal RPO order

  void Initialize(NodeRef BB, SetType &WhiteList) {
    std::copy(po_ext_begin(BB, WhiteList),
              po_ext_end(BB, WhiteList),
              std::back_inserter(Blocks));
  }

public:
  using rpo_iterator = typename NodeVec::reverse_iterator;
  using const_rpo_iterator = typename NodeVec::const_reverse_iterator;

  ReversePostOrderTraversalExt(GraphT G, SetType &WhiteList) {
    Initialize(GT::getEntryNode(G), WhiteList);
  }

  // Because we want a reverse post order, use reverse iterators from the vector
  rpo_iterator begin() { return Blocks.rbegin(); }
  const_rpo_iterator begin() const { return Blocks.crbegin(); }
  rpo_iterator end() { return Blocks.rend(); }
  const_rpo_iterator end() const { return Blocks.crend(); }
};

#endif // REVNGC_ADT_REVERSEPOSTORDERTRAVERSAL_H
