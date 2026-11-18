//===- Partitioner.cpp - SAVIO groups and the fraction search -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "llvm/Transforms/Vectorize/SAVIO/Availability.h"
#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
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

static bool isShift(unsigned Opc) {
  return Opc == Instruction::Shl || Opc == Instruction::LShr ||
         Opc == Instruction::AShr;
}

/// The name of the count operation a call to llvm.ctpop.i32 / llvm.ctlz.i32
/// is ("ctpop" / "ctlz"; any is_zero_poison flag), else empty. These are the
/// only calls SAVIO prices or moves; they have one data operand (the second
/// operand of llvm.ctlz is the immediate flag, and the callee is an operand of
/// every call: use dataOperands, not operands()).
static StringRef bitCountName(const Instruction &I) {
  if (!I.getType()->isIntegerTy(32))
    return "";
  switch (I.getOpcode() == Instruction::Call
              ? cast<CallInst>(I).getIntrinsicID()
              : Intrinsic::not_intrinsic) {
  case Intrinsic::ctpop:
    return "ctpop";
  case Intrinsic::ctlz:
    return "ctlz";
  default:
    return "";
  }
}

/// The operation name of \p I as the port-info keys spell it: the opcode name,
/// or "ctpop" / "ctlz" for those calls.
static std::string opName(const Instruction &I) {
  StringRef BC = bitCountName(I);
  return BC.empty() ? std::string(I.getOpcodeName()) : BC.str();
}

/// The operands of \p I that are data: all of them, except for a call, where
/// the callee (and the immediate flag of llvm.ctlz, which is no data) are left
/// out.
static iterator_range<User::const_op_iterator>
dataOperands(const Instruction &I) {
  if (!bitCountName(I).empty())
    return make_range(I.op_begin(), I.op_begin() + 1);
  return I.operands();
}

/// Singleton Vectorize candidates: i32 add sub mul and or xor shl lshr ashr
/// udiv urem, and calls to llvm.ctpop.i32 / llvm.ctlz.i32 (the latter's
/// lane-0 form defines the zero case as 32, which is exact for both values of
/// is_zero_poison; the flag is an immediate, so it is always a constant).
static bool isScalarCandidate(const Instruction &I) {
  if (!I.getType()->isIntegerTy(32))
    return false;
  if (StringRef BC = bitCountName(I); !BC.empty())
    return BC == "ctpop" || isa<ConstantInt>(I.getOperand(1));
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

/// Scalarize candidates: add sub mul and or xor shl lshr ashr on fixed vectors
/// of i32 or i64 that are 128, 256 or 512 bits wide (whether the port-info has
/// the keys is decided at candidacy, by isAvailable). Vector udiv/urem stay
/// with the lane-group path.
static bool isVectorCandidate(const Instruction &I) {
  auto *VT = dyn_cast<FixedVectorType>(I.getType());
  if (!VT || !(VT->getElementType()->isIntegerTy(32) ||
               VT->getElementType()->isIntegerTy(64)))
    return false;
  uint64_t Bits = VT->getPrimitiveSizeInBits().getFixedValue();
  if (Bits != 128 && Bits != 256 && Bits != 512)
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
    return true;
  default:
    return false;
  }
}

static bool isCandidate(const Instruction &I) {
  return isScalarCandidate(I) || isVectorCandidate(I);
}

static bool isImmAmount(const Value *V);

