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

/// Singleton Vectorize gives no intrinsic flag operands.  Refuse operations
/// whose poison/undef semantics are stronger than an ordinary scalar
/// operation, rather than silently dropping nuw/nsw/exact.

static bool isSimpleStoreOf(const User *U, const Value *V) {
  auto *S = dyn_cast<StoreInst>(U);
  return S && S->isSimple() && S->getValueOperand() == V &&
         S->getPointerOperand() != V;
}

/// True if the value of \p I reaches an address (a GEP operand or an inttoptr)
/// through casts, phis and other in-scope operations: addresses must stay in
/// GPRs, so such an operation is not a candidate. The walk stops at a bit count
/// (a call: whether it is in scope depends on the port information, which this
/// walk does not see; stopping keeps the behaviour of the code before bit counts
/// were candidates, on every target).
static bool reachesAddress(const Instruction &I) {
  SmallVector<const Instruction *, 8> Work{&I};
  SmallPtrSet<const Instruction *, 8> Seen{&I};
  while (!Work.empty()) {
    const Instruction *Cur = Work.pop_back_val();
    for (const User *U : Cur->users()) {
      if (isa<GetElementPtrInst>(U) || isa<IntToPtrInst>(U))
        return true;
      if (isa<SExtInst>(U) || isa<ZExtInst>(U) || isa<TruncInst>(U) ||
          isa<PHINode>(U) ||
          (isCandidate(*cast<Instruction>(U)) &&
           bitCountName(*cast<Instruction>(U)).empty())) {
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

/// The groups of one region: every whole all-in-scope recurrence (with its
/// phis, kept in vector form), and the use-def connected slices of the
/// candidates outside recurrences. Operations of a recurrence that has a member
/// out of scope are not candidates, so no transfer lands on a cycle.
/// \p Classes numbers the slice classes of the function; \p RegionNo keeps the
/// classes of different regions apart.
static SmallVector<Group, 4> formGroups(BasicBlock &BB, Loop *L,
                                        StringMap<unsigned> &Classes,
                                        unsigned RegionNo, unsigned LaneGroup,
                                        const PortDescription *PD) {
  RegionGraph RG = buildRegionGraph(BB, L);
  unsigned N = RG.Insts.size();
  // Scalar candidates move to the vector unit, vector candidates to the scalar
  // ports; a slice is one or the other. A vector operation is never part of a
  // recurrence group: recurrences are pinned in the form LLVM chose.
  SmallVector<bool, 32> InScope(N, false), IsVec(N, false);
  for (unsigned I = 0; I != N; ++I) {
    const Instruction &In = *RG.Insts[I];
    IsVec[I] = isVectorCandidate(In);
    InScope[I] = isCandidate(In) && !reachesAddress(In) &&
                 isAvailable(PD,
                             IsVec[I] ? Transform::Scalarize
                                      : Transform::SingletonVectorize,
                             In);
  }

  SmallVector<bool, 8> CompCycle;
  SmallVector<unsigned, 32> Comp = computeSCCs(RG, CompCycle);
  SmallVector<SmallVector<unsigned, 4>, 8> CompNodes(CompCycle.size());
  for (unsigned I = 0; I != N; ++I)
    CompNodes[Comp[I]].push_back(I);
  // A bit count on a loop-carried chain stays scalar (v1: such chains are
  // pinned unless every member is a plain arithmetic operation); the rest of
  // that chain is then pinned with it. Bit counts off every chain move.
  for (unsigned I = 0; I != N; ++I)
    if (CompCycle[Comp[I]] && !bitCountName(*RG.Insts[I]).empty())
      InScope[I] = false;

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
    bool AllInScope = all_of(
        CompNodes[C], [&](unsigned I) { return InScope[I] && !IsVec[I]; });
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
        if (DSIndex[P] != None && IsVec[P] == IsVec[I])
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
      Groups.back().Dir = IsVec[I] ? Direction::ToScalar : Direction::ToVector;
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
    // v1: innermost single-block loops, and blocks outside any loop.
    if (L && !(L->isInnermost() && L->getNumBlocks() == 1))
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
