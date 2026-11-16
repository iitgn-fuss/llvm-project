//===- AArch64SAVIOExpand.cpp - Expand SAVIO Singleton Vectorize pseudos --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file Expands the SAVIO_SV_*_V4I32 pseudos (selected from the
/// llvm.savio.sv.*.v4i32 intrinsics) into real NEON/FP instructions. Runs in
/// SSA form before register allocation, so the register allocator sees the
/// final instructions.
///
/// Lane 0 of each result is the scalar operation on lane 0 of the operands; the
/// other lanes are unspecified. Constant-count shifts use the immediate forms,
/// variable-count shifts use USHL/SSHL (negating the count for right shifts).
/// Unsigned divide of lane 0 goes through double precision: both operands are
/// widened to 64 bits and converted (exact), divided, and truncated back. The
/// quotient of two 32-bit values is exact this way (see the SAVIO lane-0 udiv
/// notes): when a / p is not an integer its distance to the next integer is at
/// least 1/p, while the rounding error of the division is at most
/// 2^32 / p * 2^-53, which is smaller. urem adds an MLS on the 32-bit lanes.
//===----------------------------------------------------------------------===//

#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "aarch64-savio-expand"
#define AARCH64_SAVIO_EXPAND_NAME "AArch64 SAVIO Singleton Vectorize expansion"

STATISTIC(NumExpanded, "SAVIO pseudos expanded");

namespace {

class AArch64SAVIOExpandImpl {
public:
  bool run(MachineFunction &MF);

private:
  const TargetInstrInfo *TII = nullptr;
  MachineRegisterInfo *MRI = nullptr;

  bool expand(MachineInstr &MI);
  void expandBinary(MachineInstr &MI, unsigned Opc);
  void expandShiftImm(MachineInstr &MI, unsigned Opc);
  void expandVarShift(MachineInstr &MI, unsigned Opc, bool NegateCount);
  void expandDiv(MachineInstr &MI, bool Rem);

  Register lowHalf(MachineInstr &MI, Register Vec);
};

class AArch64SAVIOExpandLegacy : public MachineFunctionPass {
public:
  static char ID;
  AArch64SAVIOExpandLegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    return AArch64SAVIOExpandImpl().run(MF);
  }

  StringRef getPassName() const override { return AARCH64_SAVIO_EXPAND_NAME; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};
char AArch64SAVIOExpandLegacy::ID = 0;

} // end anonymous namespace

INITIALIZE_PASS(AArch64SAVIOExpandLegacy, "aarch64-savio-expand",
                AARCH64_SAVIO_EXPAND_NAME, false, false)

/// The low 64 bits (dsub) of the 128-bit register \p Vec, as a new FPR64.
Register AArch64SAVIOExpandImpl::lowHalf(MachineInstr &MI, Register Vec) {
  Register Lo = MRI->createVirtualRegister(&AArch64::FPR64RegClass);
  BuildMI(*MI.getParent(), MI, MI.getDebugLoc(), TII->get(TargetOpcode::COPY),
          Lo)
      .addReg(Vec, {}, AArch64::dsub);
  return Lo;
}

void AArch64SAVIOExpandImpl::expandBinary(MachineInstr &MI, unsigned Opc) {
  BuildMI(*MI.getParent(), MI, MI.getDebugLoc(), TII->get(Opc),
          MI.getOperand(0).getReg())
      .add(MI.getOperand(1))
      .add(MI.getOperand(2));
}

void AArch64SAVIOExpandImpl::expandShiftImm(MachineInstr &MI, unsigned Opc) {
  BuildMI(*MI.getParent(), MI, MI.getDebugLoc(), TII->get(Opc),
          MI.getOperand(0).getReg())
      .add(MI.getOperand(1))
      .add(MI.getOperand(2));
}

void AArch64SAVIOExpandImpl::expandVarShift(MachineInstr &MI, unsigned Opc,
                                            bool NegateCount) {
  MachineBasicBlock &MBB = *MI.getParent();
  DebugLoc DL = MI.getDebugLoc();
  Register Count = MI.getOperand(2).getReg();
  if (NegateCount) {
    Register Neg = MRI->createVirtualRegister(&AArch64::FPR128RegClass);
    BuildMI(MBB, MI, DL, TII->get(AArch64::NEGv4i32), Neg).addReg(Count);
    Count = Neg;
  }
  BuildMI(MBB, MI, DL, TII->get(Opc), MI.getOperand(0).getReg())
      .add(MI.getOperand(1))
      .addReg(Count);
}