/// Whether the transformation \p X of \p I has every port-info key it needs
/// (see requiredKeys); no description (\p PD null) means no check. Logs the
/// keys looked up, or the first one missing.
static bool isAvailable(const PortDescription *PD, Transform X,
                        const Instruction &I, unsigned L = 0) {
  if (!PD)
    return true;
  std::string Op = opName(I);
  if (isShift(I.getOpcode()) &&
      (X == Transform::SingletonVectorize ? isa<ConstantInt>(I.getOperand(1))
                                          : isImmAmount(I.getOperand(1))))
    Op += ".imm";
  SmallVector<std::string, 8> Keys = requiredKeys(X, Op, I.getType(), L);
  std::optional<std::string> Missing = firstMissing(
      *PD, Keys, X == Transform::Scalarize ? typeKey(I.getType()) : "");
  LLVM_DEBUG({
    dbgs() << "savio: " << transformName(X) << " of " << opName(I) << ' '
           << *I.getType();
    if (Missing) {
      dbgs() << " not available: missing '" << *Missing << "'\n";
    } else {
      dbgs() << " keys:";
      for (const std::string &K : Keys)
        dbgs() << " '" << K << "'";
      dbgs() << "\n";
    }
  });
  return !Missing;
}

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

//===----------------------------------------------------------------------===//
// Scalarize costing helpers
//===----------------------------------------------------------------------===//

/// A constant shift amount: a scalar constant, or a splat of one.
static bool isImmAmount(const Value *V) {
  auto *C = dyn_cast<Constant>(V);
  return C && (isa<ConstantInt>(C) || C->getSplatValue());
}

static unsigned laneCount(const Group &G) {
  return cast<FixedVectorType>(G.Ops[0]->getType())->getNumElements();
}

/// "v4i32" / "v8i32" / "v2i64" ... (vector form) or "i32" / "i64" (scalar lane
/// form).
static std::string laneTypeName(const Group &G, bool Vector) {
  auto *VT = cast<FixedVectorType>(G.Ops[0]->getType());
  return typeKey(Vector ? static_cast<Type *>(VT) : VT->getElementType());
}

/// The shift of \p U's operands that the backend folds into \p U (shifted-
/// register form, or USRA/SSRA for the vector add of a right shift): a shift
/// of the group by a constant amount whose only user is \p U. The shifted
/// operand of `sub` must be the second. Null if none. \p Vector selects the
/// vector form, in which only `add` of a right shift fuses.
static const Instruction *fusibleShift(const Instruction &U, const Group &G,
                                       bool Vector) {
  switch (U.getOpcode()) {
  case Instruction::Add:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    break;
  case Instruction::Sub:
    if (Vector)
      return nullptr;
    break;
  default:
    return nullptr;
  }
  for (unsigned K : {1u, 0u}) {
    if (K == 0 && U.getOpcode() == Instruction::Sub)
      break;
    auto *S = dyn_cast<Instruction>(U.getOperand(K));
    if (!S || !isShift(S->getOpcode()) || !S->hasOneUse() ||
        !is_contained(G.Ops, S) || !isImmAmount(S->getOperand(1)))
      continue;
    if (Vector && !(U.getOpcode() == Instruction::Add &&
                    S->getOpcode() != Instruction::Shl))
      continue;
    return S;
  }
  return nullptr;
}

/// Cost of the operation \p I of the ToScalar group \p G in its vector form
/// (\p Vector, as LLVM left it) or as one scalar operation of a lane (not yet
/// multiplied by the lane count). A shift by a constant uses the ".imm" key. A
/// shift folded into its user is charged once, with the fused key, on the user
/// (the shift then costs nothing); if the port-info has no fused key the two
/// operations are charged separately. Null if a needed key is missing.
static std::optional<OpCost> scalarizeOpCost(const PortDescription &PD,
                                              const Instruction &I,
                                              const Group &G, bool Vector) {
  std::string Ty = laneTypeName(G, Vector);
  auto Fused = [&](const Instruction &U, const Instruction &S) {
    return (Twine(U.getOpcodeName()) + "." + S.getOpcodeName() + ".imm " + Ty)
        .str();
  };
  // A shift folded into its (only) user.
  if (isShift(I.getOpcode()) && I.hasOneUse())
    if (auto *U = dyn_cast<Instruction>(*I.user_begin()))
      if (is_contained(G.Ops, U) && fusibleShift(*U, G, Vector) == &I &&
          PD.irOpCost(Fused(*U, I)))
        return OpCost{{}, 0, 0};
  if (const Instruction *S = fusibleShift(I, G, Vector))
    if (std::optional<OpCost> C = PD.irOpCost(Fused(I, *S)))
      return C;
  bool Imm = isShift(I.getOpcode()) && isImmAmount(I.getOperand(1));
  return PD.irOpCost(
      (Twine(I.getOpcodeName()) + (Imm ? ".imm " : " ") + Ty).str());
}

