//===- Partitioner.cpp - SAVIO groups and the fraction search -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "llvm/Transforms/Vectorize/SAVIO/Availability.h"
#include "PartitionerUtils.h"
#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include "llvm/Transforms/Vectorize/SAVIO/SearchCore.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <tuple>
#include <string>
#include <utility>
#include <cfloat>

using namespace llvm;
using namespace llvm::savio;

#define DEBUG_TYPE "savio"

cl::opt<double> llvm::SAVIOMargin(
    "savio-margin", cl::init(0.05), cl::Hidden,
    cl::desc("Relative reduction of T a SAVIO step must reach to be taken"));

cl::opt<bool> llvm::SAVIOForce(
    "savio-force", cl::init(false), cl::Hidden,
    cl::desc("Move every candidate group regardless of cost, in either "
             "direction: scalar groups to the vector unit (Singleton "
             "Vectorize), vector groups to scalar operations (Scalarize) "
             "(for correctness tests only)"));

cl::opt<int> llvm::SAVIOForceCount(
    "savio-force-count", cl::init(-1), cl::Hidden,
    cl::desc("Move the first k instances (program order) of the slice class "
             "with the most instances in each region, whichever direction "
             "that class moves (Singleton Vectorize or Scalarize), and "
             "nothing else (-1 = off; for calibration sweeps)"));

static cl::opt<bool> EnableMultiBlockLoops(
    "savio-multi-block-loops", cl::init(true),
    cl::desc("Allow forming acyclic regions for basic blocks within multi-block loops"));

/// Singleton Vectorize gives no intrinsic flag operands.  Refuse operations
/// whose poison/undef semantics are stronger than an ordinary scalar
/// operation, rather than silently dropping nuw/nsw/exact.

/// Returns true if user \p U is a simple (non-atomic, non-volatile) store
/// storing value \p V to some memory address (and not storing into V itself).
static bool isSimpleStoreOf(const User *U, const Value *V) {
  auto *S = dyn_cast<StoreInst>(U);
  return S && S->isSimple() && S->getValueOperand() == V &&
         S->getPointerOperand() != V;
}

/// Returns true if the value computed by instruction \p I flows into an address
/// calculation (e.g. as an index/offset to a GetElementPtrInst or an IntToPtrInst)
/// through casts, PHIs, or other in-scope arithmetic within the same loop or block.
///
/// WHY: Address calculations must reside in General Purpose Registers (GPRs).
/// Vectorizing an operation that produces or feeds an in-region address would force
/// an expensive vector-to-GPR transfer instruction before every memory access,
/// eliminating any performance advantage. Out-of-region uses (such as after loop
/// exit) receive live-outs in GPRs at region exits and incur zero in-region overhead.
static bool reachesAddress(const Instruction &I, const Loop *L,
                           const BasicBlock *BB) {
  SmallVector<const Instruction *, 8> Work{&I};
  SmallPtrSet<const Instruction *, 8> Seen{&I};
  while (!Work.empty()) {
    const Instruction *Cur = Work.pop_back_val();
    for (const User *U : Cur->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI)
        continue;
      // Only penalize address calculations within the same region (in-loop or
      // in-block). Uses outside the region receive live-out values in GPRs at
      // region exit and do not incur in-loop vector-to-GPR transfers.
      if (L ? !L->contains(UI) : UI->getParent() != BB)
        continue;
      if (isa<GetElementPtrInst>(UI) || isa<IntToPtrInst>(UI))
        return true;
      if (isa<SExtInst>(UI) || isa<ZExtInst>(UI) || isa<TruncInst>(UI) ||
          isa<PHINode>(UI) ||
          (isCandidate(*UI) && bitCountName(*UI).empty())) {
        if (Seen.insert(UI).second)
          Work.push_back(UI);
      }
    }
  }
  return false;
}

/// Returns true if all users of \p I outside its recurrence group only feed
/// branch comparisons or terminators (i.e. loop branch control). Vectorizing
/// pure loop-counter recurrences provides no data computation and forces
/// expensive vector-to-GPR transfers on every loop iteration.
/// Returns true if all users of \p I outside its recurrence group (including
/// carried PHIs) only feed branch comparisons or terminators (i.e. loop branch
/// control). Vectorizing pure loop-counter recurrences provides no data
/// computation and forces expensive vector-to-GPR transfers on every loop
/// iteration.
static bool onlyFeedsLoopControl(const Instruction *I,
                                 ArrayRef<Instruction *> Ops,
                                 ArrayRef<PHINode *> Phis) {
  for (const User *U : I->users()) {
    if (is_contained(Ops, U) || is_contained(Phis, U))
      continue;
    auto *Cmp = dyn_cast<CmpInst>(U);
    if (!Cmp)
      return false;
    for (const User *CU : Cmp->users()) {
      auto *CUI = dyn_cast<Instruction>(CU);
      if (!CUI || !CUI->isTerminator())
        return false;
    }
  }
  return true;
}

