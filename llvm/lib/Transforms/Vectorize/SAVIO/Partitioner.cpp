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
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <tuple>
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

cl::opt<int> llvm::SAVIOForceCount(
    "savio-force-count", cl::init(-1), cl::Hidden,
    cl::desc("Singleton Vectorize the first k instances (program order) of "
             "the slice class with the most instances in each region, and "
             "nothing else (-1 = off; for calibration sweeps)"));

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
      for (Use &U : I->operands())
        if (!InGroup(U.get()))
          Grp.Inputs.push_back(&U);
      if (any_of(I->users(), [&](User *U) { return !InGroup(U); }))
        Grp.LiveOuts.push_back(I);
    }
    std::string Key = sliceKey(Grp, L, RegionNo);
    if (Grp.Recurrence) // A whole recurrence is one instance: never shared.
      Key += "@" + std::to_string(Classes.size());
    Grp.SliceClass = Classes.try_emplace(Key, Classes.size()).first->second;
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

  // The lane pairs of each unsigned divide or remainder of <4 x i32> by a
  // non-constant divisor (a constant divisor becomes a multiply: the integer
  // divider is not used and the fdiv form would lose): two groups, pair
  // {0,1} then pair {2,3}, instances of one slice class per opcode.
  for (const Instruction *CI : RG.Insts) {
    auto *I = const_cast<Instruction *>(CI);
    auto *VT = dyn_cast<FixedVectorType>(I->getType());
    if (!VT || VT->getNumElements() != 4 ||
        !VT->getElementType()->isIntegerTy(32) ||
        (I->getOpcode() != Instruction::UDiv &&
         I->getOpcode() != Instruction::URem) ||
        isa<Constant>(I->getOperand(1)))
      continue;
    std::string Key = (Twine(RegionNo) + "V:" + I->getOpcodeName() +
                       " v4i32")
                          .str();
    unsigned Cls = Classes.try_emplace(Key, Classes.size()).first->second;
    for (unsigned Pair = 0; Pair != 2; ++Pair) {
      Groups.emplace_back();
      Group &Grp = Groups.back();
      Grp.Ops.push_back(I);
      Grp.SliceClass = Cls;
      Grp.VectorDivide = true;
      Grp.Pair = Pair;
    }
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
///
/// The builder can also model the region unrolled \p Copies times without
/// touching the IR ("virtual unroll"): the body is laid out Copies times, the
/// carried edge of distance d from copy j goes to copy j-d (a same-iteration
/// edge) or, past the first copy, wraps to the last copies with a distance in
/// unrolled iterations; the loop-control branch and the compare that only
/// feeds it appear once, in the last copy. Groups of a copy are separate
/// instances (virtual group j * NumGroups + g), except a recurrence, which is
/// one chain through all copies and keeps the virtual group g.
class NodeBuilder {
  const Region &R;
  const RegionGraph &RG;
  const PortDescription &PD;
  FallbackCostFn Fallback;
  ArrayRef<bool> Promoted; // Indexed by virtual group.
  unsigned Copies;
  unsigned N;  // Nodes of the region graph.
  unsigned NG; // Groups of the region.

  SmallVector<CostNode, 32> Nodes;
  /// Node of each virtual instruction (copy * N + graph node), or None.
  SmallVector<unsigned, 32> NodeOf, OutNode;
  /// Group of each graph node, or None.
  SmallVector<unsigned, 32> GroupOfNode;
  /// The two lane-pair groups of each vector divide node (pair 0, pair 1).
  DenseMap<unsigned, std::pair<unsigned, unsigned>> PairGroups;
  SmallVector<bool, 32> ControlOnly;
  DenseMap<std::tuple<unsigned, const Value *, unsigned>, unsigned> InNode;
  static constexpr unsigned None = ~0u;
  /// A loop-carried edge to add once all nodes exist: the value of virtual
  /// instruction Latch, Dist unrolled iterations earlier, reaches node Dest.
  /// Through a phi kept in vector form (Joined) it stays in the vector domain;
  /// otherwise it is the scalar value, i.e. the VecToGpr node if the latch
  /// operation has one.
  struct CarriedEdge {
    unsigned Dest;
    unsigned Latch;
    bool Joined;
    unsigned Dist;
  };
  SmallVector<CarriedEdge, 4> Pending;

  bool addTransfer(TransferKind K, ArrayRef<unsigned> Preds, unsigned &Out) {
    std::optional<OpCost> C = PD.transferCost(K);
    if (!C)
      return false;
    CostNode Nd;
    Nd.Cost = *C;
    Nd.Preds.assign(Preds.begin(), Preds.end());
    Out = Nodes.size();
    Nodes.push_back(std::move(Nd));
    return true;
  }

public:
  NodeBuilder(const Region &R, const RegionGraph &RG, const PortDescription &PD,
              FallbackCostFn Fallback, ArrayRef<bool> Promoted,
              unsigned Copies = 1)
      : R(R), RG(RG), PD(PD), Fallback(Fallback), Promoted(Promoted),
        Copies(Copies), N(RG.Insts.size()), NG(R.Groups.size()),
        NodeOf(size_t(Copies) * N, None), OutNode(size_t(Copies) * N, None),
        GroupOfNode(N, None), ControlOnly(N, false) {
    for (unsigned G = 0; G != NG; ++G)
      for (Instruction *I : R.Groups[G].Ops) {
        unsigned NI = RG.Index.lookup(I);
        if (!R.Groups[G].VectorDivide) {
          GroupOfNode[NI] = G;
        } else if (R.Groups[G].Pair == 0) {
          PairGroups[NI].first = G;
        } else {
          PairGroups[NI].second = G;
        }
      }
    // In a loop, the terminator and what only feeds it (the exit test).
    if (R.L)
      for (unsigned I = 0; I != N; ++I) {
        const Instruction *In = RG.Insts[I];
        ControlOnly[I] = In->isTerminator() ||
                         (!In->use_empty() &&
                          all_of(In->users(), [](const User *U) {
                            return cast<Instruction>(U)->isTerminator();
                          }));
      }
  }

  /// The virtual group of group \p G in copy \p J.
  unsigned virtualGroup(unsigned G, unsigned J) const {
    return R.Groups[G].Recurrence ? G : J * NG + G;
  }

  /// Fills the nodes; false if the assignment needs a cost the port
  /// description lacks.
  bool build() {
    for (unsigned J = 0; J != Copies; ++J)
      for (unsigned NI = 0; NI != N; ++NI)
        if ((J + 1 == Copies || !ControlOnly[NI]) && !buildNode(J, NI))
          return false;

    for (const CarriedEdge &E : Pending) {
      unsigned Src = E.Joined ? NodeOf[E.Latch] : scalarNode(E.Latch);
      Nodes[E.Dest].Carried.push_back({Src, E.Dist});
    }
    return true;
  }

  ArrayRef<CostNode> nodes() const { return Nodes; }

private:
  bool buildNode(unsigned J, unsigned NI) {
    const Instruction &I = *RG.Insts[NI];
    unsigned V = J * N + NI;
    // The region graph holds one edge per operand defined in the block, in
    // operand order: same-iteration in Preds, through a header phi in
    // Carried.
    unsigned PredCur = 0, CarriedCur = 0;
    bool InGroup = GroupOfNode[NI] != None;
    unsigned GIdx = InGroup ? GroupOfNode[NI] : 0;
    unsigned VG = virtualGroup(GIdx, J);
    bool P = InGroup && Promoted[VG];

    CostNode Nd;
    // Carried edges whose destination is this instruction, resolved below.
    SmallVector<CarriedEdge, 2> OwnEdges;
    for (const Use &U : I.operands()) {
      const Value *Op = U.get();
      unsigned DefV = None;       // Same-iteration producer, as a virtual id.
      unsigned LatchV = None;     // Carried producer (a phi wrapped past copy 0).
      unsigned Dist = 1, SrcCopy = 0;
      bool Joined = false;
      if (auto *Phi = dyn_cast<PHINode>(Op)) {
        // A loop phi closes a recurrence through this operand when its latch
        // value is an instruction of the block: a carried edge.
        if (RG.PhiLatch.count(Phi)) {
          assert(CarriedCur < RG.Carried[NI].size() && "missing carried edge");
          auto [LatchNode, D] = RG.Carried[NI][CarriedCur++];
          if (J >= D) {
            // The value of an earlier copy of this unrolled iteration.
            SrcCopy = J - D;
            DefV = SrcCopy * N + LatchNode;
            Op = RG.Insts[LatchNode];
          } else {
            SrcCopy = ((int(J) - int(D)) % int(Copies) + int(Copies)) %
                      int(Copies);
            LatchV = SrcCopy * N + LatchNode;
            Dist = (D - J + Copies - 1) / Copies;
            Joined = InGroup && is_contained(R.Groups[GIdx].CarriedPhis, Phi);
          }
        }
      } else if (auto *OpI = dyn_cast<Instruction>(Op)) {
        if (RG.Index.count(OpI)) {
          assert(PredCur < RG.Preds[NI].size() && "missing pred edge");
          unsigned DefNode = RG.Preds[NI][PredCur++];
          assert(RG.Insts[DefNode] == OpI && "region graph edge order differs");
          SrcCopy = J;
          DefV = J * N + DefNode;
        }
      }

      if (Joined) {
        OwnEdges.push_back({0, LatchV, true, Dist});
        continue;
      }
      // The operand's value in this instruction's domain: another
      // instruction of the same group is read as is; anything else is the
      // scalar value, i.e. through the VecToGpr node of a live-out.
      bool SameGroup = false;
      if (DefV != None && InGroup) {
        unsigned DG = GroupOfNode[DefV % N];
        SameGroup = DG != None && DG == GIdx &&
                    virtualGroup(DG, DefV / N) == VG;
      }
      InputKind K = InputKind::Free;
      if (P && !SameGroup)
        K = classifyInput(R.L, R.Groups[GIdx], Op);
      if (P && !SameGroup && K != InputKind::Free) {
        auto Key = std::make_tuple(VG, Op, SrcCopy);
        auto It = InNode.find(Key);
        unsigned T;
        if (It != InNode.end()) {
          T = It->second;
        } else {
          SmallVector<unsigned, 1> TP;
          if (DefV != None)
            TP.push_back(scalarNode(DefV));
          if (!addTransfer(K == InputKind::GprToVec ? TransferKind::GprToVec
                                                    : TransferKind::LaneToLane0,
                           TP, T))
            return false;
          InNode[Key] = T;
          if (LatchV != None)
            Pending.push_back({T, LatchV, false, Dist});
        }
        Nd.Preds.push_back(T);
        continue;
      }
      if (DefV != None)
        Nd.Preds.push_back(SameGroup ? NodeOf[DefV] : scalarNode(DefV));
      else if (LatchV != None)
        OwnEdges.push_back({0, LatchV, false, Dist});
    }

    assert(PredCur == RG.Preds[NI].size() &&
           CarriedCur == RG.Carried[NI].size() && "unconsumed graph edges");
    if (auto PG = PairGroups.find(NI); PG != PairGroups.end())
      return buildVectorDivide(J, NI, PG->second, Nd, OwnEdges);
    std::optional<OpCost> C;
    const ConcurrencyTable *Conc = nullptr;
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
      if (C && P)
        Conc = PD.concurrency(Key);
    }
    Nd.Cost = C ? *C : Fallback(I);
    Nd.Conc = Conc;
    unsigned Me = Nodes.size();
    Nodes.push_back(std::move(Nd));
    NodeOf[V] = Me;
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
        OutNode[V] = T;
      }
    }
    return true;
  }

  /// A `udiv`/`urem <4 x i32>`: one node per lane pair, joined by a node of no
  /// cost that stands for the instruction. A pair that stays is half of the
  /// backend's scalar split (key "<op> v4i32", half the resource use and
  /// micro-ops, the same latency); lanes {0,1} alone are the fpdiv2 sequence
  /// ("fpdiv2.<op> v4i32"), both pairs the fpdiv4 sequence, each with its
  /// concurrency table. Without the split's cost the
  /// instruction costs what the fallback says, and cannot be moved. The
  /// operands are vectors already: there is no transfer.
  bool buildVectorDivide(unsigned J, unsigned NI,
                         std::pair<unsigned, unsigned> Groups, CostNode &Nd,
                         ArrayRef<CarriedEdge> OwnEdges) {
    const Instruction &I = *RG.Insts[NI];
    unsigned V = J * N + NI;
    bool Pr[2] = {bool(Promoted[virtualGroup(Groups.first, J)]),
                  bool(Promoted[virtualGroup(Groups.second, J)])};
    std::string Op = (Twine(I.getOpcodeName()) + " v4i32").str();
    std::optional<OpCost> Base = PD.irOpCost(Op);
    if (!Base) {
      if (Pr[0] || Pr[1])
        return false;
      Nd.Cost = Fallback(I);
      NodeOf[V] = Nodes.size();
      unsigned Me = Nodes.size();
      Nodes.push_back(std::move(Nd));
      for (const CarriedEdge &E : OwnEdges)
        Pending.push_back({Me, E.Latch, E.Joined, E.Dist});
      return true;
    }
    // Lanes {2,3} alone would need operand and result shuffles that no
    // sequence prices: not an assignment the model can cost, so never chosen.
    if (!Pr[0] && Pr[1])
      return false;
    SmallVector<unsigned, 2> PairNodes;
    for (unsigned P = 0; P != 2; ++P) {
      CostNode PN;
      PN.Preds = Nd.Preds;
      if (Pr[0] && Pr[1]) {
        // Both pairs: one four-lane sequence, carried by the node of pair 0;
        // the node of pair 1 costs nothing.
        if (P == 0) {
          std::string Key = "fpdiv4." + Op;
          std::optional<OpCost> C = PD.irOpCost(Key);
          if (!C)
            return false;
          PN.Cost = *C;
          PN.Conc = PD.concurrency(Key);
        } else {
          PN.Cost = OpCost{{}, 0, 0};
        }
      } else if (Pr[P]) {
        std::string Key = "fpdiv2." + Op;
        std::optional<OpCost> C = PD.irOpCost(Key);
        if (!C)
          return false;
        PN.Cost = *C;
        PN.Conc = PD.concurrency(Key);
      } else {
        // A pair that stays: half the split's resource use and micro-ops (the
        // odd micro-op goes to pair 0). The latency stays that of the whole
        // split, which is pessimistic for a two-lane remainder.
        PN.Cost = *Base;
        for (ResourceUse &U : PN.Cost.Uses)
          U.Cycles /= 2;
        PN.Cost.Uops = P == 0 ? (PN.Cost.Uops + 1) / 2 : PN.Cost.Uops / 2;
      }
      PairNodes.push_back(Nodes.size());
      Nodes.push_back(std::move(PN));
      for (const CarriedEdge &E : OwnEdges)
        Pending.push_back({PairNodes.back(), E.Latch, E.Joined, E.Dist});
    }
    CostNode Join;
    Join.Cost = OpCost{{}, 0, 0};
    Join.Preds.assign(PairNodes.begin(), PairNodes.end());
    NodeOf[V] = Nodes.size();
    Nodes.push_back(std::move(Join));
    return true;
  }

  /// The node producing the scalar value of virtual instruction \p V: its
  /// VecToGpr node if it is a Singleton Vectorized live-out, else its own.
  unsigned scalarNode(unsigned V) const {
    return OutNode[V] != None ? OutNode[V] : NodeOf[V];
  }
};

} // namespace

