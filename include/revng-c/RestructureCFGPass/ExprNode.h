#ifndef REVNGC_RESTRUCTURE_CFG_EXPRNODE_H
#define REVNGC_RESTRUCTURE_CFG_EXPRNODE_H

//
// Copyright rev.ng Srls. See LICENSE.md for details.
//

// Standard includes
#include <cstdlib>

// LLVM includes
#include <llvm/Support/Casting.h>

class AtomicNode;
class NotNode;
class AndNode;
class OrNode;

namespace llvm {
class BasicBlock;
}

class ExprNode {
public:
  enum NodeKind { NK_Atomic, NK_Not, NK_And, NK_Or };

protected:
  const NodeKind Kind;

public:
  NodeKind getKind() const { return Kind; }

  ExprNode(const ExprNode &) = default;
  ExprNode(ExprNode &&) = default;

  static void deleteExprNode(ExprNode *E);

protected:
  ExprNode(NodeKind K) : Kind(K) {}
  ~ExprNode() = default;
};

class AtomicNode : public ExprNode {
protected:
  llvm::BasicBlock *ConditionBB;

public:
  friend ExprNode;
  static bool classof(const ExprNode *E) { return E->getKind() == NK_Atomic; }

  AtomicNode(llvm::BasicBlock *BB) : ExprNode(NK_Atomic), ConditionBB(BB) {}

  AtomicNode(const AtomicNode &) = default;
  AtomicNode(AtomicNode &&) = default;

  AtomicNode() = delete;

protected:
  ~AtomicNode() = default;

public:
  llvm::BasicBlock *getConditionalBasicBlock() const { return ConditionBB; }
};

class NotNode : public ExprNode {
protected:
  ExprNode *Child;

public:
  friend ExprNode;
  static bool classof(const ExprNode *E) { return E->getKind() == NK_Not; }

  NotNode(ExprNode *N) : ExprNode(NK_Not), Child(N) {}

  NotNode(const NotNode &) = default;
  NotNode(NotNode &&) = default;

  NotNode() = delete;

protected:
  ~NotNode() = default;

public:
  ExprNode *getNegatedNode() const { return Child; }
};

class BinaryNode : public ExprNode {
protected:
  ExprNode *LeftChild;
  ExprNode *RightChild;

public:
  friend ExprNode;
  static bool classof(const ExprNode *E) {
    return E->getKind() <= NK_Or and E->getKind() >= NK_And;
  }

protected:
  BinaryNode(NodeKind K, ExprNode *Left, ExprNode *Right) :
    ExprNode(K), LeftChild(Left), RightChild(Right) {}

  BinaryNode(const BinaryNode &) = default;
  BinaryNode(BinaryNode &&) = default;

  BinaryNode() = delete;
  ~BinaryNode() = default;

public:
  std::pair<ExprNode *, ExprNode *> getInternalNodes() {
    return std::make_pair(LeftChild, RightChild);
  }
};

class AndNode : public BinaryNode {
public:
  friend ExprNode;
  static bool classof(const ExprNode *E) { return E->getKind() == NK_And; }

  AndNode(ExprNode *Left, ExprNode *Right) : BinaryNode(NK_And, Left, Right) {}

  AndNode(const AndNode &) = default;
  AndNode(AndNode &&) = default;

  AndNode() = delete;

protected:
  ~AndNode() = default;
};

class OrNode : public BinaryNode {

public:
  friend ExprNode;
  static bool classof(const ExprNode *E) { return E->getKind() == NK_Or; }

  OrNode(ExprNode *Left, ExprNode *Right) : BinaryNode(NK_Or, Left, Right) {}

  OrNode(const OrNode &) = default;
  OrNode(OrNode &&) = default;

  OrNode() = delete;

protected:
  ~OrNode() = default;
};

#endif // define REVNGC_RESTRUCTURE_CFG_EXPRNODE_H