namespace {

/// Disjoint-set data structure (Union-Find with path compression) over integer indices.
struct DisjointSets {
  SmallVector<unsigned, 16> Parent;
  unsigned add() {
    Parent.push_back(Parent.size());
    return Parent.size() - 1;
  }
  unsigned find(unsigned X) {
    while (Parent[X] != X)
      X = Parent[X] = Parent[Parent[X]];
    return X;
  }
  void unite(unsigned A, unsigned B) {
    A = find(A);
    B = find(B);
    if (A != B)
      Parent[std::max(A, B)] = std::min(A, B);
  }
};

/// DFS frame for iterative Tarjan's SCC search.
struct TarjanFrame {
  unsigned Node;
  unsigned NextSucc;
};

} // namespace

/// Computes the strongly connected components (SCCs) of the region graph
/// considering both same-iteration def-use edges and loop-carried dependencies,
/// using an iterative implementation of Tarjan's algorithm.
///
/// Returns a vector mapping each node index to its dense SCC component ID.
/// \p HasCycle is populated with a boolean per component indicating whether the
/// component forms a recurrence cycle (contains >1 node, or a self-loop).
static SmallVector<unsigned, 32> computeSCCs(const RegionGraph &RG,
                                             SmallVectorImpl<bool> &HasCycle) {
  const unsigned NumNodes = RG.Insts.size();
  SmallVector<SmallVector<unsigned, 2>, 32> Successors(NumNodes);
  for (unsigned NodeIdx = 0; NodeIdx != NumNodes; ++NodeIdx) {
    for (unsigned PredIdx : RG.Preds[NodeIdx])
      Successors[PredIdx].push_back(NodeIdx);
    for (const auto &CarriedEdge : RG.Carried[NodeIdx])
      Successors[CarriedEdge.first].push_back(NodeIdx);
  }

  const unsigned Unvisited = ~0u;
  SmallVector<unsigned, 32> DfsIndex(NumNodes, Unvisited);
  SmallVector<unsigned, 32> LowLink(NumNodes, 0);
  SmallVector<unsigned, 32> NodeComponent(NumNodes, Unvisited);
  SmallVector<bool, 32> OnStack(NumNodes, false);
  SmallVector<unsigned, 32> SccStack;
  SmallVector<TarjanFrame, 32> DfsWorkStack;

  unsigned NextIndex = 0;
  unsigned NumComponents = 0;

  for (unsigned Root = 0; Root != NumNodes; ++Root) {
    if (DfsIndex[Root] != Unvisited)
      continue;

    DfsWorkStack.push_back({Root, 0});
    DfsIndex[Root] = LowLink[Root] = NextIndex++;
    SccStack.push_back(Root);
    OnStack[Root] = true;

    while (!DfsWorkStack.empty()) {
      auto &[CurrNode, NextSuccIdx] = DfsWorkStack.back();
      if (NextSuccIdx < Successors[CurrNode].size()) {
        unsigned SuccNode = Successors[CurrNode][NextSuccIdx++];
        if (DfsIndex[SuccNode] == Unvisited) {
          DfsIndex[SuccNode] = LowLink[SuccNode] = NextIndex++;
          SccStack.push_back(SuccNode);
          OnStack[SuccNode] = true;
          DfsWorkStack.push_back({SuccNode, 0});
        } else if (OnStack[SuccNode]) {
          LowLink[CurrNode] = std::min(LowLink[CurrNode], DfsIndex[SuccNode]);
        }
        continue;
      }

      unsigned FinishedNode = CurrNode;
      DfsWorkStack.pop_back();
      if (!DfsWorkStack.empty()) {
        unsigned ParentNode = DfsWorkStack.back().Node;
        LowLink[ParentNode] = std::min(LowLink[ParentNode], LowLink[FinishedNode]);
      }

      // Root of an SCC found.
      if (LowLink[FinishedNode] == DfsIndex[FinishedNode]) {
        unsigned SccSize = 0;
        unsigned MemberNode = 0;
        do {
          MemberNode = SccStack.pop_back_val();
          OnStack[MemberNode] = false;
          NodeComponent[MemberNode] = NumComponents;
          ++SccSize;
        } while (MemberNode != FinishedNode);

        bool IsCycle = SccSize > 1;
        if (!IsCycle) {
          // Check for self-loop via carried dependencies.
          for (const auto &CarriedEdge : RG.Carried[FinishedNode])
            IsCycle |= (CarriedEdge.first == FinishedNode);
        }
        HasCycle.push_back(IsCycle);
        ++NumComponents;
      }
    }
  }
  return NodeComponent;
}

