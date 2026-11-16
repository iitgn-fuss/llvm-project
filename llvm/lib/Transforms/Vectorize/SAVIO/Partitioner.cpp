//===- Partitioner.cpp - SAVIO groups and the greedy split search ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <string>
#include <utility>

using namespace llvm;
using namespace llvm::savio;

#define DEBUG_TYPE "savio"

cl::opt<double> llvm::SAVIOMargin(
    "savio-margin", cl::init(0.05), cl::Hidden,
    cl::desc("Relative reduction of T a SAVIO step must reach to be taken"));

cl::opt<bool> llvm::SAVIOForce(
    "savio-force", cl::init(false), cl::Hidden,
    cl::desc("Singleton Vectorize every candidate group regardless of cost "
             "(for correctness tests only)"));

/// Operations in scope: i32 add sub mul and or xor shl lshr ashr udiv urem.
static bool isCandidate(const Instruction &I) {
  if (!I.getType()->isIntegerTy(32))
    return false;
  switch (I.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::UDiv:
  case Instruction::URem:
    return true;
  default:
    return false;
  }
}

static bool isSimpleStoreOf(const User *U, const Value *V) {
  auto *S = dyn_cast<StoreInst>(U);
  return S && S->isSimple() && S->getValueOperand() == V &&
         S->getPointerOperand() != V;
}

/// True if the value of \p I reaches an address (a GEP operand or an inttoptr)
/// through casts, phis and other in-scope operations: addresses must stay in GPRs, so such an operation
/// is not a candidate.
static bool reachesAddress(const Instruction &I) {
  SmallVector<const Instruction *, 8> Work{&I};
  SmallPtrSet<const Instruction *, 8> Seen{&I};
  while (!Work.empty()) {
    const Instruction *Cur = Work.pop_back_val();
    for (const User *U : Cur->users()) {
      if (isa<GetElementPtrInst>(U) || isa<IntToPtrInst>(U))
        return true;
      if (isa<SExtInst>(U) || isa<ZExtInst>(U) || isa<TruncInst>(U) ||
          isa<PHINode>(U) || isCandidate(*cast<Instruction>(U))) {
        auto *UI = cast<Instruction>(U);
        if (Seen.insert(UI).second)
          Work.push_back(UI);
      }
    }
  }
  return false;
}

namespace {

/// Union-find over indices.
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

} // namespace

/// The groups of one region. Loop phis are returned through \p Group's
/// CarriedPhis.
static SmallVector<Group, 4> formGroups(BasicBlock &BB, Loop *L) {
  SmallVector<Instruction *, 16> Cands;
  DenseMap<const Instruction *, unsigned> Index;
  DisjointSets DS;
  for (Instruction &I : BB)
    if (isCandidate(I) && !reachesAddress(I)) {
      Index[&I] = DS.add();
      Cands.push_back(&I);
    }

  // Connect operations by def-use.
  for (Instruction *I : Cands)
    for (Value *Op : I->operands())
      if (auto *OpI = dyn_cast<Instruction>(Op)) {
        auto It = Index.find(OpI);
        if (It != Index.end())
          DS.unite(Index[I], It->second);
      }

  // One group per set, in order of first appearance.
  SmallVector<Group, 4> Groups;
  DenseMap<unsigned, unsigned> GroupOfRoot;
  DenseMap<const Instruction *, unsigned> GroupOf;
  for (Instruction *I : Cands) {
    unsigned Root = DS.find(Index[I]);
    auto [It, New] = GroupOfRoot.try_emplace(Root, Groups.size());
    if (New)
      Groups.emplace_back();
    Groups[It->second].Ops.push_back(I);
    GroupOf[I] = It->second;
  }

  // A loop phi joins a group when its latch value and all its users are in
  // that group, so the value stays in vector form around the loop.
  DenseSet<const PHINode *> Joined;
  if (L)
    for (PHINode &Phi : BB.phis()) {
      if (!Phi.getType()->isIntegerTy(32) || Phi.use_empty())
        continue;
      auto *Latch = dyn_cast<Instruction>(Phi.getIncomingValueForBlock(&BB));
      auto LIt = Latch ? GroupOf.find(Latch) : GroupOf.end();
      if (LIt == GroupOf.end())
        continue;
      bool AllInGroup = all_of(Phi.users(), [&](User *U) {
        auto It = GroupOf.find(dyn_cast<Instruction>(U));
        return It != GroupOf.end() && It->second == LIt->second;
      });
      if (AllInGroup) {
        Groups[LIt->second].CarriedPhis.push_back(&Phi);
        Joined.insert(&Phi);
      }
    }

  for (unsigned G = 0; G != Groups.size(); ++G) {
    Group &Grp = Groups[G];
    auto InGroup = [&](const Value *V) {
      if (auto *I = dyn_cast<Instruction>(V)) {
        auto It = GroupOf.find(I);
        if (It != GroupOf.end() && It->second == G)
          return true;
      }
      auto *P = dyn_cast<PHINode>(V);
      return P && is_contained(Grp.CarriedPhis, P);
    };
    for (Instruction *I : Grp.Ops) {
      for (Use &U : I->operands())
        if (!InGroup(U.get()))
          Grp.Inputs.push_back(&U);
      if (any_of(I->users(), [&](User *U) { return !InGroup(U); }))
        Grp.LiveOuts.push_back(I);
    }
    LLVM_DEBUG({
      dbgs() << "savio: group of " << Grp.Ops.size() << " ops in ";
      BB.printAsOperand(dbgs(), false);
      dbgs() << "\n";
      for (Instruction *I : Grp.LiveOuts) {
        dbgs() << "  liveout: ";
        I->printAsOperand(dbgs(), false);
        dbgs() << "\n";
      }
    });
  }
  return Groups;
}

