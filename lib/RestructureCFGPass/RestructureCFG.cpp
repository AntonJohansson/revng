/// \file Restructure.cpp
/// \brief FunctionPass that applies the comb to the RegionCFG of a function

//
// Copyright rev.ng Srls. See LICENSE.md for details.
//

#include <limits>
#include <sstream>

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/GenericDomTreeConstruction.h"
#include "llvm/Support/raw_os_ostream.h"

#include "revng/Model/LoadModelPass.h"
#include "revng/Support/Debug.h"
#include "revng/Support/FunctionTags.h"
#include "revng/Support/IRHelpers.h"

#include "revng-c/RestructureCFGPass/ASTTree.h"
#include "revng-c/RestructureCFGPass/BasicBlockNodeImpl.h"
#include "revng-c/RestructureCFGPass/GenerateAst.h"
#include "revng-c/RestructureCFGPass/MetaRegionBB.h"
#include "revng-c/RestructureCFGPass/RegionCFGTreeBB.h"
#include "revng-c/RestructureCFGPass/RestructureCFG.h"
#include "revng-c/RestructureCFGPass/Utils.h"
#include "revng-c/TargetFunctionOption/TargetFunctionOption.h"

using namespace llvm;
using namespace llvm::cl;

using std::make_pair;
using std::pair;
using std::string;
using std::to_string;

// TODO: Move the initialization of the logger here from "Utils.h"
// Debug logger.
Logger<> CombLogger("restructure");

// EdgeDescriptor is a handy way to create and manipulate edges on the
// RegionCFG.
using BasicBlockNodeBB = BasicBlockNode<BasicBlock *>;
using EdgeDescriptor = std::pair<BasicBlockNodeBB *, BasicBlockNodeBB *>;

// Explicit instantation of template classes `Metaregion` and `RegionCFG`.
template class MetaRegion<BasicBlock *>;
template class RegionCFG<BasicBlock *>;
using MetaRegionBB = MetaRegion<BasicBlock *>;
using MetaRegionBBVect = std::vector<MetaRegionBB>;
using MetaRegionBBPtrVect = std::vector<MetaRegionBB *>;
using BackedgeMetaRegionMap = std::map<EdgeDescriptor, MetaRegionBB *>;

static std::set<EdgeDescriptor> getBackedges(RegionCFG<BasicBlock *> &Graph) {

  // Some helper data structures.
  int Time = 0;
  std::map<BasicBlockNodeBB *, int> StartTime;
  std::map<BasicBlockNodeBB *, int> FinishTime;
  std::vector<std::pair<BasicBlockNodeBB *, size_t>> Stack;

  // Set of backedges.
  std::set<EdgeDescriptor> Backedges;

  // Push the entry node in the exploration stack.
  BasicBlockNodeBB &EntryNode = Graph.getEntryNode();
  Stack.push_back(make_pair(&EntryNode, 0));

  // Go through the exploration stack.
  while (!Stack.empty()) {
    auto StackElem = Stack.back();
    Stack.pop_back();
    BasicBlockNodeBB *Vertex = StackElem.first;
    Time++;

    // Check if we are inspecting a vertex for the first time, and in case mark
    // the start time of the visit.
    if (StartTime.count(Vertex) == 0) {
      StartTime[Vertex] = Time;
    }

    // Successor exploraition
    size_t Index = StackElem.second;

    // If we are still successors to explore.
    if (Index < StackElem.first->successor_size()) {
      BasicBlockNodeBB *Successor = Vertex->getSuccessorI(Index);
      Index++;
      Stack.push_back(make_pair(Vertex, Index));

      // We are in presence of a backedge.
      if (StartTime.count(Successor) != 0
          and FinishTime.count(Successor) == 0) {
        Backedges.insert(make_pair(Vertex, Successor));
      }

      // Enqueue the successor for the visit.
      if (StartTime.count(Successor) == 0) {
        Stack.push_back(make_pair(Successor, 0));
      }
    } else {

      // Mark the finish of the visit of a vertex.
      FinishTime[Vertex] = Time;
    }
  }

  return Backedges;
}

static bool mergeSCSStep(MetaRegionBBVect &MetaRegions) {
  for (auto RegionIt1 = MetaRegions.begin(); RegionIt1 != MetaRegions.end();
       RegionIt1++) {
    for (auto RegionIt2 = std::next(RegionIt1); RegionIt2 != MetaRegions.end();
         RegionIt2++) {
      bool Intersects = (*RegionIt1).intersectsWith(*RegionIt2);
      bool IsIncluded = (*RegionIt1).isSubSet(*RegionIt2);
      bool IsIncludedReverse = (*RegionIt2).isSubSet(*RegionIt1);
      bool AreEquivalent = (*RegionIt1).nodesEquality(*RegionIt2);
      if (Intersects
          and (((!IsIncluded) and (!IsIncludedReverse)) or AreEquivalent)) {
        (*RegionIt1).mergeWith(*RegionIt2);
        MetaRegions.erase(RegionIt2);
        return true;
      }
    }
  }

  return false;
}

static void simplifySCS(MetaRegionBBVect &MetaRegions) {
  bool Changes = true;
  while (Changes) {
    Changes = mergeSCSStep(MetaRegions);
  }
}