/// Cost of \p Lanes copies of \p C issued side by side: resource use and
/// micro-ops add up, latency does not (the lanes are independent).
static OpCost lanes(OpCost C, unsigned Lanes) {
  for (ResourceUse &U : C.Uses)
    U.Cycles *= Lanes;
  C.Uops *= Lanes;
  return C;
}

/// True if the operand \p V of the Scalarized group \p G reaches its lanes
/// without a lane extract: a constant (splat or not: immediates), a vector load
/// the slice owns (split into scalar loads), or, in a loop with a preheader, a
/// loop-invariant value (its lanes are extracted once in the preheader).
static bool scalarizeInputIsFree(const Loop *L, const Group &G,
                                 const Value *V) {
  if (isa<Constant>(V))
    return true;
  if (auto *LD = dyn_cast<LoadInst>(V))
    if (is_contained(G.OwnedLoads, LD))
      return true;
  return L && L->getLoopPreheader() && L->isLoopInvariant(V);
}

namespace {

/// Whether \p I is an operation on a 512-bit vector type: it produces one, or
/// is a store of one.
static bool is512BitOp(const Instruction &I) {
  Type *T = I.getType();
  if (auto *SI = dyn_cast<StoreInst>(&I))
    T = SI->getValueOperand()->getType();
  auto *VT = dyn_cast<FixedVectorType>(T);
  return VT && VT->getPrimitiveSizeInBits().getFixedValue() == 512;
}

/// The port-info cost of an instruction that is in no group: it stays as it
/// is before and after the move, so it adds a fixed load to the resource and
/// dispatch bounds. Loads, stores, the lane extracts / inserts the SLP
/// vectorizer leaves at the scalar boundary, and the arithmetic and logic ops
/// that have an "<op> <type>" key. Nullopt (the caller's latency-only
/// fallback, logged under -debug-only=savio) for anything without a key.
static std::optional<OpCost> outOfGroupOpCost(const PortDescription &PD,
                                              const Instruction &I) {
  std::optional<OpCost> C;
  if (auto *EE = dyn_cast<ExtractElementInst>(&I)) {
    auto *VT = dyn_cast<FixedVectorType>(EE->getVectorOperand()->getType());
    auto *Idx = dyn_cast<ConstantInt>(EE->getIndexOperand());
    if (VT && Idx && VT->getElementType()->isIntegerTy())
      C = PD.transferCost(Idx->isZero() ? TransferKind::VecToGpr
                                        : TransferKind::LaneExtract);
  } else if (auto *IE = dyn_cast<InsertElementInst>(&I)) {
    auto *VT = dyn_cast<FixedVectorType>(IE->getType());
    auto *Idx = dyn_cast<ConstantInt>(IE->getOperand(2));
    if (VT && Idx && VT->getElementType()->isIntegerTy())
      C = PD.transferCost(Idx->isZero() && isa<PoisonValue>(IE->getOperand(0))
                              ? TransferKind::GprToVec
                              : TransferKind::LaneInsert);
  } else if (auto *LD = dyn_cast<LoadInst>(&I)) {
    C = PD.irOpCost("load " + typeKey(LD->getType()));
  } else if (auto *ST = dyn_cast<StoreInst>(&I)) {
    C = PD.irOpCost(
        "store " + typeKey(ST->getValueOperand()->getType()));
  } else if (!bitCountName(I).empty()) {
    C = PD.irOpCost(opName(I) + " i32");
  } else if (I.isBinaryOp() && !I.getType()->isFPOrFPVectorTy()) {
    bool Imm = isa<ConstantInt>(I.getOperand(1)) &&
               (I.getOpcode() == Instruction::Shl ||
                I.getOpcode() == Instruction::LShr ||
                I.getOpcode() == Instruction::AShr);
    C = PD.irOpCost((Twine(I.getOpcodeName()) + (Imm ? ".imm " : " ") +
                     typeKey(I.getType()))
                        .str());
  } else {
    return std::nullopt;
  }
  LLVM_DEBUG(if (!C) dbgs() << "savio: no port-info cost for out-of-group "
                            << I << "\n");
  LLVM_DEBUG(if (C) dbgs() << "savio: out-of-group charge: " << I << "\n");
  return C;
}

/// Cost model nodes of a region for one assignment, one node per instruction
/// of the region graph plus the transfers a Singleton Vectorized group needs.
///
/// In-scope operations of groups are priced from the port description (scalar
/// or lane-0 vector form); every other instruction (loads, stores, address
/// arithmetic, calls, compares, branches, other types) from the fallback, which
/// gives it latency and micro-ops but no resource use (unless the port
/// description sets chargeOutOfGroup(): then outOfGroupOpCost prices loads,
/// stores, lane extracts / inserts and integer ops). So a dependence through
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
  /// The lane groups of each vector divide node, by group index.
  DenseMap<unsigned, SmallVector<unsigned, 4>> LaneGroups;
  SmallVector<bool, 32> ControlOnly;
  /// The ToScalar group owning each load/store node, if any.
  DenseMap<unsigned, unsigned> MemOwner;
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