static void recordMoved(const Region &R, Decision &D) {
  DenseMap<unsigned, unsigned> Total;
  for (const Group &G : R.Groups)
    ++Total[G.SliceClass];
  SmallVector<ClassMove, 2> M;
  for (unsigned G : D.Promoted) {
    unsigned C = R.Groups[G].SliceClass;
    auto *It = find_if(M, [&](const ClassMove &P) { return P.Class == C; });
    if (It == M.end())
      M.push_back({C, 1, Total[C]});
    else
      ++It->K;
  }
  D.MovedPerClass = M;
}

namespace {

/// Outcome of the fraction search on the region unrolled \p Copies times.
struct SearchResult {
  unsigned Copies = 1;
  RegionEstimate Before, After; // Per original iteration.
  SmallVector<bool, 8> Promoted;  // By virtual group.
  SmallVector<ClassMove, 2> Moves;
  bool any() const { return !Moves.empty(); }
};

} // namespace

/// Estimate of an assignment on the region unrolled \p Copies times, per
/// original iteration; nullopt if it needs a missing cost.
static std::optional<RegionEstimate>
estimateAssignment(const Region &R, const RegionGraph &RG,
                   const PortDescription &PD, FallbackCostFn Fallback,
                   ArrayRef<bool> Promoted, unsigned Copies) {
  NodeBuilder B(R, RG, PD, Fallback, Promoted, Copies);
  if (!B.build())
    return std::nullopt;
  // Second pass: sequence costs depend on how many instances are in flight.
  SmallVector<CostNode, 32> Nodes(B.nodes().begin(), B.nodes().end());
  applyConcurrency(Nodes, PD, R.L != nullptr);
  RegionEstimate E = estimateRegion(Nodes, PD, R.L != nullptr);
  E.TRes /= Copies;
  E.TRec /= Copies;
  E.TDisp /= Copies;
  LLVM_DEBUG({
    dbgs() << "savio: estimate (copies " << Copies << ", promoted";
    for (unsigned I = 0; I != Promoted.size(); ++I)
      if (Promoted[I])
        dbgs() << ' ' << I;
    dbgs() << "): TRes " << E.TRes << " (" << PD.className(E.BusiestClass)
           << ") TRec " << E.TRec << " TDisp " << E.TDisp << "\n";
  });
  return E;
}

