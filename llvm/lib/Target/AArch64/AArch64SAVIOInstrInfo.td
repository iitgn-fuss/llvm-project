//=- AArch64SAVIOInstrInfo.td - SAVIO Singleton Vectorize pseudos -*- tablegen -*-=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pseudo-instructions for the llvm.savio.sv.*.v4i32 intrinsics. Lane 0 of the
// result is the scalar operation on lane 0 of the operands; the other lanes are
// unspecified. AArch64SAVIOExpand rewrites each pseudo into real NEON/FP
// instructions in SSA form, before register allocation.
//
//===----------------------------------------------------------------------===//

// Constant shift counts: the splat operand is selected as MOVI #n, with n a
// valid immediate-shift amount. Zero and counts of 32 or more (poison for the
// scalar operation) take the variable-count pseudo instead.
def savio_shift_imm : ImmLeaf<i32, [{ return Imm >= 1 && Imm <= 31; }]>;

let isPseudo = 1, hasSideEffects = 0, hasNoSchedulingInfo = 1 in {
  def SAVIO_SV_ADD_V4I32  : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;
  def SAVIO_SV_SUB_V4I32  : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;
  def SAVIO_SV_MUL_V4I32  : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;
  def SAVIO_SV_AND_V4I32  : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;
  def SAVIO_SV_OR_V4I32   : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;
  def SAVIO_SV_XOR_V4I32  : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;
  def SAVIO_SV_SHL_V4I32  : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;
  def SAVIO_SV_LSHR_V4I32 : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;
  def SAVIO_SV_ASHR_V4I32 : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;
  def SAVIO_SV_UDIV_V4I32 : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;
  def SAVIO_SV_UREM_V4I32 : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;

  // Lanes 0 and 1 of an unsigned divide/remainder through fdiv .2d.
  def SAVIO_FPDIV2_UDIV_V4I32 : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;
  def SAVIO_FPDIV2_UREM_V4I32 : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;

  // All four lanes through two fdiv .2d.
  def SAVIO_FPDIV4_UDIV_V4I32 : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;
  def SAVIO_FPDIV4_UREM_V4I32 : Pseudo<(outs V128:$dst), (ins V128:$a, V128:$b), []>;

  // Shifts by a constant count (1..31).
  def SAVIO_SV_SHL_IMM_V4I32  : Pseudo<(outs V128:$dst), (ins V128:$a, i32imm:$n), []>;
  def SAVIO_SV_LSHR_IMM_V4I32 : Pseudo<(outs V128:$dst), (ins V128:$a, i32imm:$n), []>;
  def SAVIO_SV_ASHR_IMM_V4I32 : Pseudo<(outs V128:$dst), (ins V128:$a, i32imm:$n), []>;
}

multiclass SAVIOSVBinary<string Name, Intrinsic Int> {
  def : Pat<(v4i32 (Int (v4i32 V128:$a), (v4i32 V128:$b))),
            (!cast<Instruction>("SAVIO_SV_" # Name # "_V4I32") V128:$a, V128:$b)>;
}

defm : SAVIOSVBinary<"ADD", int_savio_sv_add_v4i32>;
defm : SAVIOSVBinary<"SUB", int_savio_sv_sub_v4i32>;
defm : SAVIOSVBinary<"MUL", int_savio_sv_mul_v4i32>;
defm : SAVIOSVBinary<"AND", int_savio_sv_and_v4i32>;
defm : SAVIOSVBinary<"OR", int_savio_sv_or_v4i32>;
defm : SAVIOSVBinary<"XOR", int_savio_sv_xor_v4i32>;
defm : SAVIOSVBinary<"SHL", int_savio_sv_shl_v4i32>;
defm : SAVIOSVBinary<"LSHR", int_savio_sv_lshr_v4i32>;
defm : SAVIOSVBinary<"ASHR", int_savio_sv_ashr_v4i32>;
defm : SAVIOSVBinary<"UDIV", int_savio_sv_udiv_v4i32>;
defm : SAVIOSVBinary<"UREM", int_savio_sv_urem_v4i32>;

multiclass SAVIOSVShiftImm<string Name, Intrinsic Int> {
  def : Pat<(v4i32 (Int (v4i32 V128:$a),
                        (v4i32 (AArch64movi_shift (i32 savio_shift_imm:$n),
                                                  (i32 0))))),
            (!cast<Instruction>("SAVIO_SV_" # Name # "_IMM_V4I32") V128:$a,
                                                                   imm:$n)>;
}

defm : SAVIOSVShiftImm<"SHL", int_savio_sv_shl_v4i32>;
defm : SAVIOSVShiftImm<"LSHR", int_savio_sv_lshr_v4i32>;
defm : SAVIOSVShiftImm<"ASHR", int_savio_sv_ashr_v4i32>;

def : Pat<(v4i32 (int_savio_fpdiv2_udiv_v4i32 (v4i32 V128:$a), (v4i32 V128:$b))),
          (SAVIO_FPDIV2_UDIV_V4I32 V128:$a, V128:$b)>;
def : Pat<(v4i32 (int_savio_fpdiv2_urem_v4i32 (v4i32 V128:$a), (v4i32 V128:$b))),
          (SAVIO_FPDIV2_UREM_V4I32 V128:$a, V128:$b)>;

def : Pat<(v4i32 (int_savio_fpdiv4_udiv_v4i32 (v4i32 V128:$a), (v4i32 V128:$b))),
          (SAVIO_FPDIV4_UDIV_V4I32 V128:$a, V128:$b)>;
def : Pat<(v4i32 (int_savio_fpdiv4_urem_v4i32 (v4i32 V128:$a), (v4i32 V128:$b))),
          (SAVIO_FPDIV4_UREM_V4I32 V128:$a, V128:$b)>;