  /// A transfer node. For a Scalarize boundary (\p VecTy given, \p K a lane
  /// kind): the per-width entry for \p VecTy ("lane_extract <vty>" /
  /// "lane_insert <vty>", the whole-vector sequence) once, when the port-info
  /// has it; every boundary moves all the lanes (partial transfers are a
  /// follow-up). Without it, the untyped key charged \p Lanes times (resource
  /// use and micro-ops times Lanes; latency as is, or times Lanes when
  /// \p Chain: the inserts into one vector depend on each other).
  bool addTransfer(TransferKind K, ArrayRef<unsigned> Preds, unsigned &Out,
                   unsigned Lanes = 1, bool Chain = false,
                   Type *VecTy = nullptr) {
    CostNode Nd;
    bool IsLane =
        K == TransferKind::LaneExtract || K == TransferKind::LaneInsert;
    std::optional<OpCost> W;
    if (VecTy && IsLane) {
      W = PD.laneTransferCost(K, typeKey(VecTy));
      if (W) {
        Nd.Cost = *W;
        Nd.Wide512 = VecTy->getPrimitiveSizeInBits().getFixedValue() == 512;
      }
    }
    if (!W) {
      std::optional<OpCost> C = PD.transferCost(K);
      if (!C)
        return false;
      Nd.Cost = lanes(*C, Lanes);
      if (Chain)
        Nd.Cost.Latency *= Lanes;
    }
    LLVM_DEBUG({
      if (VecTy && IsLane) {
        dbgs() << "savio: transfer "
               << (K == TransferKind::LaneExtract ? "lane_extract"
                                                  : "lane_insert");
        if (W)
          dbgs() << ' ' << typeKey(VecTy) << " per-width";
        else
          dbgs() << " per-lane x" << Lanes;
        dbgs() << ": uops " << Nd.Cost.Uops << " latency " << Nd.Cost.Latency
               << " uses";
        for (const ResourceUse &U : Nd.Cost.Uses)
          dbgs() << ' ' << PD.className(U.Class) << ' ' << U.Cycles;
        dbgs() << '\n';
      }
    });
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
    for (unsigned G = 0; G != NG; ++G) {
      for (LoadInst *LD : R.Groups[G].OwnedLoads)
        MemOwner[RG.Index.lookup(LD)] = G;
      for (StoreInst *S : R.Groups[G].OwnedStores)
        MemOwner[RG.Index.lookup(S)] = G;
    }
    for (unsigned G = 0; G != NG; ++G)
      for (Instruction *I : R.Groups[G].Ops) {
        unsigned NI = RG.Index.lookup(I);
        if (!R.Groups[G].VectorDivide) {
          GroupOfNode[NI] = G;
        } else {
          SmallVector<unsigned, 4> &LG = LaneGroups[NI];
          LG.resize(std::max<size_t>(LG.size(), R.Groups[G].LaneGroup + 1));
          LG[R.Groups[G].LaneGroup] = G;
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
      unsigned DefV = None;   // Same-iteration producer, as a virtual id.
      unsigned LatchV = None; // Carried producer (a phi wrapped past copy 0).
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
      // A ToScalar group's non-free input is a lane extract of the vector.
      bool Vec = InGroup && R.Groups[GIdx].Dir == Direction::ToScalar;
      InputKind K = InputKind::Free;
      if (P && !SameGroup) {
        if (!Vec)
          K = classifyInput(R.L, R.Groups[GIdx], Op);
        else if (!scalarizeInputIsFree(R.L, R.Groups[GIdx], Op))
          K = InputKind::GprToVec; // Stands for "needs a transfer".
      }
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
          bool Ok =
              Vec ? addTransfer(TransferKind::LaneExtract, TP, T,
                                laneCount(R.Groups[GIdx]), /*Chain=*/false,
                                I.getType())
                  : addTransfer(K == InputKind::GprToVec
                                    ? TransferKind::GprToVec
                                    : TransferKind::LaneToLane0,
                                TP, T);
          if (!Ok)
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
    if (auto LG = LaneGroups.find(NI); LaneGroups.end() != LG)
      return buildVectorDivide(J, NI, LG->second, Nd, OwnEdges);
    std::optional<OpCost> C;
    const ConcurrencyTable *Conc = nullptr;
    auto OwnerIt = MemOwner.find(NI);
    if (InGroup && R.Groups[GIdx].Dir == Direction::ToScalar) {
      // Scalarize candidate: its vector form as LLVM left it, or one scalar
      // operation per lane. An unpromoted one without a key costs what the
      // fallback says; a promoted one cannot be costed.
      const Group &G = R.Groups[GIdx];
      C = scalarizeOpCost(PD, I, G, /*Vector=*/!P);
      if (!C && P)
        return false;
      if (C && P)
        C = lanes(*C, laneCount(G));
    } else if (OwnerIt != MemOwner.end()) {
      // A load or store owned by a ToScalar group: a vector access, or one
      // scalar access per lane when the group is Scalarized. (The scalar
      // accesses are charged per op by their summed address-unit use, which
      // over-charges a split copy by about 15% against measured pairs: a known
      // limitation of the key-per-op model.)
      const Group &G = R.Groups[OwnerIt->second];
      bool MP = Promoted[virtualGroup(OwnerIt->second, J)];
      C = PD.irOpCost((Twine(isa<LoadInst>(I) ? "load " : "store ") +
                       laneTypeName(G, !MP))
                          .str());
      if (!C && MP)
        return false;
      if (C && MP)
        C = lanes(*C, laneCount(G));
    } else if (InGroup) {
      std::string Key;
      if (P) {
        bool Imm = isShift(I.getOpcode()) && isa<ConstantInt>(I.getOperand(1));
        Key = "sv." + opName(I) + (Imm ? ".imm v4i32" : " v4i32");
      } else {
        Key = opName(I) + " i32";
      }
      C = PD.irOpCost(Key);
      if (!C && P)
        return false;
      if (C && P)
        Conc = PD.concurrency(Key);
    }
    if (!C && !InGroup && PD.chargeOutOfGroup())
      C = outOfGroupOpCost(PD, I);
    Nd.Cost = C ? *C : Fallback(I);
    Nd.Conc = Conc;
    // A Scalarized operation is scalar code, whatever the IR type says.
    bool Scalarized =
        (InGroup && R.Groups[GIdx].Dir == Direction::ToScalar && P) ||
        (OwnerIt != MemOwner.end() &&
         Promoted[virtualGroup(OwnerIt->second, J)]);
    Nd.Wide512 = !Scalarized && is512BitOp(I);
    unsigned Me = Nodes.size();
    Nodes.push_back(std::move(Nd));
    NodeOf[V] = Me;
    for (CarriedEdge &E : OwnEdges) {
      E.Dest = Me;
      Pending.push_back(E);
    }

    // Live-out of a Scalarized group: its lanes back into a vector, unless
    // every user outside the group is a store the group owns.
    if (P && R.Groups[GIdx].Dir == Direction::ToScalar) {
      const Group &G = R.Groups[GIdx];
      // Not when every user outside the group is a constant-lane extract the
      // out-of-group charge already prices (the SLP vectorizer's lane
      // extracts): the backend folds extract-of-insert, so the lane insert
      // would charge the same lanes twice.
      auto IsChargedExtract = [&](const User *U) {
        auto *EE = dyn_cast<ExtractElementInst>(U);
        return EE && isa<ConstantInt>(EE->getIndexOperand()) &&
               !isInGroup(G, U);
      };
      bool ExtractsOnly = PD.chargeOutOfGroup() &&
                          all_of(I.users(), [&](const User *U) {
                            return isInGroup(G, U) ||
                                   is_contained(G.OwnedStores,
                                                dyn_cast<StoreInst>(U)) ||
                                   IsChargedExtract(U);
                          });
      if (is_contained(G.LiveOuts, &I) && liveOutNeedsInsert(G, I) &&
          !ExtractsOnly) {
        unsigned T;
        if (!addTransfer(TransferKind::LaneInsert, {Me}, T, laneCount(G),
                         /*Chain=*/true, I.getType()))
          return false;
        OutNode[V] = T;
      }
      return true;
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

  /// A `udiv`/`urem <N x i32>`: one node per lane group, joined by a node of
  /// no cost that stands for the instruction. A group that stays is its share
  /// of the backend's scalar split (key "<op> v<N>i32", the resource use and
  /// micro-ops divided by the number of groups, the same latency); the first
  /// m groups together are the fpdiv<m*G> sequence ("fpdiv<m*G>.<op> v<N>i32",
  /// G lanes per group), at its measured whole-sequence cost, plus a
  /// concurrency table if the entry has one. Without the split's cost the
  /// instruction costs what the fallback says, and cannot be moved. The
  /// operands are vectors already: there is no transfer.
  bool buildVectorDivide(unsigned J, unsigned NI, ArrayRef<unsigned> Groups,
                         CostNode &Nd, ArrayRef<CarriedEdge> OwnEdges) {
    const Instruction &I = *RG.Insts[NI];
    unsigned V = J * N + NI;
    unsigned NumGroups = Groups.size();
    unsigned Size = R.Groups[Groups[0]].GroupLanes;
    SmallVector<bool, 4> Pr;
    unsigned Moved = 0; // The groups moved are the first Moved ones.
    bool Prefix = true;
    for (unsigned Idx : Groups) {
      bool P = Promoted[virtualGroup(Idx, J)];
      Pr.push_back(P);
      if (P && Moved != Pr.size() - 1)
        Prefix = false;
      Moved += P;
    }
    std::string Ty = typeKey(I.getType());
    std::string Op = (Twine(I.getOpcodeName()) + " " + Ty).str();
    std::optional<OpCost> Base = PD.irOpCost(Op);
    if (!Base) {
      if (Moved)
        return false;
      Nd.Cost = Fallback(I);
      Nd.Wide512 = is512BitOp(I);
      NodeOf[V] = Nodes.size();
      unsigned Me = Nodes.size();
      Nodes.push_back(std::move(Nd));
      for (const CarriedEdge &E : OwnEdges)
        Pending.push_back({Me, E.Latch, E.Joined, E.Dist});
      return true;
    }
    // A later group alone, or after a gap, would need operand and result
    // shuffles that no sequence prices: not an assignment the model can cost,
    // so never chosen. Nor is a lane count that has no intrinsic.
    unsigned MovedLanes = Moved * Size;
    if (!Prefix || (Moved && (MovedLanes < 2 || !isPowerOf2_32(MovedLanes))))
      return false;
    std::optional<OpCost> Seq;
    const ConcurrencyTable *SeqConc = nullptr;
    if (Moved) {
      std::string Key =
          fpDivideKey(I.getOpcodeName(), I.getType(), MovedLanes, Size);
      Seq = PD.irOpCost(Key);
      if (!Seq)
        return false;
      SeqConc = PD.concurrency(Key);
    }
    SmallVector<unsigned, 4> GroupNodes;
    for (unsigned P = 0; P != NumGroups; ++P) {
      CostNode PN;
      PN.Preds = Nd.Preds;
      if (Pr[P]) {
        // The moved groups are one sequence, carried by the node of group 0;
        // the nodes of the other moved groups cost nothing.
        if (P == 0) {
          PN.Cost = *Seq;
          PN.Conc = SeqConc;
          // A zmm divide is emitted only where the backend uses zmm: the
          // function holds a 512-bit vector, or the lane group is 512 bits
          // (G = 8). Two ymm divides (G = 4 on a v8i32) are not 512-bit.
          PN.Wide512 =
              MovedLanes * 64 >= 512 &&
              (Size * 64 == 512 ||
               I.getType()->getPrimitiveSizeInBits().getFixedValue() >= 512);
        } else {
          PN.Cost = OpCost{{}, 0, 0};
        }
      } else {
        // A group that stays: its share of the split's resource use and
        // micro-ops (the odd micro-ops go to the first groups). The latency
        // stays that of the whole split, which is pessimistic for a part.
        PN.Cost = *Base;
        for (ResourceUse &U : PN.Cost.Uses)
          U.Cycles /= NumGroups;
        PN.Cost.Uops = (PN.Cost.Uops + NumGroups - 1 - P) / NumGroups;
      }
      GroupNodes.push_back(Nodes.size());
      Nodes.push_back(std::move(PN));
      for (const CarriedEdge &E : OwnEdges)
        Pending.push_back({GroupNodes.back(), E.Latch, E.Joined, E.Dist});
    }
    CostNode Join;
    Join.Cost = OpCost{{}, 0, 0};
    Join.Wide512 = is512BitOp(I);
    Join.Preds.assign(GroupNodes.begin(), GroupNodes.end());
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
  E.TMem = R.MemFloor; // Per original iteration: the unroll does not change it.
  LLVM_DEBUG({
    dbgs() << "savio: estimate (copies " << Copies << ", promoted";
    for (unsigned I = 0; I != Promoted.size(); ++I)
      if (Promoted[I])
        dbgs() << ' ' << I;
    dbgs() << "): TRes " << E.TRes << " (" << PD.className(E.BusiestClass)
           << ") TRec " << E.TRec << " TDisp " << E.TDisp << " TMem "
           << E.TMem;
    if (PD.hasPortSets())
      dbgs() << " with512=" << E.With512;
    dbgs() << "\n";
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
    // A vector divide's instances are lane groups: the count is in lanes.
    unsigned Limit = SAVIOForceCount;
    for (const Group &G : R.Groups)
      if (G.SliceClass == BestClass && G.VectorDivide) {
        if (Limit % G.GroupLanes) {
          if (G.GroupLanes == 2)
            report_fatal_error("savio-force-count: a vector divide moves lane "
                               "pairs, so the count of lanes must be even");
          report_fatal_error("-savio-force-count=" + Twine(Limit) +
                             " must be a multiple of the lane group " +
                             Twine(G.GroupLanes));
        }
        Limit /= G.GroupLanes;
        break;
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