SmallVector<Region, 4> savio::formRegions(Function &F, LoopInfo &LI) {
  SmallVector<Region, 4> Regions;
  for (BasicBlock &BB : F) {
    Loop *L = LI.getLoopFor(&BB);
    // v1: innermost single-block loops, and blocks outside any loop.
    if (L && !(L->isInnermost() && L->getNumBlocks() == 1))
      continue;
    SmallVector<Group, 4> Groups = formGroups(BB, L);
    if (!Groups.empty())
      Regions.push_back({&BB, L, std::move(Groups)});
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
  // In a loop the transfer of an invariant value is hoisted out of it.
  if (L && L->isLoopInvariant(V))
    return InputKind::Free;
  if (isa<ExtractElementInst>(V))
    return InputKind::LaneToLane0;
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

namespace {

/// Cost model nodes of a region for one assignment.
///
/// Only the in-scope operations of the region's groups (and the transfers a
/// Singleton Vectorized group needs) are costed. Everything else in the block
/// (loads, stores, address arithmetic, calls, other types) is not costed in
/// v1, so T is an estimate of the contested resources, not of the whole block.
class NodeBuilder {
  const Region &R;
  const PortDescription &PD;
  ArrayRef<bool> Promoted;

  SmallVector<CostNode, 32> Nodes;
  DenseMap<const Instruction *, unsigned> NodeOf, OutNode;
  DenseMap<const Instruction *, unsigned> GroupOf;
  DenseMap<std::pair<unsigned, const Value *>, unsigned> InNode;
  /// A loop-carried edge to add once all nodes exist: the value of Latch one
  /// iteration earlier reaches node Dest. Through a phi kept in vector form
  /// (Joined) it stays in the vector domain; otherwise it is the scalar value,
  /// i.e. the VecToGpr node if the latch operation has one.
  struct CarriedEdge {
    unsigned Dest;
    const Instruction *Latch;
    bool Joined;
  };
  SmallVector<CarriedEdge, 4> Pending;

  bool addTransfer(TransferKind K, ArrayRef<unsigned> Preds, unsigned &Out) {
    std::optional<OpCost> C = PD.transferCost(K);
    if (!C)
      return false;
    CostNode N;
    N.Cost = *C;
    N.Preds.assign(Preds.begin(), Preds.end());
    Out = Nodes.size();
    Nodes.push_back(std::move(N));
    return true;
  }

public:
  NodeBuilder(const Region &R, const PortDescription &PD,
              ArrayRef<bool> Promoted)
      : R(R), PD(PD), Promoted(Promoted) {
    for (unsigned G = 0; G != R.Groups.size(); ++G)
      for (Instruction *I : R.Groups[G].Ops)
        GroupOf[I] = G;
  }

  /// Fills the nodes; false if the assignment needs a cost the port
  /// description lacks.
  bool build() {
    for (Instruction &I : *R.BB) {
      auto GIt = GroupOf.find(&I);
      if (GIt == GroupOf.end())
        continue;
      unsigned GIdx = GIt->second;
      const Group &G = R.Groups[GIdx];
      bool P = Promoted[GIdx];

      CostNode N;
      // Carried edges whose destination is this operation, resolved below.
      SmallVector<CarriedEdge, 2> OwnEdges;
      for (Use &U : I.operands()) {
        Value *V = U.get();
        if (auto *OpI = dyn_cast<Instruction>(V)) {
          auto It = NodeOf.find(OpI);
          if (It != NodeOf.end()) {
            N.Preds.push_back(It->second);
            continue;
          }
        }
        // A loop phi whose latch value is an in-scope operation closes a
        // recurrence through this operand.
        const Instruction *Latch = nullptr;
        bool Joined = false;
        if (auto *Phi = dyn_cast<PHINode>(V))
          if (R.L && Phi->getParent() == R.BB) {
            auto *L = dyn_cast<Instruction>(Phi->getIncomingValueForBlock(R.BB));
            if (L && GroupOf.count(L))
              Latch = L;
            Joined = is_contained(G.CarriedPhis, Phi);
          }
        if (Joined) {
          OwnEdges.push_back({0, Latch, true});
          continue;
        }
        if (P) {
          InputKind K = classifyInput(R.L, G, V);
          if (K != InputKind::Free) {
            auto Key = std::make_pair(GIdx, (const Value *)V);
            auto It = InNode.find(Key);
            unsigned T;
            if (It != InNode.end()) {
              T = It->second;
            } else {
              if (!addTransfer(K == InputKind::GprToVec
                                   ? TransferKind::GprToVec
                                   : TransferKind::LaneToLane0,
                               {}, T))
                return false;
              InNode[Key] = T;
              if (Latch)
                Pending.push_back({T, Latch, false});
            }
            N.Preds.push_back(T);
            continue;
          }
        }
        if (Latch)
          OwnEdges.push_back({0, Latch, false});
      }

      std::string Key;
      if (P) {
        bool Imm = isa<ConstantInt>(I.getOperand(1)) &&
                   (I.getOpcode() == Instruction::Shl ||
                    I.getOpcode() == Instruction::LShr ||
                    I.getOpcode() == Instruction::AShr);
        Key = ("sv." + Twine(I.getOpcodeName()) +
               (Imm ? ".imm v4i32" : " v4i32"))
                  .str();
      } else {
        Key = (Twine(I.getOpcodeName()) + " i32").str();
      }
      std::optional<OpCost> C = PD.irOpCost(Key);
      if (!C) {
        if (P)
          return false;
        // A scalar form without a cost is not charged.
        C = OpCost{{}, 0, 0};
      }
      N.Cost = *C;
      unsigned Me = Nodes.size();
      Nodes.push_back(std::move(N));
      NodeOf[&I] = Me;
      for (CarriedEdge &E : OwnEdges) {
        E.Dest = Me;
        Pending.push_back(E);
      }

      // Live-out of a Singleton Vectorized group: back to a GPR unless every
      // user outside the group is a store, which can store from the vector.
      if (P && is_contained(G.LiveOuts, &I)) {
        if (liveOutNeedsGpr(G, I)) {
          unsigned T;
          if (!addTransfer(TransferKind::VecToGpr, {Me}, T))
            return false;
          OutNode[&I] = T;
        }
      }
    }

    for (const CarriedEdge &E : Pending) {
      unsigned Src = NodeOf[E.Latch];
      if (!E.Joined) {
        auto It = OutNode.find(E.Latch);
        if (It != OutNode.end())
          Src = It->second;
      }
      Nodes[E.Dest].Carried.push_back({Src, 1});
    }
    return true;
  }

  ArrayRef<CostNode> nodes() const { return Nodes; }
};

} // namespace

Decision savio::decide(const Region &R, const PortDescription &PD,
                       double Margin) {
  unsigned N = R.Groups.size();
  SmallVector<bool, 8> Promoted(N, false);
  bool IsLoop = R.L != nullptr;

  // Estimate of the current assignment; nullopt if it needs a missing cost.
  auto Estimate = [&]() -> std::optional<RegionEstimate> {
    NodeBuilder B(R, PD, Promoted);
    if (!B.build())
      return std::nullopt;
    return estimateRegion(B.nodes(), PD, IsLoop);
  };

  Decision D;
  D.Before = *Estimate();
  D.After = D.Before;

  if (SAVIOForce) {
    for (unsigned G = 0; G != N; ++G) {
      Promoted[G] = true;
      if (!Estimate())
        Promoted[G] = false;
      else
        D.Promoted.push_back(G);
    }
    D.After = *Estimate();
    return D;
  }

  // Greedy: while the resource bound limits T, take the move that lowers T
  // the most, if it lowers it by at least the margin.
  while (D.After.limiting() == Bound::Resource) {
    std::optional<RegionEstimate> Best;
    unsigned BestG = 0;
    for (unsigned G = 0; G != N; ++G) {
      if (Promoted[G])
        continue;
      Promoted[G] = true;
      std::optional<RegionEstimate> E = Estimate();
      Promoted[G] = false;
      if (E && (!Best || E->t() < Best->t())) {
        Best = E;
        BestG = G;
      }
    }
    if (!Best)
      break;
    double Gain = D.After.t() - Best->t();
    if (Gain <= 0 || Gain < Margin * D.After.t())
      break;
    Promoted[BestG] = true;
    D.Promoted.push_back(BestG);
    D.After = *Best;
  }
  return D;
}
