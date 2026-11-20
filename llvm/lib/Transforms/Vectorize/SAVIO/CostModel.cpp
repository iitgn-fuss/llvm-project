//===- CostModel.cpp - SAVIO assignment costing --------------------------===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//

// Costing and fraction search are kept separate from region formation so the
// cost model can be read and tested without navigating the grouping logic.
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "CostModelUtils.h"
#include "PartitionerUtils.h"
#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include "llvm/Transforms/Vectorize/SAVIO/RegionCost.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <cfloat>
#include <optional>
#include <tuple>

using namespace llvm;
using namespace llvm::savio;

#define DEBUG_TYPE "savio"

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
    std::string TyKey = typeKey(LD->getType());
    if (TyKey.empty()) {
      if (LD->getType()->isPointerTy())
        TyKey = "i64";
      else if (LD->getType()->isIntegerTy())
        TyKey = "i32";
    }
    C = PD.irOpCost("load " + TyKey);
  } else if (auto *ST = dyn_cast<StoreInst>(&I)) {
    std::string TyKey = typeKey(ST->getValueOperand()->getType());
    if (TyKey.empty()) {
      if (ST->getValueOperand()->getType()->isPointerTy())
        TyKey = "i64";
      else if (ST->getValueOperand()->getType()->isIntegerTy())
        TyKey = "i32";
    }
    C = PD.irOpCost("store " + TyKey);
  } else if (!supportedIntrinsicName(I).empty()) {
    std::string TyKey = typeKey(I.getType());
    if (TyKey.empty() && I.getType()->isIntegerTy())
      TyKey = "i32";
    C = PD.irOpCost(opName(I) + " " + TyKey);
  } else if (I.isBinaryOp() && !I.getType()->isFPOrFPVectorTy()) {
    bool Imm = isa<ConstantInt>(I.getOperand(1)) &&
               (I.getOpcode() == Instruction::Shl ||
                I.getOpcode() == Instruction::LShr ||
                I.getOpcode() == Instruction::AShr);
    std::string TyKey = typeKey(I.getType());
    if (TyKey.empty() && I.getType()->isIntegerTy())
      TyKey = "i32";
    C = PD.irOpCost((Twine(I.getOpcodeName()) + (Imm ? ".imm " : " ") +
                     TyKey)
                        .str());
  } else if (isa<SelectInst>(&I) && !I.getType()->isFPOrFPVectorTy()) {
    std::string TyKey = typeKey(I.getType());
    if (TyKey.empty()) {
      if (I.getType()->isPointerTy())
        TyKey = "i64";
      else if (I.getType()->isIntegerTy())
        TyKey = "i32";
    }
    C = PD.irOpCost("select " + TyKey);
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
  unsigned NumInsts;        // Number of non-PHI instruction nodes in the region graph.
  unsigned NumRegionGroups; // Number of transformation groups in the region.

  SmallVector<CostNode, 32> Nodes;
  /// Node of each virtual instruction (CopyIdx * NumInsts + GraphNodeIdx), or None.
  SmallVector<unsigned, 32> NodeOf, OutNode;
  /// Group of each graph node, or None.
  SmallVector<unsigned, 32> GroupOfNode;
  /// The lane groups of each vector divide node, by group index.
  DenseMap<unsigned, SmallVector<unsigned, 4>> LaneGroups;
  SmallVector<bool, 32> ControlOnly;
  /// The ToScalar group owning each load/store node, if any.
  DenseMap<unsigned, unsigned> MemOwner;

  /// Cache of boundary transfer nodes keyed by:
  /// (VirtualGroupIdx, OperandValue, SourceCopyIdx) -> TransferCostNodeIdx.
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
        Copies(Copies), NumInsts(RG.Insts.size()), NumRegionGroups(R.Groups.size()),
        NodeOf(size_t(Copies) * NumInsts, None), OutNode(size_t(Copies) * NumInsts, None),
        GroupOfNode(NumInsts, None), ControlOnly(NumInsts, false) {
    for (unsigned G = 0; G != NumRegionGroups; ++G) {
      for (LoadInst *LD : R.Groups[G].OwnedLoads)
        MemOwner[RG.Index.lookup(LD)] = G;
      for (StoreInst *S : R.Groups[G].OwnedStores)
        MemOwner[RG.Index.lookup(S)] = G;
    }
    for (unsigned G = 0; G != NumRegionGroups; ++G)
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
      for (unsigned I = 0; I != NumInsts; ++I) {
        const Instruction *In = RG.Insts[I];
        ControlOnly[I] = In->isTerminator() ||
                         (!In->use_empty() &&
                          all_of(In->users(), [](const User *U) {
                            return cast<Instruction>(U)->isTerminator();
                          }));
      }
  }

  /// The virtual group of group \p G in copy \p CopyIdx.
  unsigned virtualGroup(unsigned G, unsigned CopyIdx) const {
    return R.Groups[G].Recurrence ? G : CopyIdx * NumRegionGroups + G;
  }

  /// Fills the nodes; false if the assignment needs a cost the port
  /// description lacks.
  bool build() {
    for (unsigned CopyIdx = 0; CopyIdx != Copies; ++CopyIdx)
      for (unsigned NodeIdx = 0; NodeIdx != NumInsts; ++NodeIdx)
        if ((CopyIdx + 1 == Copies || !ControlOnly[NodeIdx]) && !buildNode(CopyIdx, NodeIdx))
          return false;

    for (const CarriedEdge &E : Pending) {
      unsigned Src = E.Joined ? NodeOf[E.Latch] : scalarNode(E.Latch);
      Nodes[E.Dest].Carried.push_back({Src, E.Dist});
    }
    return true;
  }

  ArrayRef<CostNode> nodes() const { return Nodes; }

private:
  bool buildNode(unsigned CopyIdx, unsigned NodeIdx) {
    const Instruction &I = *RG.Insts[NodeIdx];
    unsigned VirtualInstIdx = CopyIdx * NumInsts + NodeIdx;

    //===------------------------------------------------------------------===//
    // 1. Resolve Operands and Predecessors
    //===------------------------------------------------------------------===//
    // The region graph holds one edge per operand defined in the block, in
    // operand order: intra-iteration edges in Preds, loop-carried through a
    // header PHI in Carried.
    unsigned PredCur = 0, CarriedCur = 0;
    bool InGroup = GroupOfNode[NodeIdx] != None;
    unsigned GIdx = InGroup ? GroupOfNode[NodeIdx] : 0;
    unsigned VG = virtualGroup(GIdx, CopyIdx);
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
          assert(CarriedCur < RG.Carried[NodeIdx].size() && "missing carried edge");
          auto [LatchNode, D] = RG.Carried[NodeIdx][CarriedCur++];
          if (CopyIdx >= D) {
            // The value of an earlier copy of this unrolled iteration.
            SrcCopy = CopyIdx - D;
            DefV = SrcCopy * NumInsts + LatchNode;
            Op = RG.Insts[LatchNode];
          } else {
            SrcCopy = ((int(CopyIdx) - int(D)) % int(Copies) + int(Copies)) %
                      int(Copies);
            LatchV = SrcCopy * NumInsts + LatchNode;
            Dist = (D - CopyIdx + Copies - 1) / Copies;
            Joined = InGroup && is_contained(R.Groups[GIdx].CarriedPhis, Phi);
          }
        }
      } else if (auto *OpI = dyn_cast<Instruction>(Op)) {
        if (RG.Index.count(OpI)) {
          assert(PredCur < RG.Preds[NodeIdx].size() && "missing pred edge");
          unsigned DefNode = RG.Preds[NodeIdx][PredCur++];
          assert(RG.Insts[DefNode] == OpI && "region graph edge order differs");
          SrcCopy = CopyIdx;
          DefV = CopyIdx * NumInsts + DefNode;
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
        unsigned DG = GroupOfNode[DefV % NumInsts];
        SameGroup = DG != None && DG == GIdx &&
                    virtualGroup(DG, DefV / NumInsts) == VG;
      }
      // A ToScalar group's non-free input is a lane extract of the vector.
      bool Vec = InGroup && R.Groups[GIdx].Dir == Direction::ToScalar;
      InputKind K = InputKind::Free;
      if (P && !SameGroup) {
        if (!Vec) {
          bool ProducerInVector = false;
          if (DefV != None) {
            unsigned DG = GroupOfNode[DefV % NumInsts];
            if (DG != None && R.Groups[DG].Dir == Direction::ToVector) {
              unsigned DVG = virtualGroup(DG, DefV / NumInsts);
              if (Promoted[DVG])
                ProducerInVector = true;
            }
          }
          if (ProducerInVector)
            K = InputKind::Free;
          else
            K = classifyInput(R.L, R.Groups[GIdx], Op);
        } else if (!scalarizeInputIsFree(R.L, R.Groups[GIdx], Op))
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

    assert(PredCur == RG.Preds[NodeIdx].size() &&
           CarriedCur == RG.Carried[NodeIdx].size() && "unconsumed graph edges");
    if (auto LG = LaneGroups.find(NodeIdx); LaneGroups.end() != LG)
      return buildVectorDivide(CopyIdx, NodeIdx, LG->second, Nd, OwnEdges);

    //===------------------------------------------------------------------===//
    // 2. Compute Operation Cost
    //===------------------------------------------------------------------===//
    std::optional<OpCost> C;
    const ConcurrencyTable *Conc = nullptr;
    auto OwnerIt = MemOwner.find(NodeIdx);
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
      bool MP = Promoted[virtualGroup(OwnerIt->second, CopyIdx)];
      C = PD.irOpCost((Twine(isa<LoadInst>(I) ? "load " : "store ") +
                       laneTypeName(G, !MP))
                          .str());
      if (!C && MP)
        return false;
      if (C && MP)
        C = lanes(*C, laneCount(G));
    } else if (InGroup) {
      std::string Key;
      bool Imm = isShift(I.getOpcode()) && isa<ConstantInt>(I.getOperand(1));
      if (P) {
          if (I.getType()->isIntegerTy(64))
            Key = opName(I) + (Imm ? ".imm " : " ") + "v2i64";
          else
            Key = "sv." + opName(I) + (Imm ? ".imm " : " ") + "v4i32";
        } else {
          std::string EltTyStr = typeKey(I.getType());
          Key = opName(I) + (Imm ? ".imm " : " ") + EltTyStr;
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
         Promoted[virtualGroup(OwnerIt->second, CopyIdx)]);
    Nd.Wide512 = !Scalarized && is512BitOp(I);
    unsigned Me = Nodes.size();
    Nodes.push_back(std::move(Nd));
    NodeOf[VirtualInstIdx] = Me;
    for (CarriedEdge &E : OwnEdges) {
      E.Dest = Me;
      Pending.push_back(E);
    }

    //===------------------------------------------------------------------===//
    // 3. Handle Live-Out Boundary Transfers
    //===------------------------------------------------------------------===//
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
        OutNode[VirtualInstIdx] = T;
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
        OutNode[VirtualInstIdx] = T;
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
  bool buildVectorDivide(unsigned CopyIdx, unsigned NodeIdx, ArrayRef<unsigned> Groups,
                         CostNode &Nd, ArrayRef<CarriedEdge> OwnEdges) {
    const Instruction &I = *RG.Insts[NodeIdx];
    unsigned VirtualInstIdx = CopyIdx * NumInsts + NodeIdx;
    unsigned NumDivideGroups = Groups.size();
    unsigned Size = R.Groups[Groups[0]].GroupLanes;
    SmallVector<bool, 4> Pr;
    unsigned Moved = 0; // The groups moved are the first Moved ones.
    bool Prefix = true;
    for (unsigned Idx : Groups) {
      bool P = Promoted[virtualGroup(Idx, CopyIdx)];
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
      NodeOf[VirtualInstIdx] = Nodes.size();
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
    for (unsigned P = 0; P != NumDivideGroups; ++P) {
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
          U.Cycles /= NumDivideGroups;
        PN.Cost.Uops = (PN.Cost.Uops + NumDivideGroups - 1 - P) / NumDivideGroups;
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
    NodeOf[VirtualInstIdx] = Nodes.size();
    Nodes.push_back(std::move(Join));
    return true;
  }

  /// The node producing the scalar value of virtual instruction \p VirtualInstIdx:
  /// its VecToGpr node if it is a Singleton Vectorized live-out, else its own.
  unsigned scalarNode(unsigned VirtualInstIdx) const {
    return OutNode[VirtualInstIdx] != None ? OutNode[VirtualInstIdx]
                                          : NodeOf[VirtualInstIdx];
  }
};

} // namespace

namespace llvm {
namespace savio {

/// Estimate an assignment on the region unrolled \p Copies times, per
/// original iteration; nullopt if it needs a missing cost.
std::optional<RegionEstimate>
estimateAssignment(const Region &R, const RegionGraph &RG,
                   const PortDescription &PD, FallbackCostFn Fallback,
                   ArrayRef<bool> Promoted, unsigned Copies) {
  NodeBuilder B(R, RG, PD, Fallback, Promoted, Copies);
  if (!B.build())
    return std::nullopt;
  SmallVector<CostNode, 32> Nodes(B.nodes().begin(), B.nodes().end());
  applyConcurrency(Nodes, PD, R.L != nullptr);
  RegionEstimate E = estimateRegion(Nodes, PD, R.L != nullptr);
  E.TRes /= Copies;
  E.TRec /= Copies;
  E.TDisp /= Copies;
  E.TMem = R.MemFloor;
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

} // namespace savio
} // namespace llvm
