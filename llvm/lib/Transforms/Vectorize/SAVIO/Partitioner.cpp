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
#include "llvm/ADT/StringMap.h"
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

/// Strongly connected components of the region graph (same-iteration and
/// carried edges), by iterative Tarjan. Returns the component id of each node;
/// ids are dense. \p HasCycle[c] is true for components that are cycles (more
/// than one node, or a node with a carried edge to itself).
static SmallVector<unsigned, 32> computeSCCs(const RegionGraph &RG,
                                             SmallVectorImpl<bool> &HasCycle) {
  unsigned N = RG.Insts.size();
  SmallVector<SmallVector<unsigned, 2>, 32> Succs(N);
  for (unsigned U = 0; U != N; ++U) {
    for (unsigned P : RG.Preds[U])
      Succs[P].push_back(U);
    for (auto &E : RG.Carried[U])
      Succs[E.first].push_back(U);
  }
  const unsigned None = ~0u;
  SmallVector<unsigned, 32> Index(N, None), Low(N, 0), Comp(N, None);
  SmallVector<bool, 32> OnStack(N, false);
  SmallVector<unsigned, 32> Stack;
  SmallVector<std::pair<unsigned, unsigned>, 32> Work; // (node, next succ)
  unsigned Counter = 0, NumComps = 0;
  for (unsigned Root = 0; Root != N; ++Root) {
    if (Index[Root] != None)
      continue;
    Work.push_back({Root, 0});
    Index[Root] = Low[Root] = Counter++;
    Stack.push_back(Root);
    OnStack[Root] = true;
    while (!Work.empty()) {
      auto &[V, Next] = Work.back();
      if (Next < Succs[V].size()) {
        unsigned W = Succs[V][Next++];
        if (Index[W] == None) {
          Index[W] = Low[W] = Counter++;
          Stack.push_back(W);
          OnStack[W] = true;
          Work.push_back({W, 0});
        } else if (OnStack[W]) {
          Low[V] = std::min(Low[V], Index[W]);
        }
        continue;
      }
      unsigned Vn = V;
      Work.pop_back();
      if (!Work.empty())
        Low[Work.back().first] = std::min(Low[Work.back().first], Low[Vn]);
      if (Low[Vn] == Index[Vn]) {
        unsigned Size = 0, X;
        do {
          X = Stack.pop_back_val();
          OnStack[X] = false;
          Comp[X] = NumComps;
          ++Size;
        } while (X != Vn);
        bool Cycle = Size > 1;
        if (!Cycle)
          for (auto &E : RG.Carried[Vn])
            Cycle |= E.first == Vn;
        HasCycle.push_back(Cycle);
        ++NumComps;
      }
    }
  }
  return Comp;
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
  for (const Instruction *I : G.Ops) {
    OS << I->getOpcodeName() << '(';
    bool First = true;
    for (const Value *V : I->operands()) {
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
        OS << "load";
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
    OS << Pos[I] << ',';
  return Key;
}

/// The groups of one region: every whole all-in-scope recurrence (with its
/// phis, kept in vector form), and the use-def connected slices of the
/// candidates outside recurrences. Operations of a recurrence that has a member
/// out of scope are not candidates, so no transfer lands on a cycle.
/// \p Classes numbers the slice classes of the function; \p RegionNo keeps the
/// classes of different regions apart.
static SmallVector<Group, 4> formGroups(BasicBlock &BB, Loop *L,
                                        StringMap<unsigned> &Classes,
                                        unsigned RegionNo) {
  RegionGraph RG = buildRegionGraph(BB, L);
  unsigned N = RG.Insts.size();
  SmallVector<bool, 32> InScope(N, false);
  for (unsigned I = 0; I != N; ++I)
    InScope[I] = isCandidate(*RG.Insts[I]) && !reachesAddress(*RG.Insts[I]);

  SmallVector<bool, 8> CompCycle;
  SmallVector<unsigned, 32> Comp = computeSCCs(RG, CompCycle);
  SmallVector<SmallVector<unsigned, 4>, 8> CompNodes(CompCycle.size());
  for (unsigned I = 0; I != N; ++I)
    CompNodes[Comp[I]].push_back(I);

  // Group number of each node, or none: a recurrence is one group, the other
  // candidates group by use-def.
  const unsigned None = ~0u;
  SmallVector<unsigned, 32> GroupOfNode(N, None);
  SmallVector<bool, 32> IsRec(N, false);
  unsigned NumRec = 0;
  SmallVector<unsigned, 8> RecOfComp(CompCycle.size(), None);
  for (unsigned C = 0; C != CompCycle.size(); ++C) {
    if (!CompCycle[C])
      continue;
    // Whether the whole recurrence is in scope.
    bool AllInScope = all_of(CompNodes[C], [&](unsigned I) { return InScope[I]; });
    for (unsigned I : CompNodes[C]) {
      IsRec[I] = true;
      if (AllInScope)
        GroupOfNode[I] = RecOfComp[C] != None ? RecOfComp[C]
                                              : (RecOfComp[C] = NumRec++);
    }
  }

  // The acyclic candidates, connected by def-use.
  DisjointSets DS;
  SmallVector<unsigned, 32> DSIndex(N, None);
  for (unsigned I = 0; I != N; ++I)
    if (InScope[I] && !IsRec[I])
      DSIndex[I] = DS.add();
  for (unsigned I = 0; I != N; ++I)
    if (DSIndex[I] != None)
      for (unsigned P : RG.Preds[I])
        if (DSIndex[P] != None)
          DS.unite(DSIndex[I], DSIndex[P]);

  // One group per recurrence or set, in order of first appearance.
  SmallVector<Group, 4> Groups;
  DenseMap<unsigned, unsigned> GroupOfKey; // 2*rec or 2*root+1
  DenseMap<const Instruction *, unsigned> GroupOf;
  for (unsigned I = 0; I != N; ++I) {
    unsigned Key;
    if (IsRec[I] && GroupOfNode[I] != None)
      Key = 2 * GroupOfNode[I];
    else if (DSIndex[I] != None)
      Key = 2 * DS.find(DSIndex[I]) + 1;
    else
      continue;
    auto [It, New] = GroupOfKey.try_emplace(Key, Groups.size());
    if (New) {
      Groups.emplace_back();
      Groups.back().Recurrence = (Key & 1) == 0;
    }
    Groups[It->second].Ops.push_back(const_cast<Instruction *>(RG.Insts[I]));
    GroupOf[RG.Insts[I]] = It->second;
  }

  // The phis of a recurrence stay in vector form: every phi that closes the
  // cycle joins its group. If such a phi is also read outside the group the
  // recurrence is not a group at all: a conservative v1 choice (a lane-0 read
  // of the vector phi would be off the cycle, but it is not implemented).
  SmallVector<bool, 4> Drop(Groups.size(), false);
  if (L)
    for (PHINode &Phi : BB.phis()) {
      auto LatchIt = RG.PhiLatch.find(&Phi);
      if (!Phi.getType()->isIntegerTy(32) || Phi.use_empty() ||
          LatchIt == RG.PhiLatch.end())
        continue;
      auto GIt = GroupOf.find(RG.Insts[LatchIt->second.first]);
      if (GIt == GroupOf.end() || !Groups[GIt->second].Recurrence)
        continue;
      unsigned G = GIt->second;
      auto InG = [&](User *U) {
        auto It = GroupOf.find(dyn_cast<Instruction>(U));
        return It != GroupOf.end() && It->second == G;
      };
      if (all_of(Phi.users(), InG)) {
        // A phi whose latch is a phi (distance above 1) is not joined.
        if (isa<PHINode>(Phi.getIncomingValueForBlock(&BB)))
          Drop[G] = true;
        else
          Groups[G].CarriedPhis.push_back(&Phi);
      } else if (any_of(Phi.users(), InG)) {
        Drop[G] = true;
      }
    }
  SmallVector<Group, 4> Kept;
  for (unsigned G = 0; G != Groups.size(); ++G)
    if (!Drop[G])
      Kept.push_back(std::move(Groups[G]));
  Groups = std::move(Kept);

  for (Group &Grp : Groups) {
    auto InGroup = [&](const Value *V) {
      if (auto *I = dyn_cast<Instruction>(V))
        if (is_contained(Grp.Ops, I))
          return true;
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
    Grp.SliceClass =
        Classes.try_emplace(sliceKey(Grp, L, RegionNo), Classes.size()).first->second;
    LLVM_DEBUG({
      dbgs() << "savio: group of " << Grp.Ops.size() << " ops ("
             << (Grp.Recurrence ? "recurrence" : "slice") << ", class "
             << Grp.SliceClass << ") in ";
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
  StringMap<unsigned> Classes;
  unsigned RegionNo = 0;
  for (BasicBlock &BB : F) {
    Loop *L = LI.getLoopFor(&BB);
    // v1: innermost single-block loops, and blocks outside any loop.
    if (L && !(L->isInnermost() && L->getNumBlocks() == 1))
      continue;
    SmallVector<Group, 4> Groups = formGroups(BB, L, Classes, RegionNo++);
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
  // In a loop the transfer of an invariant value is hoisted to the
  // preheader; without one it stays in the loop.
  if (L && L->getLoopPreheader() && L->isLoopInvariant(V))
    return InputKind::Free;
  // A constant in-range lane of a fixed vector moves to lane 0 with a shuffle.
  if (auto *EE = dyn_cast<ExtractElementInst>(V))
    if (auto *VT = dyn_cast<FixedVectorType>(EE->getVectorOperand()->getType()))
      if (auto *Idx = dyn_cast<ConstantInt>(EE->getIndexOperand()))
        if (Idx->getValue().ult(VT->getNumElements()))
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

RegionGraph savio::buildRegionGraph(BasicBlock &BB, Loop *L) {
  RegionGraph G;
  for (Instruction &I : BB)
    // Outside a loop the terminator is not a node; in a loop region the
    // terminator is the loop-control branch.
    if (!isa<PHINode>(I) && (L || !I.isTerminator())) {
      G.Index[&I] = G.Insts.size();
      G.Insts.push_back(&I);
    }
  G.Preds.resize(G.Insts.size());
  G.Carried.resize(G.Insts.size());
  if (L) {
    // The node and distance a header phi takes its value from, following a
    // phi whose latch value is another header phi.
    for (PHINode &Phi : BB.phis()) {
      const PHINode *Cur = &Phi;
      unsigned Dist = 0;
      for (unsigned Step = 0; Step <= BB.size(); ++Step) {
        int Idx = Cur->getBasicBlockIndex(&BB);
        if (Idx < 0)
          break;
        ++Dist;
        const Value *V = Cur->getIncomingValue(Idx);
        if (auto *P = dyn_cast<PHINode>(V)) {
          if (P->getParent() != &BB)
            break;
          Cur = P;
          continue;
        }
        auto It = isa<Instruction>(V) ? G.Index.find(cast<Instruction>(V))
                                      : G.Index.end();
        if (It != G.Index.end())
          G.PhiLatch[&Phi] = {It->second, Dist};
        break;
      }
    }
  }
  for (unsigned N = 0; N != G.Insts.size(); ++N)
    for (const Use &U : G.Insts[N]->operands()) {
      auto *OpI = dyn_cast<Instruction>(U.get());
      if (!OpI)
        continue;
      if (auto *Phi = dyn_cast<PHINode>(OpI)) {
        auto It = G.PhiLatch.find(Phi);
        if (It != G.PhiLatch.end())
          G.Carried[N].push_back(It->second);
        continue;
      }
      auto It = G.Index.find(OpI);
      if (It != G.Index.end())
        G.Preds[N].push_back(It->second);
    }
  return G;
}

namespace {

/// Cost model nodes of a region for one assignment, one node per instruction
/// of the region graph plus the transfers a Singleton Vectorized group needs.
///
/// In-scope operations of groups are priced from the port description (scalar
/// or lane-0 vector form); every other instruction (loads, stores, address
/// arithmetic, calls, compares, branches, other types) from the fallback, which
/// gives it latency and micro-ops but no resource use. So a dependence through
/// any instruction is kept, and T bounds the whole block.
class NodeBuilder {
  const Region &R;
  const RegionGraph &RG;
  const PortDescription &PD;
  FallbackCostFn Fallback;
  ArrayRef<bool> Promoted;

  SmallVector<CostNode, 32> Nodes;
  DenseMap<const Instruction *, unsigned> NodeOf, OutNode;
  DenseMap<const Instruction *, unsigned> GroupOf;
  DenseMap<std::pair<unsigned, const Value *>, unsigned> InNode;
  /// A loop-carried edge to add once all nodes exist: the value of Latch Dist
  /// iterations earlier reaches node Dest. Through a phi kept in vector form
  /// (Joined) it stays in the vector domain; otherwise it is the scalar value,
  /// i.e. the VecToGpr node if the latch operation has one.
  struct CarriedEdge {
    unsigned Dest;
    const Instruction *Latch;
    bool Joined;
    unsigned Dist;
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
  NodeBuilder(const Region &R, const RegionGraph &RG, const PortDescription &PD,
              FallbackCostFn Fallback, ArrayRef<bool> Promoted)
      : R(R), RG(RG), PD(PD), Fallback(Fallback), Promoted(Promoted) {
    for (unsigned G = 0; G != R.Groups.size(); ++G)
      for (Instruction *I : R.Groups[G].Ops)
        GroupOf[I] = G;
  }

  /// Fills the nodes; false if the assignment needs a cost the port
  /// description lacks.
  bool build() {
    for (unsigned NI = 0; NI != RG.Insts.size(); ++NI) {
      const Instruction &I = *RG.Insts[NI];
      // The region graph holds one edge per operand defined in the block, in
      // operand order: same-iteration in Preds, through a header phi in
      // Carried.
      unsigned PredCur = 0, CarriedCur = 0;
      auto GIt = GroupOf.find(&I);
      bool InGroup = GIt != GroupOf.end();
      unsigned GIdx = InGroup ? GIt->second : 0;
      bool P = InGroup && Promoted[GIdx];

      CostNode N;
      // Carried edges whose destination is this instruction, resolved below.
      SmallVector<CarriedEdge, 2> OwnEdges;
      for (const Use &U : I.operands()) {
        const Value *V = U.get();
        const Instruction *Latch = nullptr;
        unsigned Dist = 1;
        bool Joined = false;
        const Instruction *Def = nullptr; // Operand defined in the block.
        if (auto *Phi = dyn_cast<PHINode>(V)) {
          // A loop phi closes a recurrence through this operand when its latch
          // value is an instruction of the block: a carried edge.
          if (RG.PhiLatch.count(Phi)) {
            assert(CarriedCur < RG.Carried[NI].size() && "missing carried edge");
            auto [LatchNode, D] = RG.Carried[NI][CarriedCur++];
            Latch = RG.Insts[LatchNode];
            Dist = D;
            Joined = InGroup && is_contained(R.Groups[GIdx].CarriedPhis, Phi);
          }
        } else if (auto *OpI = dyn_cast<Instruction>(V)) {
          if (RG.Index.count(OpI)) {
            assert(PredCur < RG.Preds[NI].size() && "missing pred edge");
            Def = RG.Insts[RG.Preds[NI][PredCur++]];
            assert(Def == OpI && "region graph edge order differs");
          }
        }

        if (Joined) {
          OwnEdges.push_back({0, Latch, true, Dist});
          continue;
        }
        // The operand's value in this instruction's domain: another
        // instruction of the same group is read as is; anything else is the
        // scalar value, i.e. through the VecToGpr node of a live-out.
        bool SameGroup = Def && InGroup && GroupOf.count(Def) &&
                         GroupOf.find(Def)->second == GIdx;
        InputKind K = InputKind::Free;
        if (P && !SameGroup)
          K = classifyInput(R.L, R.Groups[GIdx], V);
        if (P && !SameGroup && K != InputKind::Free) {
          auto Key = std::make_pair(GIdx, V);
          auto It = InNode.find(Key);
          unsigned T;
          if (It != InNode.end()) {
            T = It->second;
          } else {
            SmallVector<unsigned, 1> TP;
            if (Def)
              TP.push_back(scalarNode(Def));
            if (!addTransfer(K == InputKind::GprToVec
                                 ? TransferKind::GprToVec
                                 : TransferKind::LaneToLane0,
                             TP, T))
              return false;
            InNode[Key] = T;
            if (Latch)
              Pending.push_back({T, Latch, false, Dist});
          }
          N.Preds.push_back(T);
          continue;
        }
        if (Def)
          N.Preds.push_back(SameGroup ? NodeOf[Def] : scalarNode(Def));
        else if (Latch)
          OwnEdges.push_back({0, Latch, false, Dist});
      }

      assert(PredCur == RG.Preds[NI].size() &&
             CarriedCur == RG.Carried[NI].size() && "unconsumed graph edges");
      std::optional<OpCost> C;
      if (InGroup) {
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
        C = PD.irOpCost(Key);
        if (!C && P)
          return false;
      }
      N.Cost = C ? *C : Fallback(I);
      unsigned Me = Nodes.size();
      Nodes.push_back(std::move(N));
      NodeOf[&I] = Me;
      for (CarriedEdge &E : OwnEdges) {
        E.Dest = Me;
        Pending.push_back(E);
      }

      // Live-out of a Singleton Vectorized group: back to a GPR unless every
      // user outside the group is a store, which can store from the vector.
      if (P && is_contained(R.Groups[GIdx].LiveOuts, &I)) {
        if (liveOutNeedsGpr(R.Groups[GIdx], I)) {
          unsigned T;
          if (!addTransfer(TransferKind::VecToGpr, {Me}, T))
            return false;
          OutNode[&I] = T;
        }
      }
    }

    for (const CarriedEdge &E : Pending) {
      unsigned Src = E.Joined ? NodeOf[E.Latch] : scalarNode(E.Latch);
      Nodes[E.Dest].Carried.push_back({Src, E.Dist});
    }
    return true;
  }

  ArrayRef<CostNode> nodes() const { return Nodes; }

private:
  /// The node producing the scalar value of \p I: its VecToGpr node if it is a
  /// Singleton Vectorized live-out, else its own.
  unsigned scalarNode(const Instruction *I) const {
    auto It = OutNode.find(I);
    return It != OutNode.end() ? It->second : NodeOf.lookup(I);
  }
};

} // namespace

Decision savio::decide(const Region &R, const PortDescription &PD,
                       FallbackCostFn Fallback, double Margin) {
  unsigned N = R.Groups.size();
  SmallVector<bool, 8> Promoted(N, false);
  bool IsLoop = R.L != nullptr;
  RegionGraph RG = buildRegionGraph(*R.BB, R.L);

  // Estimate of the current assignment; nullopt if it needs a missing cost.
  auto Estimate = [&]() -> std::optional<RegionEstimate> {
    NodeBuilder B(R, RG, PD, Fallback, Promoted);
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
