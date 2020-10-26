#ifndef REVNGC_RESTRUCTURE_CFG_BASICBLOCKNODE_H
#define REVNGC_RESTRUCTURE_CFG_BASICBLOCKNODE_H

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Standard includes
#include <cstdlib>
#include <map>
#include <set>

// LLVM includes
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>

// revng includes
#include <revng/Support/Debug.h>

// Forward declarations
template<class NodeT>
class RegionCFG;

/// \brief Graph Node, representing a basic block
template<class NodeT>
class BasicBlockNode {
protected:
  using BasicBlockNodeMap = std::map<BasicBlockNode *, BasicBlockNode *>;

public:
  enum class Type {
    Code,
    Empty,
    Break,
    Continue,
    Set,
    Collapsed,
    Dispatcher,
  };

  using BasicBlockNodeT = BasicBlockNode<NodeT>;
  using BBNodeSet = std::set<BasicBlockNode<NodeT> *>;
  using BBNodeMap = std::map<BasicBlockNodeT *, BasicBlockNodeT *>;
  using RegionCFGT = RegionCFG<NodeT>;

  // EdgeDescriptor is a handy way to create and manipulate edges on the
  // RegionCFG.
  using EdgeDescriptor = std::pair<BasicBlockNodeT *, BasicBlockNodeT *>;
  using edge_label_t = llvm::SmallSet<uint64_t, 1>;

  // The `EdgeInfo` struct is devoted to contain additional info for the edges,
  // that may come handy during the control flow processing.
  struct EdgeInfo {

    // In this field, the labels associated to an edge are stored.
    edge_label_t Labels;

    // This field of the struct represent the fact that on this edge, we have an
    // inlinable path. This means that, the edge dominates all nodes reachable
    // from the edge on all the possible paths going towards all the exit nodes
    // reachable from the edge. Therefore, this edges can be excluded from the
    // computation of the postdominator tree, since they can be emitted
    // completely as body of the `then`/`else` branches.
    bool Inlined = false;

    // Spaceship operator for struct comparison.
    auto operator<=>(const EdgeInfo &) const = default;
  };
  using node_edgeinfo_pair = std::pair<BasicBlockNodeT *, EdgeInfo>;

  using links_container = llvm::SmallVector<node_edgeinfo_pair, 2>;
  using links_iterator = typename links_container::iterator;
  using links_const_iterator = typename links_container::const_iterator;
  using links_range = llvm::iterator_range<links_iterator>;
  using links_const_range = llvm::iterator_range<links_const_iterator>;

protected:
  static BasicBlockNodeT *getChild(const node_edgeinfo_pair &P) {
    return P.first;
  }

public:
  using child_iterator = llvm::mapped_iterator<links_iterator,
                                               decltype(&getChild)>;
  using child_const_iterator = llvm::mapped_iterator<links_const_iterator,
                                                     decltype(&getChild)>;
  using child_range = llvm::iterator_range<child_iterator>;
  using child_const_range = llvm::iterator_range<child_const_iterator>;

protected:
  /// Unique Node Id inside a RegionCFG<NodeT>, useful for printing to graphviz
  unsigned ID;

  /// Pointer to the parent RegionCFG<NodeT>
  RegionCFGT *Parent;

  /// Reference to the corresponding collapsed region
  //
  // This is nullptr unless the BasicBlockNode represents a collapsed
  // RegionCFG<NodeT>
  RegionCFGT *CollapsedRegion;

  /// Flag to identify the exit type of a block
  Type NodeType;

  /// Name of the basic block.
  llvm::SmallString<32> Name;

  unsigned StateVariableValue;

  /// List of successors
  links_container Successors;

  /// List of predecessors
  links_container Predecessors;

  // Original object pointer
  NodeT OriginalNode;

  // Flag for nodes that were created by weaving switches
  bool Weaved;

  explicit BasicBlockNode(RegionCFGT *Parent,
                          NodeT OriginalNode,
                          RegionCFGT *Collapsed,
                          llvm::StringRef Name,
                          Type T,
                          unsigned StateVariableVal = 0);

public:
  BasicBlockNode() = delete;
  BasicBlockNode(BasicBlockNode &&BBN) = delete;
  BasicBlockNode &operator=(const BasicBlockNode &BBN) = delete;
  BasicBlockNode &operator=(BasicBlockNode &&BBN) = delete;