void AArch64SAVIOExpandImpl::expandDiv(MachineInstr &MI, bool Rem) {
  MachineBasicBlock &MBB = *MI.getParent();
  DebugLoc DL = MI.getDebugLoc();
  Register Dst = MI.getOperand(0).getReg();
  Register A = MI.getOperand(1).getReg();
  Register P = MI.getOperand(2).getReg();

  // Widen the 32-bit lane 0 to a 64-bit lane (ushll .2d, #0) and convert it to
  // double; the converted value is in the low 64 bits of the result.
  auto ToDouble = [&](Register V) {
    Register Lo = lowHalf(MI, V);
    Register Wide = MRI->createVirtualRegister(&AArch64::FPR128RegClass);
    BuildMI(MBB, MI, DL, TII->get(AArch64::USHLLv2i32_shift), Wide)
        .addReg(Lo)
        .addImm(0);
    Register Wlo = lowHalf(MI, Wide);
    Register D = MRI->createVirtualRegister(&AArch64::FPR64RegClass);
    BuildMI(MBB, MI, DL, TII->get(AArch64::UCVTFv1i64), D).addReg(Wlo);
    return D;
  };

  Register DA = ToDouble(A);
  Register DP = ToDouble(P);
  Register DQ = MRI->createVirtualRegister(&AArch64::FPR64RegClass);
  BuildMI(MBB, MI, DL, TII->get(AArch64::FDIVDrr), DQ).addReg(DA).addReg(DP);
  Register Q = MRI->createVirtualRegister(&AArch64::FPR64RegClass);
  BuildMI(MBB, MI, DL, TII->get(AArch64::FCVTZUv1i64), Q).addReg(DQ);

  // The quotient (below 2^32) is in the low 32 bits of Q. Put the result in a
  // 128-bit register; the remaining lanes are unspecified.
  Register Res = Q;
  if (Rem) {
    Register ALo = lowHalf(MI, A);
    Register PLo = lowHalf(MI, P);
    Res = MRI->createVirtualRegister(&AArch64::FPR64RegClass);
    BuildMI(MBB, MI, DL, TII->get(AArch64::MLSv2i32), Res)
        .addReg(ALo)
        .addReg(Q)
        .addReg(PLo);
  }
  BuildMI(MBB, MI, DL, TII->get(TargetOpcode::SUBREG_TO_REG), Dst)
      .addReg(Res)
      .addImm(AArch64::dsub);
}

bool AArch64SAVIOExpandImpl::expand(MachineInstr &MI) {
  switch (MI.getOpcode()) {
  default:
    return false;
  case AArch64::SAVIO_SV_ADD_V4I32:
    expandBinary(MI, AArch64::ADDv4i32);
    break;
  case AArch64::SAVIO_SV_SUB_V4I32:
    expandBinary(MI, AArch64::SUBv4i32);
    break;
  case AArch64::SAVIO_SV_MUL_V4I32:
    expandBinary(MI, AArch64::MULv4i32);
    break;
  case AArch64::SAVIO_SV_AND_V4I32:
    expandBinary(MI, AArch64::ANDv16i8);
    break;
  case AArch64::SAVIO_SV_OR_V4I32:
    expandBinary(MI, AArch64::ORRv16i8);
    break;
  case AArch64::SAVIO_SV_XOR_V4I32:
    expandBinary(MI, AArch64::EORv16i8);
    break;
  case AArch64::SAVIO_SV_SHL_V4I32:
    expandVarShift(MI, AArch64::USHLv4i32, /*NegateCount=*/false);
    break;
  case AArch64::SAVIO_SV_LSHR_V4I32:
    expandVarShift(MI, AArch64::USHLv4i32, /*NegateCount=*/true);
    break;
  case AArch64::SAVIO_SV_ASHR_V4I32:
    expandVarShift(MI, AArch64::SSHLv4i32, /*NegateCount=*/true);
    break;
  case AArch64::SAVIO_SV_SHL_IMM_V4I32:
    expandShiftImm(MI, AArch64::SHLv4i32_shift);
    break;
  case AArch64::SAVIO_SV_LSHR_IMM_V4I32:
    expandShiftImm(MI, AArch64::USHRv4i32_shift);
    break;
  case AArch64::SAVIO_SV_ASHR_IMM_V4I32:
    expandShiftImm(MI, AArch64::SSHRv4i32_shift);
    break;
  case AArch64::SAVIO_SV_UDIV_V4I32:
    expandDiv(MI, /*Rem=*/false);
    break;
  case AArch64::SAVIO_SV_UREM_V4I32:
    expandDiv(MI, /*Rem=*/true);
    break;
  }
  ++NumExpanded;
  MI.eraseFromParent();
  return true;
}

bool AArch64SAVIOExpandImpl::run(MachineFunction &MF) {
  const AArch64Subtarget &STI = MF.getSubtarget<AArch64Subtarget>();
  TII = STI.getInstrInfo();
  MRI = &MF.getRegInfo();

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF)
    for (MachineInstr &MI : llvm::make_early_inc_range(MBB))
      Changed |= expand(MI);
  return Changed;
}

PreservedAnalyses
AArch64SAVIOExpandPass::run(MachineFunction &MF,
                            MachineFunctionAnalysisManager &MFAM) {
  if (!AArch64SAVIOExpandImpl().run(MF))
    return PreservedAnalyses::all();
  PreservedAnalyses PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

FunctionPass *llvm::createAArch64SAVIOExpandPass() {
  return new AArch64SAVIOExpandLegacy();
}
