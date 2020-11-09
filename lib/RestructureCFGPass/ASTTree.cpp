/// \file ASTTree.cpp
/// \brief

//
// Copyright rev.ng Srls. See LICENSE.md for details.
//

// Standard includes
#include <cstdlib>

// LLVM includes
#include <llvm/Support/raw_os_ostream.h>

// Local libraries includes
#include "revng-c/RestructureCFGPass/ASTNode.h"
#include "revng-c/RestructureCFGPass/ASTTree.h"
#include "revng-c/RestructureCFGPass/Utils.h"

using namespace llvm;
using ASTNodeMap = std::map<ASTNode *, ASTNode *>;
using ExprNodeMap = std::map<ExprNode *, ExprNode *>;

// Helper to obtain a unique incremental counter (to give name to sequence
// nodes).
static int Counter = 1;
static std::string getID() {
  return std::to_string(Counter++);
}

SequenceNode *ASTTree::addSequenceNode() {
  ASTNodeList.emplace_back(SequenceNode::createEmpty("sequence " + getID()));

  // Set the Node ID
  ASTNodeList.back()->setID(getNewID());

  return llvm::cast<SequenceNode>(ASTNodeList.back().get());
}

size_t ASTTree::size() const {
  return ASTNodeList.size();
}

void ASTTree::addASTNode(BasicBlockNode<BasicBlock *> *Node,
                         ast_unique_ptr &&ASTObject) {
  ASTNodeList.emplace_back(std::move(ASTObject));

  ASTNode *ASTNode = ASTNodeList.back().get();

  // Set the Node ID
  ASTNode->setID(getNewID());

  bool New = BBASTMap.insert({ Node, ASTNode }).second;
  revng_assert(New);
  New = ASTBBMap.insert({ ASTNode, Node }).second;
  revng_assert(New);
}

void ASTTree::removeASTNode(ASTNode *Node) {
  revng_log(CombLogger, "Removing AST node named: " << Node->getName() << "\n");

  for (auto It = ASTNodeList.begin(); It != ASTNodeList.end(); It++) {
    if ((*It).get() == Node) {
      ASTNodeList.erase(It);
      break;
    }
  }
}

ASTNode *ASTTree::findASTNode(BasicBlockNode<BasicBlock *> *BlockNode) {
  return BBASTMap.at(BlockNode);
}

BasicBlockNode<BasicBlock *> *ASTTree::findCFGNode(ASTNode *ASTNode) {
  auto It = ASTBBMap.find(ASTNode);
  if (It != ASTBBMap.end())
    return It->second;
  // We may return nullptr, since for example continue and break nodes do not
  // have a corresponding CFGNode.
  return nullptr;
}

void ASTTree::setRoot(ASTNode *Root) {
  RootNode = Root;
}

ASTNode *ASTTree::getRoot() const {
  return RootNode;
}

ASTNode *ASTTree::copyASTNodesFrom(ASTTree &OldAST) {
  ASTNodeMap ASTSubstitutionMap{};
  ExprNodeMap CondExprMap{};

  // Clone each ASTNode in the current AST.
  links_container::difference_type NewNodes = 0;
  for (ASTNode *Old : OldAST.nodes()) {
    ASTNodeList.emplace_back(std::move(Old->Clone()));
    ++NewNodes;

    ASTNode *NewASTNode = ASTNodeList.back().get();

    // Set the Node ID
    NewASTNode->setID(getNewID());

    BasicBlockNode<BasicBlock *> *OldCFGNode = OldAST.findCFGNode(Old);
    if (OldCFGNode != nullptr) {
      BBASTMap.insert({ OldCFGNode, NewASTNode });
      ASTBBMap.insert({ NewASTNode, OldCFGNode });
    }
    ASTSubstitutionMap[Old] = NewASTNode;
  }

  // Clone the conditional expression nodes.
  for (const expr_unique_ptr &OldExpr : OldAST.expressions()) {
    CondExprList.emplace_back(new AtomicNode(*cast<AtomicNode>(OldExpr.get())),
                              expr_destructor());
    ExprNode *NewExpr = CondExprList.back().get();
    CondExprMap[OldExpr.get()] = NewExpr;
  }

  // Update the AST and BBNode pointers inside the newly created AST nodes,
  // to reflect the changes made. Update also the pointer to the conditional
  // expressions just cloned.
  auto BeginInserted = ASTNodeList.end() - NewNodes;
  auto EndInserted = ASTNodeList.end();
  using MovedIteratorRange = llvm::iterator_range<links_container::iterator>;
  MovedIteratorRange Result = llvm::make_range(BeginInserted, EndInserted);
  for (ast_unique_ptr &NewNode : Result) {
    NewNode->updateASTNodesPointers(ASTSubstitutionMap);
    if (auto *If = llvm::dyn_cast<IfNode>(NewNode.get())) {
      If->updateCondExprPtr(CondExprMap);
    }
  }

  revng_assert(ASTSubstitutionMap.count(OldAST.getRoot()) != 0);
  return ASTSubstitutionMap[OldAST.getRoot()];
}

void ASTTree::dumpASTOnFile(const std::string &FileName) const {
  std::error_code EC;
  llvm::raw_fd_ostream DotFile(FileName, EC);
  revng_check(not EC, "Could not open file to print AST dot");

  // Open the `digraph`.
  DotFile << "digraph CFGFunction {\n";

  // Dump the graph in an iteratively fashion.
  for (const auto &Node : ASTNodeList) {
    Node->dump(DotFile);
  }

  // For each node in the graph, dump the outgoing edges.
  for (const auto &Node : ASTNodeList) {
    Node->dumpEdge(DotFile);

    // For each node, if present, dump the edge going to the node in the
    // `Successor` field.
    Node->dumpSuccessor(DotFile);
  }

  // Conclude the `digraph`.
  DotFile << "}\n";
}

void ASTTree::dumpASTOnFile(const std::string &FunctionName,
                            const std::string &FolderName,
                            const std::string &FileName) const {

  const std::string GraphDir = "debug-graphs";
  std::error_code EC = llvm::sys::fs::create_directory(GraphDir);
  revng_check(not EC, "Could not create directory to print AST dot");
  EC = llvm::sys::fs::create_directory(GraphDir + "/" + FunctionName);
  revng_check(not EC, "Could not create directory to print AST dot");
  const std::string PathName = GraphDir + "/" + FunctionName + "/" + FolderName;
  EC = llvm::sys::fs::create_directory(PathName);
  revng_check(not EC, "Could not create directory to print AST dot");
  dumpASTOnFile(PathName + "/" + FileName);
}

ExprNode *ASTTree::addCondExpr(expr_unique_ptr &&Expr) {
  CondExprList.emplace_back(std::move(Expr));
  return CondExprList.back().get();
}