/// Fraction search. Unlike the greedy loop it replaces, there is no "only
/// while the resource bound limits" precondition: any bound may be lowered,
/// guarded by the margin. Per slice class, estimate every k of its U instances
/// (spread evenly) with the other classes fixed; take the best if it beats
/// the current T by the margin. Repeat over classes until nothing changes.
/// With \p Copies > 1 the region is the loop unrolled that many times (not
/// done in the IR); a recurrence stays one instance through all copies.
static SearchResult searchFraction(const Region &R, const RegionGraph &RG,
                                   const PortDescription &PD,
                                   FallbackCostFn Fallback, double Margin,
                                   unsigned Copies) {
  unsigned NG = R.Groups.size();
  SearchResult Res;
  Res.Copies = Copies;
  Res.Promoted.assign(size_t(Copies) * NG, false);
  SmallVector<bool, 8> &Promoted = Res.Promoted;
  auto Estimate = [&] {
    return estimateAssignment(R, RG, PD, Fallback, Promoted, Copies);
  };
  Res.Before = Res.After = *Estimate();

  // The movable instances of each class, as virtual groups in program order of
  // the unrolled body.
  SmallVector<unsigned, 4> Classes;
  DenseMap<unsigned, SmallVector<unsigned, 4>> Members;
  for (unsigned J = 0; J != Copies; ++J)
    for (unsigned G = 0; G != NG; ++G) {
      const Group &Grp = R.Groups[G];
      if (Grp.Recurrence && J != 0)
        continue;
      unsigned C = Grp.SliceClass;
      if (Members[C].empty())
        Classes.push_back(C);
      Members[C].push_back(Grp.Recurrence ? G : J * NG + G);
    }
  DenseMap<unsigned, unsigned> Current;
  auto Apply = [&](ArrayRef<unsigned> M, unsigned K) {
    for (unsigned G : M)
      Promoted[G] = false;
    for (unsigned I = 0; I != K; ++I)
      Promoted[M[size_t(I) * M.size() / K]] = true;
  };
  for (unsigned Round = 0; Round != 4; ++Round) {
    bool Changed = false;
    for (unsigned C : Classes) {
      ArrayRef<unsigned> M = Members[C];
      unsigned U = M.size(), Cur = Current[C];
      std::optional<RegionEstimate> Best;
      unsigned BestK = Cur;
      for (unsigned K = 0; K <= U; ++K) {
        if (K == Cur)
          continue;
        Apply(M, K);
        std::optional<RegionEstimate> E = Estimate();
        if (E && (!Best || E->t() < Best->t())) {
          Best = E;
          BestK = K;
        }
      }
      if (Best && Res.After.t() - Best->t() > 0 &&
          Res.After.t() - Best->t() >= Margin * Res.After.t()) {
        Apply(M, BestK);
        Current[C] = BestK;
        Res.After = *Best;
        Changed = true;
      } else {
        Apply(M, Cur);
      }
    }
    if (!Changed)
      break;
  }
  for (unsigned C : Classes)
    if (Current[C])
      Res.Moves.push_back({C, Current[C], unsigned(Members[C].size())});
  return Res;
}