static bool
mergeSCSAbnormalRetreating(MetaRegionBBVect &MetaRegions,
                           const std::set<EdgeDescriptor> &Backedges,
                           BackedgeMetaRegionMap &BackedgeMetaRegionMap,
                           std::set<MetaRegionBB *> &BlacklistedMetaregions) {
  for (auto RegionIt = MetaRegions.begin(); RegionIt != MetaRegions.end();
       RegionIt++) {
    MetaRegionBB &Region = *RegionIt;

    // Do not re-analyze blacklisted metaregions.
    if (BlacklistedMetaregions.count(&Region) == 0) {

      // Iterate over all the backedges present in the graph, if the current
      // region contains the source of a backedge, it should contain also the
      // the target of that backedge. If not, merge the two SCSs.
      for (EdgeDescriptor Backedge : Backedges) {
        bool FirstIn = Region.containsNode(Backedge.first);
        bool SecondIn = Region.containsNode(Backedge.second);
        bool AbnormalIncoming = FirstIn and not SecondIn;
        bool AbnormalOutgoing = not FirstIn and SecondIn;
        if (AbnormalIncoming or AbnormalOutgoing) {

          // Retrieve the Metaregion identified by the backedge with goes
          // goes outside the scope of the current Metaregion.
          MetaRegionBB *OtherRegion = BackedgeMetaRegionMap.at(Backedge);
          Region.mergeWith(*OtherRegion);

          // Blacklist the region which we have merged.
          BackedgeMetaRegionMap[Backedge] = &Region;
          BlacklistedMetaregions.insert(OtherRegion);
          return true;

          // Abort if we didn't find the metaregion to remove.
          revng_abort("Not found the region to merge with.");
        }
      }
    }
  }

  return false;
}

static void
simplifySCSAbnormalRetreating(MetaRegionBBVect &MetaRegions,
                              const std::set<EdgeDescriptor> &Backedges) {

  // Temporary map where to store the corrispondence between the backedge and
  // the SCS it gives origin to.
  // HACK: this should be done at the same time of the metaregion creation.
  unsigned MetaRegionIndex = 0;
  std::map<EdgeDescriptor, MetaRegionBB *> BackedgeMetaRegionMap;
  for (EdgeDescriptor Backedge : Backedges) {
    BackedgeMetaRegionMap[Backedge] = &MetaRegions.at(MetaRegionIndex);
    MetaRegionIndex++;
  }

  std::set<MetaRegionBB *> BlacklistedMetaregions;
  bool Changes = true;
  while (Changes) {
    Changes = mergeSCSAbnormalRetreating(MetaRegions,
                                         Backedges,
                                         BackedgeMetaRegionMap,
                                         BlacklistedMetaregions);
  }

  // Remove all the metaregion that have been merged with others, using the
  // erase/remove idiom.
  MetaRegions.erase(remove_if(MetaRegions.begin(),
                              MetaRegions.end(),
                              [&BlacklistedMetaregions](MetaRegionBB &M) {
                                return BlacklistedMetaregions.count(&M) == 1;
                              }),
                    MetaRegions.end());
}

static void sortMetaRegions(MetaRegionBBVect &MetaRegions) {
  std::sort(MetaRegions.begin(),
            MetaRegions.end(),
            [](MetaRegionBB &First, MetaRegionBB &Second) {
              return First.getNodes().size() < Second.getNodes().size();
            });
}

static bool
checkMetaregionConsistency(const MetaRegionBBVect &MetaRegions,
                           const std::set<EdgeDescriptor> &Backedges) {
  bool ComparisonState = true;
  for (const MetaRegionBB &MetaRegion : MetaRegions) {
    for (EdgeDescriptor Backedge : Backedges) {
      BasicBlockNodeBB *Source = Backedge.first;
      BasicBlockNodeBB *Target = Backedge.second;
      bool HasSource = MetaRegion.containsNode(Source);
      bool HasTarget = MetaRegion.containsNode(Target);
      revng_assert(HasSource == HasTarget);
      if (HasSource != HasTarget) {
        ComparisonState = false;
      }
    }
  }

  return ComparisonState;
}

static void
computeParents(MetaRegionBBVect &MetaRegions, MetaRegionBB *RootMetaRegion) {
  for (MetaRegionBB &MetaRegion1 : MetaRegions) {
    bool ParentFound = false;
    for (MetaRegionBB &MetaRegion2 : MetaRegions) {
      if (&MetaRegion1 != &MetaRegion2) {
        if (MetaRegion1.isSubSet(MetaRegion2)) {

          if (CombLogger.isEnabled()) {
            CombLogger << "For metaregion: " << &MetaRegion1 << "\n";
            CombLogger << "parent found\n";
            CombLogger << &MetaRegion2 << "\n";
          }

          MetaRegion1.setParent(&MetaRegion2);
          ParentFound = true;
          break;
        }
      }
    }

    if (!ParentFound) {

      if (CombLogger.isEnabled()) {
        CombLogger << "For metaregion: " << &MetaRegion1 << "\n";
        CombLogger << "no parent found\n";
      }

      MetaRegion1.setParent(RootMetaRegion);
    }
  }
}

static MetaRegionBBPtrVect applyPartialOrder(MetaRegionBBVect &V) {
  MetaRegionBBPtrVect OrderedVector;
  std::set<MetaRegionBB *> Processed;

  while (V.size() != Processed.size()) {
    for (auto RegionIt1 = V.begin(); RegionIt1 != V.end(); RegionIt1++) {
      if (Processed.count(&*RegionIt1) == 0) {
        bool FoundParent = false;
        for (auto RegionIt2 = V.begin(); RegionIt2 != V.end(); RegionIt2++) {
          if ((RegionIt1 != RegionIt2) and Processed.count(&*RegionIt2) == 0) {
            if ((*RegionIt1).getParent() == &*RegionIt2) {
              FoundParent = true;
              break;
            }
          }
        }

        if (FoundParent == false) {
          OrderedVector.push_back(&*RegionIt1);
          Processed.insert(&*RegionIt1);
          break;
        }
      }
    }
  }

  std::reverse(OrderedVector.begin(), OrderedVector.end());
  return OrderedVector;
}

static bool alreadyInMetaregion(MetaRegionBBVect &V, BasicBlockNodeBB *N) {

  // Scan all the metaregions and check if a node is already contained in one of
  // them
  for (MetaRegionBB &Region : V) {
    if (Region.containsNode(N)) {
      return true;
    }
  }

  return false;
}