/// ToScalar groups: the loads and stores the slice owns. A vector load owned by
/// the slice has every user in the slice, so Scalarize splits it into scalar
/// loads; a live-out whose users outside the slice are all simple stores of it
/// is stored lane by lane with scalar stores. Anything else keeps its vector
/// load/store and the boundary is a lane extract/insert. Only the region's own
/// loads and stores are owned.
static void computeOwnership(Group &G, const BasicBlock &BB) {
  auto InOps = [&](const User *U) {
    auto *UI = dyn_cast<Instruction>(U);
    return UI && is_contained(G.Ops, UI);
  };
  for (Instruction *I : G.Ops)
    for (Value *V : I->operands())
      if (auto *LD = dyn_cast<LoadInst>(V))
        if (LD->getParent() == &BB && LD->isSimple() &&
            all_of(LD->users(), InOps) && !is_contained(G.OwnedLoads, LD))
          G.OwnedLoads.push_back(LD);
  for (Instruction *I : G.LiveOuts) {
    auto OwnedStoreOf = [&](const User *U) {
      auto *S = dyn_cast<StoreInst>(U);
      return S && S->getParent() == &BB && isSimpleStoreOf(U, I);
    };
    if (!all_of(I->users(),
                [&](const User *U) { return InOps(U) || OwnedStoreOf(U); }))
      continue;
    for (User *U : I->users())
      if (auto *S = dyn_cast<StoreInst>(U))
        if (!InOps(U) && !is_contained(G.OwnedStores, S))
          G.OwnedStores.push_back(S);
  }
}

bool savio::liveOutNeedsInsert(const Group &G, const Instruction &I) {
  return !all_of(I.users(), [&](const User *U) {
    return isInGroup(G, U) || is_contained(G.OwnedStores, dyn_cast<StoreInst>(U));
  });
}

/// A canonical description of the group: operations in program order as
/// opcode(operand-refs), where an operand-ref is a position in the group, a
/// constant's value or the kind of input (numbered by first occurrence within
/// the group, so `mul x,x` differs from `mul x,y`), then the live-out
/// positions. The region is part of the key: slices of different regions are
/// never instances of each other.
static std::string sliceKey(const Group &G, const Loop *L, unsigned RegionNo) {
  DenseMap<const Value *, unsigned> InNo;
  DenseMap<const Value *, unsigned> Pos;
  for (unsigned K = 0; K != G.Ops.size(); ++K)
    Pos[G.Ops[K]] = K;
  std::string Key = (Twine(RegionNo) + (G.Recurrence ? "R:" : "S:")).str();
  raw_string_ostream OS(Key);
  if (G.Dir == Direction::ToScalar)
    OS << "scalarize " << *G.Ops[0]->getType() << ':';
  for (const Instruction *I : G.Ops) {
    OS << opName(*I) << '(';
    bool First = true;
    for (const Value *V : dataOperands(*I)) {
      if (!First)
        OS << ',';
      First = false;
      auto It = Pos.find(V);
      if (It != Pos.end()) {
        OS << '#' << It->second;
        continue;
      }
      if (auto *C = dyn_cast<ConstantInt>(V)) {
        OS << 'c' << C->getValue();
        continue;
      }
      if (auto *C = dyn_cast<Constant>(V)) {
        OS << "const<";
        C->print(OS, /*IsForDebug=*/false);
        OS << '>';
        continue;
      }
      if (auto *P = dyn_cast<PHINode>(V);
          P && is_contained(G.CarriedPhis, P)) {
        // The value the phi carries: the latch operation's position and the
        // distance (a phi of a phi is never joined, so 1).
        Value *Latch = P->getIncomingValueForBlock(P->getParent());
        auto LIt = Pos.find(Latch);
        OS << "phi@#" << (LIt != Pos.end() ? LIt->second : ~0u) << "/d1";
        continue;
      }
      if (isa<Argument>(V))
        OS << "arg";
      else if (isa<LoadInst>(V))
        OS << "load"
           << (is_contained(G.OwnedLoads, cast<LoadInst>(V)) ? "!own" : "");
      else if (L && L->isLoopInvariant(V))
        OS << "invariant";
      else
        OS << "inst";
      OS << ":in" << InNo.try_emplace(V, InNo.size()).first->second;
    }
    OS << ");";
  }
  OS << "out=";
  for (const Instruction *I : G.LiveOuts)
    OS << Pos[I]
       << (G.Dir == Direction::ToScalar && !liveOutNeedsInsert(G, *I) ? "s"
                                                                         : "")
       << ',';
  return Key;
}