  /// Copy ctor: clone the node in the same Parent with new ID and without edges
  explicit BasicBlockNode(const BasicBlockNode &BBN, RegionCFGT *Parent) :
    BasicBlockNode(Parent,
                   BBN.OriginalNode,
                   BBN.CollapsedRegion,
                   BBN.Name,
                   BBN.NodeType,
                   BBN.StateVariableValue) {}

  /// \brief Constructor for nodes pointing to LLVM IR BasicBlock
  explicit BasicBlockNode(RegionCFGT *Parent,
                          NodeT OriginalNode,
                          llvm::StringRef Name = "") :
    BasicBlockNode(Parent, OriginalNode, nullptr, Name, Type::Code) {}

  /// \brief Constructor for nodes representing collapsed subgraphs
  explicit BasicBlockNode(RegionCFGT *Parent, RegionCFGT *Collapsed) :
    BasicBlockNode(Parent, nullptr, Collapsed, "collapsed", Type::Collapsed) {}

  /// \brief Constructor for empty dummy nodes and for entry/exit dispatcher
  explicit BasicBlockNode(RegionCFG<NodeT> *Parent,
                          llvm::StringRef Name,
                          Type T) :
    BasicBlockNode(Parent, nullptr, nullptr, Name, T) {
    revng_assert(T == Type::Empty or T == Type::Break or T == Type::Continue
                 or T == Type::Dispatcher);
  }

  /// \brief Constructor for dummy nodes that handle the state variable
  explicit BasicBlockNode(RegionCFGT *Parent,
                          llvm::StringRef Name,
                          Type T,
                          unsigned Value) :
    BasicBlockNode(Parent, nullptr, nullptr, Name, T, Value) {
    revng_assert(T == Type::Set);
  }

public:
  bool isBreak() const { return NodeType == Type::Break; }
  bool isContinue() const { return NodeType == Type::Continue; }
  bool isSet() const { return NodeType == Type::Set; }
  bool isCode() const { return NodeType == Type::Code; }
  bool isEmpty() const { return NodeType == Type::Empty; }
  bool isArtificial() const {
    return NodeType != Type::Code and NodeType != Type::Collapsed;
  }
  bool isDispatcher() const { return NodeType == Type::Dispatcher; }
  Type getNodeType() const { return NodeType; }

  unsigned getStateVariableValue() const {
    revng_assert(isSet());
    return StateVariableValue;
  }

  RegionCFGT *getParent() { return Parent; }
  void setParent(RegionCFGT *P) { Parent = P; }

  // void removeNode();

  // TODO: Check why this implementation is really necessary.
  void printAsOperand(llvm::raw_ostream &O, bool /* PrintType */) const;

  void addLabeledSuccessor(const node_edgeinfo_pair &P) {
    revng_assert(not hasSuccessor(P.first));
    Successors.push_back(P);
  }

  void addLabeledSuccessor(node_edgeinfo_pair &&P) {
    revng_assert(not hasSuccessor(P.first));
    Successors.push_back(std::move(P));
  }

  void addUnlabeledSuccessor(BasicBlockNode *Successor) {
    addLabeledSuccessor(std::make_pair(Successor, EdgeInfo()));
  }

  bool hasSuccessor(const BasicBlockNode *Candidate) const {
    const auto First = [](const auto &Pair) { return Pair.first; };
    auto BBRange = llvm::map_range(Successors, First);

    const auto Find = [](const auto &Range, const auto *C) {
      return std::find(Range.begin(), Range.end(), C) != Range.end();
    };

    return Find(BBRange, Candidate);
  }

  void removeSuccessor(BasicBlockNode *Successor);
  node_edgeinfo_pair extractSuccessorEdge(BasicBlockNode *Successor);

  void addLabeledPredecessor(const node_edgeinfo_pair &P) {
    revng_assert(not hasPredecessor(P.first));
    Predecessors.push_back(P);
  }

  void addUnlabeledPredecessor(BasicBlockNode *Predecessor) {
    addLabeledPredecessor(std::make_pair(Predecessor, EdgeInfo()));
  }

  bool hasPredecessor(BasicBlockNode *Candidate) const {

    const auto First = [](const auto &Pair) { return Pair.first; };
    auto BBRange = llvm::map_range(Predecessors, First);

    const auto Find = [](const auto &Range, const auto *C) {
      return std::find(Range.begin(), Range.end(), C) != Range.end();
    };

    return Find(BBRange, Candidate);
  }