static MetaRegionBBVect
createMetaRegions(const std::set<EdgeDescriptor> &Backedges) {
  std::map<BasicBlockNodeBB *, std::set<BasicBlockNodeBB *>> AdditionalSCSNodes;
  std::vector<std::pair<BasicBlockNodeBB *, std::set<BasicBlockNodeBB *>>>
    Regions;

  for (auto &Backedge : Backedges) {
    auto SCSNodes = findReachableNodes(Backedge.second, Backedge.first);
    AdditionalSCSNodes[Backedge.second].insert(SCSNodes.begin(),
                                               SCSNodes.end());

    if (CombLogger.isEnabled()) {
      CombLogger << "SCS identified by: ";
      CombLogger << Backedge.first->getNameStr() << " -> "
                 << Backedge.second->getNameStr() << "\n";
      CombLogger << "Is composed of nodes:\n";
      for (auto Node : SCSNodes) {
        CombLogger << Node->getNameStr() << "\n";
      }
    }

    Regions.push_back(std::make_pair(Backedge.second, SCSNodes));
  }

  // Include in the regions found before other possible sub-regions, if an edge
  // which is the target of a backedge is included in an outer region.
  for (auto &Region : Regions) {
    BasicBlockNodeBB *Head = Region.first;
    std::set<BasicBlockNodeBB *> &Nodes = Region.second;
    std::set<BasicBlockNodeBB *> AdditionalNodes;
    std::set<BasicBlockNodeBB *> OldNodes;
    do {
      OldNodes = Nodes;
      for (BasicBlockNodeBB *Node : Nodes) {
        if ((Node != Head) and (AdditionalSCSNodes.count(Node) != 0)) {
          CombLogger << "Adding additional nodes for region with head: ";
          CombLogger << Head->getNameStr();
          CombLogger << " and relative to node: ";
          CombLogger << Node->getNameStr() << "\n";
          AdditionalNodes.insert(AdditionalSCSNodes[Node].begin(),
                                 AdditionalSCSNodes[Node].end());
        }
      }
      Nodes.insert(AdditionalNodes.begin(), AdditionalNodes.end());
      AdditionalNodes.clear();
    } while (Nodes != OldNodes);
  }

  MetaRegionBBVect MetaRegions;
  int SCSIndex = 1;
  for (size_t I = 0; I < Regions.size(); ++I) {
    auto &SCS = Regions[I].second;
    MetaRegions.push_back(MetaRegionBB(SCSIndex, SCS, true));
    SCSIndex++;
  }
  return MetaRegions;
}

static void
removeFromRPOT(std::vector<BasicBlockNodeBB *> &RPOT, BasicBlockNodeBB *Node) {

  RPOT.erase(std::remove_if(RPOT.begin(),
                            RPOT.end(),
                            [Node](BasicBlockNodeBB *N) {
                              if (N == Node) {
                                return true;
                              }
                              return false;
                            }),
             RPOT.end());
}

char RestructureCFG::ID = 0;
static RegisterPass<RestructureCFG> X("restructure-cfg",
                                      "Apply RegionCFG restructuring "
                                      "transformation",
                                      true,
                                      true);

static cl::opt<std::string> MetricsOutputPath("restructure-metrics-output-dir",
                                              desc("Restructure metrics dir"),
                                              value_desc("restructure-dir"),
                                              cat(MainCategory));

void RestructureCFG::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<LoadModelWrapperPass>();
}

inline void
accumulateDuplicates(RegionCFG<llvm::BasicBlock *> &Region,
                     std::map<const llvm::BasicBlock *, size_t> &NDuplicates) {
  // Compute the NDuplicates, which will be used later.
  for (BasicBlockNodeBB *BBNode : Region.nodes()) {
    if (BBNode->isCode()) {
      auto *BB = BBNode->getOriginalNode();
      ++NDuplicates[BB];
    } else if (BBNode->isCollapsed()) {
      auto *BodyGraph = BBNode->getCollapsedCFG();
      accumulateDuplicates(*BodyGraph, NDuplicates);
    }
  }
}

static void LogMetaRegions(const MetaRegionBBPtrVect &MetaRegions,
                           const std::string &HeaderMsg) {
  if (CombLogger.isEnabled()) {
    CombLogger << '\n';
    CombLogger << HeaderMsg << '\n';
    for (const MetaRegionBB *Meta : MetaRegions) {
      CombLogger << '\n';
      CombLogger << Meta << '\n';
      CombLogger << "With index " << Meta->getIndex() << '\n';
      CombLogger << "With size " << Meta->nodes_size() << '\n';
      CombLogger << "Is composed of nodes:\n";
      const auto &Nodes = Meta->getNodes();
      for (auto *Node : Nodes) {
        CombLogger << Node->getNameStr() << '\n';
      }
      CombLogger << "Is SCS: " << Meta->isSCS() << '\n';
      CombLogger << "Has parent: ";
      if (Meta->getParent())
        CombLogger << Meta->getParent();
      else
        CombLogger << "nullptr";
      CombLogger << '\n';
    }
  }
}

static void LogMetaRegions(const MetaRegionBBVect &MetaRegions,
                           const std::string &HeaderMsg) {
  if (CombLogger.isEnabled()) {
    CombLogger << '\n';
    CombLogger << HeaderMsg << '\n';
    for (const MetaRegionBB &Meta : MetaRegions) {
      CombLogger << '\n';
      CombLogger << &Meta << '\n';
      CombLogger << "With index " << Meta.getIndex() << '\n';
      CombLogger << "With size " << Meta.nodes_size() << '\n';
      CombLogger << "Is composed of nodes:\n";
      const auto &Nodes = Meta.getNodes();
      for (auto *Node : Nodes) {
        CombLogger << Node->getNameStr() << '\n';
      }
      CombLogger << "Is SCS: " << Meta.isSCS() << '\n';
      CombLogger << "Has parent: ";
      if (Meta.getParent())
        CombLogger << Meta.getParent();
      else
        CombLogger << "nullptr";
      CombLogger << '\n';
    }
  }
}