Decision savio::decide(const Region &R, const PortDescription &PD,
                       FallbackCostFn Fallback, double Margin,
                       bool CanUnroll) {
  unsigned N = R.Groups.size();
  SmallVector<bool, 8> Promoted(N, false);
  RegionGraph RG = buildRegionGraph(*R.BB, R.L);

  auto Estimate = [&]() -> std::optional<RegionEstimate> {
    return estimateAssignment(R, RG, PD, Fallback, Promoted, 1);
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
    recordMoved(R, D);
    return D;
  }

  if (SAVIOForceCount >= 0) {
    // The slice class with the most instances (the first one found on ties).
    DenseMap<unsigned, unsigned> Count;
    unsigned BestClass = 0, BestCount = 0;
    for (const Group &G : R.Groups) {
      unsigned C = ++Count[G.SliceClass];
      if (C > BestCount) {
        BestCount = C;
        BestClass = G.SliceClass;
      }
    }
    // A vector divide's instances are lane pairs: the count is in lanes.
    unsigned Limit = SAVIOForceCount;
    if (any_of(R.Groups, [&](const Group &G) {
          return G.SliceClass == BestClass && G.VectorDivide;
        })) {
      if (Limit % 2)
        report_fatal_error("savio-force-count: a vector divide moves lane "
                           "pairs, so the count of lanes must be even");
      Limit /= 2;
    }
    unsigned Taken = 0;
    for (unsigned G = 0; G != N && Taken < Limit; ++G) {
      if (R.Groups[G].SliceClass != BestClass)
        continue;
      Promoted[G] = true;
      if (!Estimate()) {
        Promoted[G] = false;
        continue;
      }
      D.Promoted.push_back(G);
      ++Taken;
    }
    D.After = *Estimate();
    recordMoved(R, D);
    return D;
  }

  SearchResult Base = searchFraction(R, RG, PD, Fallback, Margin, 1);
  const SearchResult *Chosen = Base.any() ? &Base : nullptr;

  // Unrolling to reach a better fraction: U instances of the most numerous
  // acyclic slice class are present now (copies); try U = 2*copies, ... <= 8,
  // every candidate searched as a whole. A candidate is taken if it moves
  // something and beats the unchanged loop by the margin; among those the
  // smallest unroll within the margin of the best wins (less code growth).
  SmallVector<SearchResult, 4> Unrolled;
  if (CanUnroll) {
    DenseMap<unsigned, unsigned> Count;
    unsigned Copies = 0;
    for (const Group &G : R.Groups)
      if (!G.Recurrence)
        Copies = std::max(Copies, ++Count[G.SliceClass]);
    for (unsigned M = 2; Copies && M * Copies <= 8; ++M) {
      SearchResult S = searchFraction(R, RG, PD, Fallback, Margin, M);
      // It must beat the unchanged loop by the margin and, if some fraction
      // of the loop as it is already pays, that one too: code growth is only
      // worth a real gain over not unrolling.
      double Ref = Chosen ? Chosen->After.t() : Base.Before.t();
      if (S.any() && Base.Before.t() - S.After.t() > 0 &&
          Base.Before.t() - S.After.t() >= Margin * Base.Before.t() &&
          Ref - S.After.t() > 0 && Ref - S.After.t() >= Margin * Ref)
        Unrolled.push_back(std::move(S));
    }
  }
  if (!Unrolled.empty()) {
    double Best = Chosen ? Chosen->After.t() : Unrolled[0].After.t();
    for (const SearchResult &S : Unrolled)
      Best = std::min(Best, S.After.t());
    const SearchResult *Pick = nullptr;
    if (Chosen && Chosen->After.t() <= Best * (1 + Margin))
      Pick = Chosen;
    for (const SearchResult &S : Unrolled)
      if (!Pick && S.After.t() <= Best * (1 + Margin))
        Pick = &S;
    Chosen = Pick;
  }

  if (!Chosen)
    return D;
  D.After = Chosen->After;
  D.Unroll = Chosen->Copies;
  D.MovedPerClass = Chosen->Moves;
  if (D.Unroll == 1)
    for (unsigned G = 0; G != N; ++G)
      if (Chosen->Promoted[G])
        D.Promoted.push_back(G);
  return D;
}