/// Identifies and constructs candidate transformation groups for basic block \p BB:
/// 1. Discovers self-contained loop-carried recurrences (keeping loop PHIs in vector form).
/// 2. Clusters acyclic candidate instructions into use-def connected slices.
/// 3. Identifies vector divides eligible for FP-divider lane group splitting.
///
/// \p Classes maps canonical slice keys to unique integer class identifiers.
/// \p RegionNo ensures distinct regions do not share slice class identifiers.
static SmallVector<Group, 4> formGroups(BasicBlock &BB, Loop *L,
                                        StringMap<unsigned> &Classes,
                                        unsigned RegionNo, unsigned LaneGroup,
                                        const PortDescription *PD) {
  RegionGraph RG = buildRegionGraph(BB, L);
  const unsigned NumInsts = RG.Insts.size();

  //===--------------------------------------------------------------------===//
  // Phase 1: Filter In-Scope Candidate Instructions
  //===--------------------------------------------------------------------===//
  // Scalar candidates move to vector lane 0 (ToVector); vector candidates move
  // to scalar ports (ToScalar). A candidate is in scope only if it does not feed
  // a memory address calculation and the target CPU port-info provides all
  // required execution and transfer costs.
  SmallVector<bool, 32> InScope(NumInsts, false);
  SmallVector<bool, 32> IsVectorOp(NumInsts, false);
  for (unsigned NodeIdx = 0; NodeIdx != NumInsts; ++NodeIdx) {
    const Instruction &Inst = *RG.Insts[NodeIdx];
    IsVectorOp[NodeIdx] = isVectorCandidate(Inst);
    InScope[NodeIdx] =
        isCandidate(Inst) && !reachesAddress(Inst, L, &BB) &&
        isAvailable(PD,
                    IsVectorOp[NodeIdx] ? Transform::Scalarize
                                        : Transform::SingletonVectorize,
                    Inst);
  }

  // An integer division (UDiv) and remainder (URem) with identical operands in
  // the same basic block are fused into a single scalar divl instruction on x86
  // (which simultaneously writes quotient to EAX and remainder to EDX).
  // If one of them reaches an address (requiring scalar GPR execution), vectorizing
  // the other alone breaks hardware fusion, forcing separate scalar division +
  // vector FP division + domain transfers. Keep both in scalar when one reaches address.
  for (unsigned NodeIdx = 0; NodeIdx != NumInsts; ++NodeIdx) {
    if (!InScope[NodeIdx] || IsVectorOp[NodeIdx])
      continue;
    const Instruction &Inst = *RG.Insts[NodeIdx];
    if (Inst.getOpcode() != Instruction::UDiv &&
        Inst.getOpcode() != Instruction::URem)
      continue;
    unsigned SiblingOp = (Inst.getOpcode() == Instruction::UDiv)
                             ? Instruction::URem
                             : Instruction::UDiv;
    for (unsigned SiblingIdx = 0; SiblingIdx != NumInsts; ++SiblingIdx) {
      if (SiblingIdx == NodeIdx)
        continue;
      const Instruction &Sib = *RG.Insts[SiblingIdx];
      if (Sib.getOpcode() == SiblingOp &&
          Sib.getOperand(0) == Inst.getOperand(0) &&
          Sib.getOperand(1) == Inst.getOperand(1) &&
          reachesAddress(Sib, L, &BB)) {
        InScope[NodeIdx] = false;
        break;
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // Phase 2: Compute SCCs for Recurrence Detection
  //===--------------------------------------------------------------------===//
  // Find strongly connected components in the dependence graph to detect cycles.
  // A cycle represents a loop-carried recurrence.
  SmallVector<bool, 8> CompHasCycle;
  SmallVector<unsigned, 32> NodeToComp = computeSCCs(RG, CompHasCycle);
  SmallVector<SmallVector<unsigned, 4>, 8> CompMembers(CompHasCycle.size());
  for (unsigned NodeIdx = 0; NodeIdx != NumInsts; ++NodeIdx)
    CompMembers[NodeToComp[NodeIdx]].push_back(NodeIdx);
  // Conservatively pin bit-count operations on loop-carried chains to scalar
  // execution (chains with bit counts stay scalar; independent bit counts move).
  for (unsigned NodeIdx = 0; NodeIdx != NumInsts; ++NodeIdx)
    if (CompHasCycle[NodeToComp[NodeIdx]] &&
        !bitCountName(*RG.Insts[NodeIdx]).empty())
      InScope[NodeIdx] = false;

  // Identify whole all-in-scope recurrence groups.
  const unsigned NoGroup = ~0u;
  SmallVector<unsigned, 32> RecurrenceGroupId(NumInsts, NoGroup);
  SmallVector<bool, 32> IsRecurrenceNode(NumInsts, false);
  unsigned NumRecurrences = 0;
  SmallVector<unsigned, 8> RecOfComp(CompHasCycle.size(), NoGroup);

  for (unsigned CompIdx = 0; CompIdx != CompHasCycle.size(); ++CompIdx) {
    if (!CompHasCycle[CompIdx])
      continue;

    // A recurrence cycle can be promoted as a group only if every instruction
    // in the cycle is an eligible in-scope scalar candidate.
    bool AllInScope = all_of(CompMembers[CompIdx], [&](unsigned NodeIdx) {
      return InScope[NodeIdx] && !IsVectorOp[NodeIdx];
    });

    for (unsigned NodeIdx : CompMembers[CompIdx]) {
      IsRecurrenceNode[NodeIdx] = true;
      if (AllInScope) {
        if (RecOfComp[CompIdx] == NoGroup)
          RecOfComp[CompIdx] = NumRecurrences++;
        RecurrenceGroupId[NodeIdx] = RecOfComp[CompIdx];
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // Phase 3: Cluster Acyclic Candidates via DisjointSets (Union-Find)
  //===--------------------------------------------------------------------===//
  // Acyclic candidate instructions are clustered into connected slices along
  // def-use edges where both producer and consumer share the same direction.
  DisjointSets AcyclicDS;
  SmallVector<unsigned, 32> NodeToDSIndex(NumInsts, NoGroup);
  for (unsigned NodeIdx = 0; NodeIdx != NumInsts; ++NodeIdx)
    if (InScope[NodeIdx] && !IsRecurrenceNode[NodeIdx])
      NodeToDSIndex[NodeIdx] = AcyclicDS.add();

  for (unsigned NodeIdx = 0; NodeIdx != NumInsts; ++NodeIdx) {
    if (NodeToDSIndex[NodeIdx] != NoGroup) {
      for (unsigned PredIdx : RG.Preds[NodeIdx]) {
        if (NodeToDSIndex[PredIdx] != NoGroup &&
            IsVectorOp[PredIdx] == IsVectorOp[NodeIdx])
          AcyclicDS.unite(NodeToDSIndex[NodeIdx], NodeToDSIndex[PredIdx]);
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // Phase 4: Construct Group Objects
  //===--------------------------------------------------------------------===//
  SmallVector<Group, 4> Groups;
  DenseMap<unsigned, unsigned> GroupIndexByKey; // Even key: recurrence; Odd key: acyclic
  DenseMap<const Instruction *, unsigned> InstToGroupIndex;

  for (unsigned NodeIdx = 0; NodeIdx != NumInsts; ++NodeIdx) {
    unsigned Key = 0;
    if (IsRecurrenceNode[NodeIdx] && RecurrenceGroupId[NodeIdx] != NoGroup)
      Key = 2 * RecurrenceGroupId[NodeIdx];
    else if (NodeToDSIndex[NodeIdx] != NoGroup)
      Key = 2 * AcyclicDS.find(NodeToDSIndex[NodeIdx]) + 1;
    else
      continue;

    auto [It, Inserted] = GroupIndexByKey.try_emplace(Key, Groups.size());
    if (Inserted) {
      Groups.emplace_back();
      Groups.back().Recurrence = ((Key & 1) == 0);
      Groups.back().Dir =
          IsVectorOp[NodeIdx] ? Direction::ToScalar : Direction::ToVector;
    }
    unsigned GrpIdx = It->second;
    Groups[GrpIdx].Ops.push_back(const_cast<Instruction *>(RG.Insts[NodeIdx]));
    InstToGroupIndex[RG.Insts[NodeIdx]] = GrpIdx;
  }

  //===--------------------------------------------------------------------===//
  // Phase 5: Associate Carried PHI Nodes
  //===--------------------------------------------------------------------===//
  // In a recurrence group, loop PHIs closing the loop-carried cycle are kept in
  // vector form. If a carried PHI has users outside the group, we conservatively
  // discard the recurrence group to avoid complex off-cycle scalar reads.
  SmallVector<bool, 4> DiscardGroup(Groups.size(), false);
  if (L && L->isInnermost() && L->getNumBlocks() == 1) {
    for (PHINode &Phi : BB.phis()) {
      auto LatchIt = RG.PhiLatch.find(&Phi);
      if ((!Phi.getType()->isIntegerTy(32) &&
           !Phi.getType()->isIntegerTy(64)) ||
          Phi.use_empty() || LatchIt == RG.PhiLatch.end())
        continue;

      auto GroupIt = InstToGroupIndex.find(RG.Insts[LatchIt->second.first]);
      if (GroupIt == InstToGroupIndex.end() ||
          !Groups[GroupIt->second].Recurrence)
        continue;

      unsigned GrpIdx = GroupIt->second;
      auto IsUserInThisGroup = [&](User *U) {
        auto It = InstToGroupIndex.find(dyn_cast<Instruction>(U));
        return It != InstToGroupIndex.end() && It->second == GrpIdx;
      };

      if (all_of(Phi.users(), IsUserInThisGroup)) {
        // PHI-of-PHI chains (iteration distance > 1) are not joined into vector form.
        if (isa<PHINode>(Phi.getIncomingValueForBlock(&BB)))
          DiscardGroup[GrpIdx] = true;
        else
          Groups[GrpIdx].CarriedPhis.push_back(&Phi);
      } else if (any_of(Phi.users(), IsUserInThisGroup)) {
        DiscardGroup[GrpIdx] = true;
      }
    }

    // Discard recurrence groups whose instructions only serve as loop control
    // counters (i.e. their only external uses feed branch comparisons).
    for (unsigned GrpIdx = 0; GrpIdx != Groups.size(); ++GrpIdx) {
      if (!Groups[GrpIdx].Recurrence || DiscardGroup[GrpIdx])
        continue;
      if (all_of(Groups[GrpIdx].Ops, [&](Instruction *I) {
            return onlyFeedsLoopControl(I, Groups[GrpIdx].Ops,
                                        Groups[GrpIdx].CarriedPhis);
          }))
        DiscardGroup[GrpIdx] = true;
    }
  }

  SmallVector<Group, 4> ValidGroups;
  for (unsigned GrpIdx = 0; GrpIdx != Groups.size(); ++GrpIdx)
    if (!DiscardGroup[GrpIdx])
      ValidGroups.push_back(std::move(Groups[GrpIdx]));
  Groups = std::move(ValidGroups);

  //===--------------------------------------------------------------------===//
  // Phase 6: Analyze Group Inputs, Live-Outs, Ownership, and Slice Classes
  //===--------------------------------------------------------------------===//
  for (Group &Grp : Groups) {
    if (Grp.VectorDivide)
      continue;
    auto InGroup = [&](const Value *V) {
      if (auto *I = dyn_cast<Instruction>(V))
        if (is_contained(Grp.Ops, I))
          return true;
      auto *P = dyn_cast<PHINode>(V);
      return P && is_contained(Grp.CarriedPhis, P);
    };
    for (Instruction *I : Grp.Ops) {
      for (unsigned K = 0, E = size(dataOperands(*I)); K != E; ++K) {
        Use &U = I->getOperandUse(K);
        if (!InGroup(U.get()))
          Grp.Inputs.push_back(&U);
      }
      if (any_of(I->users(), [&](User *U) { return !InGroup(U); }))
        Grp.LiveOuts.push_back(I);
    }
    if (Grp.Dir == Direction::ToScalar)
      computeOwnership(Grp, BB);
    std::string Key = sliceKey(Grp, L, RegionNo);
    if (Grp.Recurrence) // A whole recurrence is one instance: never shared.
      Key += "@" + std::to_string(Classes.size());
    Grp.SliceClass = Classes.try_emplace(Key, Classes.size()).first->second;
    LLVM_DEBUG({
      dbgs() << "savio: group of " << Grp.Ops.size() << " ops ("
             << (Grp.Dir == Direction::ToScalar ? "scalarize "
                                                : "")
             << (Grp.Recurrence ? "recurrence" : "slice") << ", class "
             << Grp.SliceClass << ") in ";
      BB.printAsOperand(dbgs(), false);
      dbgs() << "\n";
      for (LoadInst *LD : Grp.OwnedLoads) {
        dbgs() << "  load: ";
        LD->printAsOperand(dbgs(), false);
        dbgs() << "\n";
      }
      for (Instruction *I : Grp.LiveOuts) {
        dbgs() << "  liveout: ";
        I->printAsOperand(dbgs(), false);
        dbgs() << (Grp.Dir == Direction::ToScalar && !liveOutNeedsInsert(Grp, *I)
                       ? " (stored)"
                       : "")
               << "\n";
      }
      for (StoreInst *S : Grp.OwnedStores) {
        dbgs() << "  store: ";
        S->getPointerOperand()->printAsOperand(dbgs(), false);
        dbgs() << "\n";
      }
    });
  }

  //===--------------------------------------------------------------------===//
  // Phase 7: Form Vector Divide Lane Groups (FP Divider Lane Slicing)
  //===--------------------------------------------------------------------===//
  // The lane groups of each unsigned divide or remainder of <N x i32> by a
  // non-constant divisor (a constant divisor becomes a multiply: the integer
  // divider is not used and the fdiv form would lose), for vectors that fit
  // the divide width (64 * LaneGroup bits): N / g groups, lanes {0..g-1}
  // first, g = min(LaneGroup, N); instances of one slice class per opcode.
  for (const Instruction *CI : RG.Insts) {
    auto *I = const_cast<Instruction *>(CI);
    auto *VT = dyn_cast<FixedVectorType>(I->getType());
    if (!VT || !VT->getElementType()->isIntegerTy(32) ||
        (I->getOpcode() != Instruction::UDiv &&
         I->getOpcode() != Instruction::URem) ||
        isa<Constant>(I->getOperand(1)))
      continue;
    unsigned NumLanes = VT->getNumElements();
    unsigned Size = std::min(LaneGroup, NumLanes);
    if (NumLanes < 4 || NumLanes > 16 || !isPowerOf2_32(NumLanes) ||
        NumLanes * 32 > 64 * LaneGroup || NumLanes % Size ||
        !isAvailable(PD, Transform::FpDivideLanes, *I, Size))
      continue;
    std::string Key = (Twine(RegionNo) + "V:" + I->getOpcodeName() + " " +
                       typeKey(VT))
                          .str();
    unsigned Cls = Classes.try_emplace(Key, Classes.size()).first->second;
    for (unsigned Index = 0; Index != NumLanes / Size; ++Index) {
      Groups.emplace_back();
      Group &Grp = Groups.back();
      Grp.Ops.push_back(I);
      Grp.SliceClass = Cls;
      Grp.VectorDivide = true;
      Grp.LaneGroup = Index;
      Grp.GroupLanes = Size;
    }
  }
  return Groups;
}

/// Walks the computation of \p V inside \p L (through casts, arithmetic and
/// selects; a load is a leaf, its address is not followed) and records whether
/// it reads a loaded value and whether it reads a phi of the loop.
static void scanIndex(const Value *V, const Loop *L,
                      SmallPtrSetImpl<const Value *> &Seen, bool &HasLoad,
                      bool &HasPhi) {
  auto *I = dyn_cast<Instruction>(V);
  if (!I || !L->contains(I) || !Seen.insert(I).second)
    return;
  if (isa<LoadInst>(I)) {
    HasLoad = true;
    return;
  }
  if (isa<PHINode>(I)) {
    HasPhi = true;
    return;
  }
  for (const Value *Op : I->operands())
    scanIndex(Op, L, Seen, HasLoad, HasPhi);
}

/// True if the index \p V is a function of loaded data alone: it reads a loaded
/// value and no phi of the loop. An index that also reads a phi (the induction
/// variable, a carried offset such as j += x & 1) moves along with the loop and
/// stays near its previous value, so it is not a random access.
static bool isDataDependentIndex(const Value *V, const Loop *L) {
  SmallPtrSet<const Value *, 16> Seen;
  bool HasLoad = false, HasPhi = false;
  scanIndex(V, L, Seen, HasLoad, HasPhi);
  return HasLoad && !HasPhi;
}

/// Known gaps (a floor that is a lower bound in some loops and too high in
/// others): an index that reads any loop phi on any path (out[idx[i] + i],
/// idx[i] ^ i) is not charged; a load whose address is loop invariant but not
/// hoisted is charged as random; only one-index GEPs are classified and
/// pointer chasing is not charged; each charge includes the stream cost of its
/// probe loop and is added per access, so a loop with several data-dependent
/// accesses is over-charged.
///
/// Throughput floor in cycles per iteration of the loop of \p BB from its
/// data-dependent accesses: a load or store through a one-index GEP whose index
/// is computed from loaded data and may reach beyond the first-level cache
/// (the index range known from its bits, times the element size).
static double indirectAccessFloor(const BasicBlock &BB, const Loop *L,
                                  const PortDescription &PD) {
  const IndirectAccessCost *IA = PD.indirectAccess();
  if (!L || !IA)
    return 0;
  const DataLayout &DL = BB.getDataLayout();
  double Floor = 0;
  for (const Instruction &I : BB) {
    const Value *Ptr = nullptr;
    bool IsLoad = isa<LoadInst>(I);
    if (auto *LD = dyn_cast<LoadInst>(&I))
      Ptr = LD->getPointerOperand();
    else if (auto *ST = dyn_cast<StoreInst>(&I))
      Ptr = ST->getPointerOperand();
    else
      continue;
    auto *GEP = dyn_cast<GEPOperator>(Ptr);
    if (!GEP || GEP->getNumOperands() != 2 ||
        !isDataDependentIndex(GEP->getOperand(1), L))
      continue;
    uint64_t Elt = DL.getTypeAllocSize(GEP->getSourceElementType());
    if (!Elt) // Zero-sized element ([0 x T]): no footprint to bound; not charged.
      continue;
    APInt MaxIdx = computeKnownBits(GEP->getOperand(1), DL).getMaxValue();
    // Footprint = (max index + 1) * element size, saturating.
    bool Resident = false;
    if (MaxIdx.getActiveBits() <= 63) {
      uint64_t N = MaxIdx.getZExtValue();
      Resident = N < IA->ResidentBytes / Elt;
    }
    if (Resident)
      continue;
    Floor += IsLoad ? IA->LoadCycles : IA->StoreCycles;
  }
  return Floor;
}

SmallVector<Region, 4> savio::formRegions(Function &F, LoopInfo &LI,
                                          unsigned LaneGroup,
                                          const PortDescription *PD) {
  SmallVector<Region, 4> Regions;
  StringMap<unsigned> Classes;
  unsigned RegionNo = 0;
  for (BasicBlock &BB : F) {
    Loop *L = LI.getLoopFor(&BB);
    // When multi-block loops are disabled, restrict to innermost single-block loops.
    if (L && !EnableMultiBlockLoops && !(L->isInnermost() && L->getNumBlocks() == 1))
      continue;
    SmallVector<Group, 4> Groups = formGroups(BB, L, Classes, RegionNo++, LaneGroup, PD);
    if (!Groups.empty())
      Regions.push_back(
          {&BB, L, std::move(Groups),
           PD ? indirectAccessFloor(BB, L, *PD) : 0.0});
  }
  return Regions;
}

InputKind savio::classifyInput(const Loop *L, const Group &G,
                               const Value *V) {
  if (isa<Constant>(V))
    return InputKind::Free;
  // A load whose users are all in the group is replaced by a vector load.
  if (auto *LD = dyn_cast<LoadInst>(V))
    if (LD->isSimple() &&
        all_of(LD->users(), [&](const User *U) {
          return is_contained(G.Ops, dyn_cast<Instruction>(U));
        }))
      return InputKind::Free;
  // In a loop the transfer of an invariant value is hoisted to the
  // preheader; without one it stays in the loop.
  if (L && L->getLoopPreheader() && L->isLoopInvariant(V))
    return InputKind::Free;
  // A constant in-range lane of a fixed vector moves to lane 0 with a shuffle.
  if (auto *EE = dyn_cast<ExtractElementInst>(V))
    if (auto *VT = dyn_cast<FixedVectorType>(EE->getVectorOperand()->getType()))
      if (auto *Idx = dyn_cast<ConstantInt>(EE->getIndexOperand()))
        if (Idx->getValue().ult(VT->getNumElements())) {
          if (Idx->isZero() &&
              VT->getNumElements() == (EE->getType()->isIntegerTy(64) ? 2 : 4))
            return InputKind::Free;
          return InputKind::LaneToLane0;
        }
  return InputKind::GprToVec;
}

bool savio::isInGroup(const Group &G, const User *U) {
  if (auto *I = dyn_cast<Instruction>(U))
    if (is_contained(G.Ops, I))
      return true;
  auto *P = dyn_cast<PHINode>(U);
  return P && is_contained(G.CarriedPhis, P);
}

bool savio::liveOutNeedsGpr(const Group &G, const Instruction &I) {
  return !all_of(I.users(), [&](const User *U) {
    return isInGroup(G, U) || isSimpleStoreOf(U, &I);
  });
}

RegionGraph savio::buildRegionGraph(BasicBlock &BB, Loop *L) {
  RegionGraph RG;
  bool IsSingleBlockLoop = L && L->isInnermost() && L->getNumBlocks() == 1;

  // Non-PHI instructions form graph nodes in program order, which guarantees a
  // topological ordering for intra-iteration dataflow edges. Outside loops or in
  // multi-block loop blocks, the terminator is omitted; in single-block loops,
  // the loop-control branch is included.
  for (Instruction &I : BB)
    if (!isa<PHINode>(I) && (IsSingleBlockLoop || !I.isTerminator())) {
      RG.Index[&I] = RG.Insts.size();
      RG.Insts.push_back(&I);
    }
  RG.Preds.resize(RG.Insts.size());
  RG.Carried.resize(RG.Insts.size());

  // In single-block loop regions, trace header PHIs back to their latch definition in this
  // block, following PHI-of-PHI chains (e.g. multiple iteration delays) to
  // compute the producer node and carried iteration distance.
  if (IsSingleBlockLoop) {
    for (PHINode &Phi : BB.phis()) {
      const PHINode *CurrPhi = &Phi;
      unsigned IterationDistance = 0;
      for (unsigned Step = 0; Step <= BB.size(); ++Step) {
        int IncomingIdx = CurrPhi->getBasicBlockIndex(&BB);
        if (IncomingIdx < 0)
          break;
        ++IterationDistance;
        const Value *IncomingVal = CurrPhi->getIncomingValue(IncomingIdx);
        if (auto *NextPhi = dyn_cast<PHINode>(IncomingVal)) {
          if (NextPhi->getParent() != &BB)
            break;
          CurrPhi = NextPhi;
          continue;
        }
        auto IndexIt = isa<Instruction>(IncomingVal)
                           ? RG.Index.find(cast<Instruction>(IncomingVal))
                           : RG.Index.end();
        if (IndexIt != RG.Index.end())
          RG.PhiLatch[&Phi] = {IndexIt->second, IterationDistance};
        break;
      }
    }
  }

  // Populate intra-iteration predecessors and loop-carried dependencies.
  for (unsigned ConsumerIdx = 0; ConsumerIdx != RG.Insts.size(); ++ConsumerIdx) {
    for (const Use &OpUse : RG.Insts[ConsumerIdx]->operands()) {
      auto *ProducerInst = dyn_cast<Instruction>(OpUse.get());
      if (!ProducerInst)
        continue;
      if (auto *Phi = dyn_cast<PHINode>(ProducerInst)) {
        auto LatchIt = RG.PhiLatch.find(Phi);
        if (LatchIt != RG.PhiLatch.end())
          RG.Carried[ConsumerIdx].push_back(LatchIt->second);
        continue;
      }
      auto IndexIt = RG.Index.find(ProducerInst);
      if (IndexIt != RG.Index.end())
        RG.Preds[ConsumerIdx].push_back(IndexIt->second);
    }
  }
  return RG;
}