  void removePredecessor(BasicBlockNode *Successor);
  node_edgeinfo_pair extractPredecessorEdge(BasicBlockNode *Predecessor);

  void updatePointers(const BasicBlockNodeMap &SubstitutionMap);

  size_t successor_size() const { return Successors.size(); }

  links_const_range labeled_successors() const {
    return llvm::make_range(Successors.begin(), Successors.end());
  }

  links_range labeled_successors() {
    return llvm::make_range(Successors.begin(), Successors.end());
  }

  child_const_range successors() const {
    return llvm::map_range(labeled_successors(), &getChild);
  }

  child_range successors() {
    return llvm::map_range(labeled_successors(), &getChild);
  }

  BasicBlockNode *getSuccessorI(size_t i) const { return Successors[i].first; }

  size_t predecessor_size() const { return Predecessors.size(); }

  links_const_range labeled_predecessors() const {
    return llvm::make_range(Predecessors.begin(), Predecessors.end());
  }

  links_range labeled_predecessors() {
    return llvm::make_range(Predecessors.begin(), Predecessors.end());
  }

  child_const_range predecessors() const {
    return llvm::map_range(labeled_predecessors(), &getChild);
  }

  child_range predecessors() {
    return llvm::map_range(labeled_predecessors(), &getChild);
  }

  unsigned getID() const { return ID; }
  bool isBasicBlock() const { return NodeType == Type::Code; }

  NodeT getOriginalNode() const {
    revng_assert(isCode() and nullptr != OriginalNode);
    return OriginalNode;
  }

  llvm::StringRef getName() const;
  std::string getNameStr() const {
    return "ID:" + std::to_string(getID()) + " " + getName().str();
  }

  void setName(llvm::StringRef N) { Name = N; }

  bool isCollapsed() const { return NodeType == Type::Collapsed; }
  RegionCFGT *getCollapsedCFG() { return CollapsedRegion; }

  bool isEquivalentTo(BasicBlockNode *) const;

  /// \brief Obtain a estimate of the weight of a BasicBlockNode in terms of
  ///        original instructions.
  size_t getWeight() const;

  bool isWeaved() const { return Weaved; }
  void setWeaved(bool Val) { Weaved = Val; }
};

// Provide graph traits for usage with, e.g., llvm::ReversePostOrderTraversal
namespace llvm {

template<class NodeT>
struct GraphTraits<BasicBlockNode<NodeT> *> {
  using BBNodeT = BasicBlockNode<NodeT>;
  using NodeRef = BBNodeT *;
  using EdgeRef = typename BBNodeT::node_edgeinfo_pair;
  using ChildIteratorType = typename BBNodeT::child_iterator;
  using ChildEdgeIteratorType = typename BBNodeT::links_iterator;

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static inline ChildIteratorType child_begin(NodeRef N) {
    return N->successors().begin();
  }

  static inline ChildIteratorType child_end(NodeRef N) {
    return N->successors().end();
  }

  static inline ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->labeled_successors().begin();
  }

  static inline ChildEdgeIteratorType child_edge_end(NodeRef N) {
    return N->labeled_successors().end();
  }

  static inline NodeRef edge_dest(EdgeRef E) { return E.first; };
};

template<class NodeT>
struct GraphTraits<Inverse<BasicBlockNode<NodeT> *>> {
  using BBNodeT = BasicBlockNode<NodeT>;
  using NodeRef = BBNodeT *;
  using EdgeRef = typename BBNodeT::node_edgeinfo_pair;
  using ChildIteratorType = typename BBNodeT::child_iterator;
  using ChildEdgeIteratorType = typename BBNodeT::links_iterator;

  static NodeRef getEntryNode(Inverse<NodeRef> G) { return G.Graph; }

  static inline ChildIteratorType child_begin(NodeRef N) {
    return N->predecessors().begin();
  }

  static inline ChildIteratorType child_end(NodeRef N) {
    return N->predecessors().end();
  }

  static inline ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->labeled_predecessors().begin();
  }

  static inline ChildEdgeIteratorType child_edge_end(NodeRef N) {
    return N->labeled_predecessors().end();
  }

  static inline NodeRef edge_dest(EdgeRef E) { return E.first; };
};

} // namespace llvm

#endif // REVNGC_RESTRUCTURE_CFG_BASICBLOCKNODE_H