bool RestructureCFG::runOnFunction(Function &F) {

  NDuplicates.clear();
  AST = ASTTree();

  DuplicationCounter = 0;
  UntangleTentativeCounter = 0;
  UntanglePerformedCounter = 0;

  // Skip non-isolated functions
  auto FTags = FunctionTags::TagsSet::from(&F);
  if (not FTags.contains(FunctionTags::Lifted))
    return false;

  // If we passed the `-single-decompilation` option to the command line, skip
  // decompilation for all the functions that are not the selected one.
  if (TargetFunction.size() != 0) {
    if (!F.getName().equals(TargetFunction.c_str())) {
      return false;
    }
  }

  // Clear graph object from the previous pass.
  RegionCFG<BasicBlock *> RootCFG;

  // Set names of the CFG region
  RootCFG.setFunctionName(F.getName());
  RootCFG.setRegionName("root");

  // Initialize the RegionCFG object
  RootCFG.initialize(&F);

  if (CombLogger.isEnabled()) {
    CombLogger << "Analyzing function: " << F.getName() << "\n";
    RootCFG.dumpCFGOnFile(F.getName(), "dots", "begin");
  }

  // Identify SCS regions.
  std::set<EdgeDescriptor> Backedges = getBackedges(RootCFG);
  if (CombLogger.isEnabled()) {
    CombLogger << "Backedges in the graph:\n";
    for (auto &Backedge : Backedges) {
      CombLogger << Backedge.first->getNameStr() << " -> "
                 << Backedge.second->getNameStr() << "\n";
    }
  }

  // Insert a dummy node for each retreating node.
  for (EdgeDescriptor Backedge : Backedges) {
    BasicBlockNodeBB *OriginalTarget = Backedge.second;
    BasicBlockNodeBB *Dummy = RootCFG.addArtificialNode();
    moveEdgeTarget(Backedge, Dummy);
    addPlainEdge(EdgeDescriptor(Dummy, OriginalTarget));
  }
  Backedges = getBackedges(RootCFG);

  // Check that the source node of each retreating edge is a dummy node.
  for (EdgeDescriptor Backedge : Backedges) {
    revng_assert(Backedge.first->isEmpty());
  }

  // Create meta regions
  MetaRegionBBVect MetaRegions = createMetaRegions(Backedges);
  LogMetaRegions(MetaRegions, "Metaregions after nothing:");

  // Simplify SCS if they contain an edge which goes outside the scope of the
  // current region.
  simplifySCSAbnormalRetreating(MetaRegions, Backedges);
  LogMetaRegions(MetaRegions, "Metaregions after first simplification:");
  revng_assert(checkMetaregionConsistency(MetaRegions, Backedges));

  // Simplify SCS in a fixed-point fashion.
  simplifySCS(MetaRegions);
  LogMetaRegions(MetaRegions, "Metaregions after second simplification:");
  revng_assert(checkMetaregionConsistency(MetaRegions, Backedges));

  // Sort the Metaregions in increasing number of composing nodes order.
  sortMetaRegions(MetaRegions);
  LogMetaRegions(MetaRegions, "Metaregions after second ordering:");

  // Compute parent relations for the identified SCSs.
  std::set<BasicBlockNodeBB *> Empty;
  MetaRegionBB RootMetaRegion(0, Empty);
  computeParents(MetaRegions, &RootMetaRegion);

  // Print metaregions after ordering.
  LogMetaRegions(MetaRegions, "Metaregions parent relationship:");

  // Find an ordering for the metaregions that satisfies the inclusion
  // relationship. We create a new "shadow" vector containing only pointers to
  // the "real" metaregions.
  MetaRegionBBPtrVect OrderedMetaRegions = applyPartialOrder(MetaRegions);

  // Print metaregions after ordering.
  LogMetaRegions(OrderedMetaRegions, "Metaregions after partial ordering:");

  ReversePostOrderTraversal<BasicBlockNodeBB *> ORPOT(&RootCFG.getEntryNode());

  // Create a std::vector from the reverse post order (we will later need
  // the removal operation)
  std::vector<BasicBlockNodeBB *> RPOT;
  for (BasicBlockNodeBB *BN : ORPOT) {
    RPOT.push_back(BN);
  }

  if (CombLogger.isEnabled()) {
    CombLogger << "\n";
    CombLogger << "Reverse post order is:\n";
    for (BasicBlockNodeBB *BN : RPOT) {
      CombLogger << BN->getNameStr() << "\n";
    }
    CombLogger << "Reverse post order end\n";
  }

  DominatorTreeBase<BasicBlockNodeBB, false> DT;
  DT.recalculate(RootCFG);

  // Reserve enough space for all the OrderedMetaRegions.
  // The following algorithms stores pointers to the elements of this vector, so
  // we need to make sure that no reallocation happens.
  std::vector<RegionCFG<BasicBlock *>> Regions(OrderedMetaRegions.size());

  for (MetaRegionBB *Meta : OrderedMetaRegions) {
    if (CombLogger.isEnabled()) {
      CombLogger << "\nAnalyzing region: " << Meta->getIndex() << "\n";

      auto &Nodes = Meta->getNodes();
      CombLogger << "Which is composed of nodes:\n";
      for (auto *Node : Nodes) {
        CombLogger << Node->getNameStr() << "\n";
      }

      CombLogger << "Dumping main graph snapshot before restructuring\n";
      RootCFG.dumpCFGOnFile(F.getName(),
                            "dots",
                            "Out-pre-" + std::to_string(Meta->getIndex()));
    }

    // Identify all the abnormal retreating edges in a SCS.
    std::set<EdgeDescriptor> Retreatings;
    std::set<BasicBlockNodeBB *> RetreatingTargets;
    for (EdgeDescriptor Backedge : Backedges) {
      if (Meta->containsNode(Backedge.first)) {

        // Check that the target of the retreating edge falls inside the current
        // SCS.
        revng_assert(Meta->containsNode(Backedge.second));

        Retreatings.insert(Backedge);
        RetreatingTargets.insert(Backedge.second);
      }
    }
    if (CombLogger.isEnabled()) {
      CombLogger << "Retreatings found:\n";
      for (EdgeDescriptor Retreating : Retreatings) {
        CombLogger << Retreating.first->getNameStr() << " -> ";
        CombLogger << Retreating.second->getNameStr() << "\n";
      }
    }

    // We need to update the backedges list removing the edges which have been
    // considered as retreatings of the SCS under analysis.
    for (EdgeDescriptor Retreating : Retreatings) {
      revng_assert(Backedges.count(Retreating) == 1);
      Backedges.erase(Retreating);
    }

    // Always take the fist node in RPOT which is a retreating target as entry,
    // candidate.
    BasicBlockNodeBB *FirstCandidate = nullptr;
    for (BasicBlockNodeBB *BN : RPOT) {
      if (Meta->containsNode(BN) and RetreatingTargets.count(BN)) {
        FirstCandidate = BN;
        break;
      }
    }

    revng_assert(FirstCandidate != nullptr);

    // Print out the name of the node that has been selected as head of the
    // region
    if (CombLogger.isEnabled()) {
      CombLogger << "Elected head is: " << FirstCandidate->getNameStr() << "\n";
    }

    bool NewHeadNeeded = RetreatingTargets.size() > 1;
    if (CombLogger.isEnabled()) {
      CombLogger << "New head needed: " << NewHeadNeeded << "\n";
    }

    BasicBlockNodeBB *Head = FirstCandidate;
    if (NewHeadNeeded) {
      // Create the dispatcher.
      Head = RootCFG.addEntryDispatcher();
      Meta->insertNode(Head);

      // For each target of the dispatcher add the edge and add it in the map.
      std::map<BasicBlockNodeBB *, unsigned> RetreatingIdxMap;
      for (auto &Group : llvm::enumerate(RetreatingTargets)) {
        BasicBlockNodeBB *Target = Group.value();
        unsigned Idx = Group.index();

        RetreatingIdxMap[Target] = Idx;

        using edge_label_t = typename BasicBlockNodeBB::edge_label_t;
        edge_label_t Labels;
        Labels.insert(Idx);
        using EdgeInfo = BasicBlockNodeBB::EdgeInfo;
        EdgeInfo EI = { Labels, false };
        addEdge(EdgeDescriptor(Head, Target), EI);
      }

      for (EdgeDescriptor R : Retreatings) {
        BasicBlockNodeBB *OriginalSource = R.first;

        // If the original source is a set node, move it after the entry
        // dispatcher.
        unsigned Idx = RetreatingIdxMap.at(R.second);
        if (OriginalSource->isSet()) {
          BasicBlockNodeBB *OldSetNode = OriginalSource;
          revng_assert(OldSetNode->predecessor_size() == 1);
          BasicBlockNodeBB *Predecessor = *OldSetNode->predecessors().begin();
          auto *SetNode = RootCFG.addSetStateNode(Idx, OldSetNode->getName());
          Meta->insertNode(SetNode);
          moveEdgeTarget(EdgeDescriptor(Predecessor, OldSetNode), Head);
        } else {
          auto *SetNode = RootCFG.addSetStateNode(Idx, R.second->getName());
          Meta->insertNode(SetNode);
          moveEdgeTarget(EdgeDescriptor(R.first, R.second), SetNode);
          addPlainEdge(EdgeDescriptor(SetNode, Head));
        }
      }

      // Move the incoming edge from the old head to new one.
      std::vector<BasicBlockNodeBB *> Predecessors;
      for (BasicBlockNodeBB *Predecessor : FirstCandidate->predecessors())
        Predecessors.push_back(Predecessor);

      for (BasicBlockNodeBB *Predecessor : Predecessors)
        if (not Meta->containsNode(Predecessor))
          moveEdgeTarget(EdgeDescriptor(Predecessor, FirstCandidate), Head);
    }

    revng_assert(Head != nullptr);
    if (CombLogger.isEnabled()) {
      CombLogger << "New head name is: " << Head->getNameStr() << "\n";
    }

    // Successor refinement step.
    std::set<BasicBlockNodeBB *> Successors = Meta->getSuccessors();

    if (CombLogger.isEnabled()) {
      CombLogger << "Region successors are:\n";
      for (BasicBlockNodeBB *Node : Successors) {
        CombLogger << Node->getNameStr() << "\n";
      }
    }

    bool AnotherIteration = true;
    while (AnotherIteration and Successors.size() > 1) {
      AnotherIteration = false;
      std::set<EdgeDescriptor> OutgoingEdges = Meta->getOutEdges();

      std::vector<BasicBlockNodeBB *> Frontiers;
      std::map<BasicBlockNodeBB *, pair<BasicBlockNodeBB *, BasicBlockNodeBB *>>
        EdgeExtremal;

      for (EdgeDescriptor Edge : OutgoingEdges) {
        BasicBlockNodeBB *Frontier = RootCFG.addArtificialNode("frontier "
                                                               "dummy");
        BasicBlockNodeBB *OldSource = Edge.first;
        BasicBlockNodeBB *OldTarget = Edge.second;
        EdgeExtremal[Frontier] = make_pair(OldSource, OldTarget);
        moveEdgeTarget(Edge, Frontier);
        addPlainEdge(EdgeDescriptor(Frontier, OldTarget));
        Meta->insertNode(Frontier);
        Frontiers.push_back(Frontier);
      }

      DT.recalculate(RootCFG);
      for (BasicBlockNodeBB *Frontier : Frontiers) {
        for (BasicBlockNodeBB *Successor : Successors) {
          if ((DT.dominates(Head, Successor))
              and (DT.dominates(Frontier, Successor))
              and not alreadyInMetaregion(MetaRegions, Successor)) {
            Meta->insertNode(Successor);
            AnotherIteration = true;
            if (CombLogger.isEnabled()) {
              CombLogger << "Identified new candidate for successor "
                            "refinement:";
              CombLogger << Successor->getNameStr() << "\n";
            }
          }
        }
      }

      // Remove the frontier nodes since we do not need them anymore.
      for (BasicBlockNodeBB *Frontier : Frontiers) {
        BasicBlockNodeBB *OriginalSource = EdgeExtremal[Frontier].first;
        BasicBlockNodeBB *OriginalTarget = EdgeExtremal[Frontier].second;
        moveEdgeTarget({ OriginalSource, Frontier }, OriginalTarget);
        RootCFG.removeNode(Frontier);
        Meta->removeNode(Frontier);
      }

      Successors = Meta->getSuccessors();
    }

    // First Iteration outlining.
    // Clone all the nodes of the SCS except for the head.
    std::map<BasicBlockNodeBB *, BasicBlockNodeBB *> ClonedMap;
    std::vector<BasicBlockNodeBB *> OutlinedNodes;
    for (BasicBlockNodeBB *Node : Meta->nodes()) {
      if (Node != Head) {
        BasicBlockNodeBB *Clone = RootCFG.cloneNode(*Node);
        Clone->setName(Node->getName().str() + " outlined");
        ClonedMap[Node] = Clone;

        // Add the nodes to the additional vector
        OutlinedNodes.push_back(Clone);
      }
    }

    // Restore edges between cloned nodes.
    for (BasicBlockNodeBB *Node : Meta->nodes()) {
      if (Node != Head) {

        // Handle outgoing edges from SCS nodes.
        for (const auto &[Successor, Labels] : Node->labeled_successors()) {
          revng_assert(not Backedges.count(EdgeDescriptor(Node, Successor)));
          using ED = EdgeDescriptor;
          auto *NewEdgeSrc = ClonedMap.at(Node);
          auto *NewEdgeTgt = Successor;
          if (Meta->containsNode(Successor)) {
            // Handle edges pointing inside the SCS.
            if (Successor == Head) {
              // Retreating edges should point to the new head.
              NewEdgeTgt = Head;
            } else {
              // Other edges should be restored between cloned nodes.
              NewEdgeTgt = ClonedMap.at(Successor);
            }
          }
          addEdge(ED(NewEdgeSrc, NewEdgeTgt), Labels);
        }

        // We need this temporary vector to avoid invalidating iterators.
        std::vector<BasicBlockNodeBB *> Predecessors;
        for (BasicBlockNodeBB *Predecessor : Node->predecessors()) {
          Predecessors.push_back(Predecessor);
        }
        for (BasicBlockNodeBB *Predecessor : Predecessors) {
          if (not Meta->containsNode(Predecessor)) {
            // Is the edge we are moving a backedge ?.
            if (CombLogger.isEnabled()) {
              CombLogger << "Index region: " << Meta->getIndex() << "\n";
              CombLogger << "Backedge that we would insert: "
                         << Predecessor->getNameStr() << " -> "
                         << Node->getNameStr() << "\n";
            }

            // Are we moving a backedge with the first iteration outlining?
            revng_assert(not Backedges.count({ Predecessor, Node }));

            moveEdgeTarget(EdgeDescriptor(Predecessor, Node),
                           ClonedMap.at(Node));
          }
        }
      }
    }

    // Vector which contains the additional set nodes that set the default value
    // for the entry dispatcher.
    std::vector<BasicBlockNodeBB *> DefaultEntrySet;

    // Default set node for entry dispatcher.
    if (NewHeadNeeded) {
      revng_assert(Head->isDispatcher());

      llvm::SmallPtrSet<BasicBlockNodeBB *, 8> SetCandidates;
      for (BasicBlockNodeBB *Pred : Head->predecessors())
        if (not Pred->isSet())
          SetCandidates.insert(Pred);

      unsigned long Value = RetreatingTargets.size() - 1;
      for (BasicBlockNodeBB *Pred : SetCandidates) {
        BasicBlockNodeBB *Set = RootCFG.addSetStateNode(Value, Head->getName());
        DefaultEntrySet.push_back(Set);
        EdgeDescriptor PredToHead = { Pred, Head };
        EdgeDescriptor SetToHead = { Set, Head };
        moveEdgeTarget(PredToHead, Set);
        addPlainEdge(SetToHead);

        // Update the backedges set. Basically, when we place the default set
        // node in case of an entry dispatcher, we need to take care to verify
        // if the edge we are "moving" (inserting the set node before it) is a
        // backedge, and in case update the information regarding the backedges
        // present in the graph accordingly (the backedge becomes the edge
        // departing from the set node).
        auto BackEdgeIt = Backedges.find(PredToHead);
        if (BackEdgeIt != Backedges.end()) {
          Backedges.insert(SetToHead);
          Backedges.erase(BackEdgeIt);
        }
      }
    }

    // Exit dispatcher creation.

    // Deduplicate region successor across backedges. If a region has a dummy
    // successor that is a dummy backedge, we want to look across it, so that we
    // can detect if two backedges actually jump to the same target, and emit
    // only one case in the exit dispatcher. This saves us from having to take
    // care later of collapsing the two (or more) dummy branches coming out from
    // the exit dispatcher with different labels. With this strategy we already
    // emit a single label in the first place.
    std::set<BasicBlockNodeBB *> DeduplicatedRegionSuccessors;
    std::map<BasicBlockNodeBB *, BasicBlockNodeBB *> DeduplicationMap;
    {
      std::map<BasicBlockNodeBB *, BasicBlockNodeBB *> BackedgeToSucc;
      for (BasicBlockNodeBB *Succ : Successors) {
        if (Succ->isEmpty() and Succ->successor_size()) {
          revng_assert(Succ->successor_size() == 1);
          BasicBlockNodeBB *BackedgeTgt = *Succ->successors().begin();
          // Lookup if wa have already found this backedge target from another
          // exit succesor.
          const auto &[It, New] = BackedgeToSucc.insert({ BackedgeTgt, Succ });
          if (New) {
            // If we haven't, add the successor in the deduplicated successors
            DeduplicatedRegionSuccessors.insert(Succ);
            DeduplicationMap[Succ] = Succ;
          } else {
            // If we have, map thie successor to the old successor we've found
            // with the same backedge target.
            DeduplicationMap[Succ] = It->second;
          }
        } else {
          DeduplicatedRegionSuccessors.insert(Succ);
          DeduplicationMap[Succ] = Succ;
        }
      }
    }

    bool NewExitNeeded = DeduplicatedRegionSuccessors.size() > 1;
    if (CombLogger.isEnabled()) {
      CombLogger << "New exit needed: " << NewExitNeeded << "\n";
    }

    std::vector<BasicBlockNodeBB *> ExitDispatcherNodes;
    BasicBlockNodeBB *Exit = nullptr;
    if (NewExitNeeded) {

      // Create the dispatcher.
      Exit = RootCFG.addExitDispatcher();
      ExitDispatcherNodes.push_back(Exit);

      // For each target of the dispatcher add the edge and add it in the map.
      std::map<BasicBlockNodeBB *, unsigned> SuccessorsIdxMap;
      for (auto &Group : llvm::enumerate(DeduplicatedRegionSuccessors)) {
        BasicBlockNodeBB *Successor = Group.value();
        unsigned Idx = Group.index();

        SuccessorsIdxMap[Successor] = Idx;

        using edge_label_t = typename BasicBlockNodeBB::edge_label_t;
        edge_label_t Labels;
        Labels.insert(Idx);
        using EdgeInfo = typename BasicBlockNodeBB::EdgeInfo;
        EdgeInfo EI = { Labels, false };
        addEdge(EdgeDescriptor(Exit, Successor), EI);
      }

      std::set<EdgeDescriptor> OutEdges = Meta->getOutEdges();
      for (EdgeDescriptor Edge : OutEdges) {
        // We should not be adding new backedges.
        revng_assert(not Backedges.count(Edge));

        unsigned Idx = SuccessorsIdxMap.at(DeduplicationMap.at(Edge.second));
        auto *IdxSetNode = RootCFG.addSetStateNode(Idx, Edge.second->getName());
        Meta->insertNode(IdxSetNode);
        moveEdgeTarget(Edge, IdxSetNode);
        addPlainEdge(EdgeDescriptor(IdxSetNode, Edge.second));
      }

      revng_log(CombLogger, "New exit name is: " << Exit->getNameStr());
    }

    // Collapse Region.
    // Create a new RegionCFG object for representing the collapsed region and
    // populate it with the internal nodes.
    Regions.push_back(RegionCFG<BasicBlock *>());
    RegionCFG<BasicBlock *> &CollapsedGraph = Regions.back();
    RegionCFG<BasicBlock *>::BBNodeMap SubstitutionMap{};
    CollapsedGraph.setFunctionName(F.getName());
    CollapsedGraph.setRegionName(std::to_string(Meta->getIndex()));
    revng_assert(Head != nullptr);

    // Create the collapsed node in the outer region.
    BasicBlockNodeBB *Collapsed = RootCFG.createCollapsedNode(&CollapsedGraph);

    {
      // Update the backedges set, checking that if a backedge of an outer
      // region pointed to a node that now has been collapsed, now should point
      // to the collapsed node, and that does not exists at this point a
      // backedge which has as source a node that will be collapsed.
      std::set<EdgeDescriptor> NewBackedges;
      auto BackEdgeIt = Backedges.begin();
      while (BackEdgeIt != Backedges.end()) {
        const auto [Source, Target] = *BackEdgeIt;
        revng_assert(not Meta->containsNode(Source));
        if (Meta->containsNode(Target)) {
          revng_assert(Target == Head);
          NewBackedges.insert({ Source, Collapsed });
          BackEdgeIt = Backedges.erase(BackEdgeIt);
        } else {
          ++BackEdgeIt;
        }
      }
      Backedges.merge(NewBackedges);
    }

    CollapsedGraph.insertBulkNodes(Meta->getNodes(), Head, SubstitutionMap);

    // Connect the break and continue nodes with the necessary edges (we create
    // a new break/continue node for each outgoing or retreating edge).
    CollapsedGraph.connectContinueNode();
    std::set<EdgeDescriptor> OutgoingEdges = Meta->getOutEdges();
    CollapsedGraph.connectBreakNode(OutgoingEdges, SubstitutionMap);

    // Connect the old incoming edges to the collapsed node.
    std::set<EdgeDescriptor> IncomingEdges = Meta->getInEdges();
    for (EdgeDescriptor Edge : IncomingEdges) {
      BasicBlockNodeBB *OldSource = Edge.first;
      revng_assert(Edge.second == Head);

      // Check if the old edge was a backedge edge, and in case update the
      // information about backedges accordingly.
      if (Backedges.count(Edge) == 1) {
        Backedges.erase(Edge);
        Backedges.insert(EdgeDescriptor(OldSource, Collapsed));
      }

      moveEdgeTarget(Edge, Collapsed);
    }

    // Connect the outgoing edges to the collapsed node.
    if (NewExitNeeded) {
      revng_assert(Exit != nullptr);
      addPlainEdge(EdgeDescriptor(Collapsed, Exit));
    } else {

      // Double check that we have at most a single successor
      revng_assert(DeduplicatedRegionSuccessors.size() <= 1);
      if (DeduplicatedRegionSuccessors.size() == 1) {

        // Connect the collapsed node to the unique successor
        BasicBlockNodeBB *Successor = *DeduplicatedRegionSuccessors.begin();
        addPlainEdge(EdgeDescriptor(Collapsed, Successor));
      }
    }

    // Remove collapsed nodes from the outer region.
    for (BasicBlockNodeBB *Node : Meta->nodes()) {
      if (CombLogger.isEnabled()) {
        CombLogger << "Removing from main graph node :" << Node->getNameStr()
                   << "\n";
      }
      RootCFG.removeNode(Node);
      removeFromRPOT(RPOT, Node);
    }

    // Substitute in the other SCSs the nodes of the current SCS with the
    // collapsed node and the exit dispatcher structure.
    for (MetaRegionBB *OtherMeta : OrderedMetaRegions) {
      if (OtherMeta != Meta) {
        OtherMeta->updateNodes(Meta->getNodes(),
                               Collapsed,
                               ExitDispatcherNodes,
                               DefaultEntrySet,
                               OutlinedNodes);
      }
    }

    // Replace the pointers inside SCS.
    Meta->replaceNodes(CollapsedGraph.getNodes());

    // Remove useless nodes inside the SCS (like dandling break/continue)
    CollapsedGraph.removeNotReachables(OrderedMetaRegions);

    // Serialize the newly collapsed SCS region.
    if (CombLogger.isEnabled()) {
      CombLogger << "Dumping CFG of metaregion " << Meta->getIndex() << "\n";
      CollapsedGraph.dumpCFGOnFile(F.getName(),
                                   "dots",
                                   "In-" + std::to_string(Meta->getIndex()));
      CombLogger << "Dumping main graph snapshot post restructuring\n";
      RootCFG.dumpCFGOnFile(F.getName(),
                            "dots",
                            "Out-post-" + std::to_string(Meta->getIndex()));
    }

    // Remove not reachables nodes from the graph at each iteration.
    RootCFG.removeNotReachables(OrderedMetaRegions);

    // Check that the newly created collapsed region is acyclic.
    revng_assert(CollapsedGraph.isDAG());
  }

  // Serialize the newly collapsed SCS region.
  if (CombLogger.isEnabled()) {
    CombLogger << "Dumping main graph before final purge\n";
    RootCFG.dumpCFGOnFile(F.getName(), "dots", "Final-before-purge");
  }

  // Remove not reachables nodes from the main final graph.
  RootCFG.removeNotReachables(OrderedMetaRegions);

  // Serialize the newly collapsed SCS region.
  if (CombLogger.isEnabled()) {
    CombLogger << "Dumping main graph after final purge\n";
    RootCFG.dumpCFGOnFile(F.getName(), "dots", "Final-after-purge");
  }

  // Print metaregions after ordering.
  LogMetaRegions(OrderedMetaRegions, "Metaregions after collapse:");

  // Check that the root region is acyclic at this point.
  revng_assert(RootCFG.isDAG());

  // Collect statistics
  unsigned InitialWeight = 0;
  if (MetricsOutputPath.getNumOccurrences()) {
    revng_assert(MetricsOutputPath.getNumOccurrences() == 1);
    // Compute the initial weight of the CFG.
    for (BasicBlockNodeBB *BBNode : RootCFG.nodes()) {
      InitialWeight += BBNode->getWeight();
    }
  }

  // Invoke the AST generation for the root region.
  std::map<RegionCFG<llvm::BasicBlock *> *, ASTTree> CollapsedMap;
  generateAst(RootCFG, AST, CollapsedMap);

  accumulateDuplicates(RootCFG, NDuplicates);

  // Scorporated this part which was previously inside the `generateAst` to
  // avoid having it run twice or more (it was run inside the recursive step
  // of the `generateAst`, and then another time for the final root AST, which
  // now is directly the entire AST, since there's no flattening anymore).
  normalize(AST, F.getName());

  // Serialize final AST on file
  if (CombLogger.isEnabled())
    AST.dumpASTOnFile(F.getName(), "ast", "Final");

  // Serialize the collected metrics in the outputfile.
  if (MetricsOutputPath.getNumOccurrences()) {
    // Compute the increase in weight, on the AST
    unsigned FinalWeight = 0;
    for (ASTNode *N : AST.nodes()) {
      switch (N->getKind()) {
      case ASTNode::NK_Scs:
      case ASTNode::NK_If:
      case ASTNode::NK_Switch: {
        // Control-flow nodes emit single constructs, so we just increase the
        // weight by one.
        // Control-flow nodes would also have nested scopes (then-else for if,
        // cases for switch, loop body for scs). However, those nodes are visted
        // separately, and will be accounted for later.
        ++FinalWeight;
      } break;
      case ASTNode::NK_Set:
      case ASTNode::NK_Break:
      case ASTNode::NK_SwitchBreak:
      case ASTNode::NK_Continue: {
        // These AST Nodes are emitted as single instructions.
        // Just increase the weight by one.
        ++FinalWeight;
      } break;
      case ASTNode::NK_List: {
        // Sequence nodes are just scopes, they don't have a real weight.
        // Their weight is just sum of the weights of the nodes they contain,
        // that will be visited nevertheless.
      } break;
      case ASTNode::NK_Code: {
        auto *BB = cast<CodeNode>(N)->getOriginalBB();
        revng_assert(BB);
        FinalWeight += WeightTraits<llvm::BasicBlock *>::getWeight(BB);
      } break;
      default:
        revng_abort("unxpected AST node");
      }
    }

    float Increase = float(FinalWeight) / float(InitialWeight);

    std::ofstream Output;
    const char *FunctionName = F.getName().data();
    std::ostream &OutputStream = pathToStream(MetricsOutputPath + "/"
                                                + FunctionName,
                                              Output);
    OutputStream << "function,"
                    "duplications,percentage,tuntangle,puntangle,iweight\n";
    OutputStream << F.getName().data() << "," << DuplicationCounter << ","
                 << Increase << "," << UntangleTentativeCounter << ","
                 << UntanglePerformedCounter << "," << InitialWeight << "\n";
  }

  return false;
}
