; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@DEBRUIJN_IDX64 = internal unnamed_addr constant [64 x i8] c"\00\01\02\07\03\0D\08\13\04\19\0E\1C\09\22\14(\05\11\1A&\0F.\1D0\0A\1F#6\152)9?\06\0C\12\18\1B!'\10%-/\1E518>\0B\17 $,47=\16+3<*;:", align 16
@deBruijnTable64_SEAL = internal unnamed_addr constant [64 x i64] [i64 63, i64 0, i64 58, i64 1, i64 59, i64 47, i64 53, i64 2, i64 60, i64 39, i64 48, i64 27, i64 54, i64 33, i64 42, i64 3, i64 61, i64 51, i64 37, i64 40, i64 49, i64 18, i64 28, i64 20, i64 55, i64 30, i64 34, i64 11, i64 43, i64 14, i64 22, i64 4, i64 62, i64 57, i64 46, i64 52, i64 38, i64 26, i64 32, i64 41, i64 50, i64 36, i64 17, i64 19, i64 29, i64 10, i64 13, i64 21, i64 56, i64 45, i64 25, i64 31, i64 35, i64 16, i64 9, i64 12, i64 44, i64 24, i64 15, i64 8, i64 23, i64 7, i64 6, i64 5], align 16
@MultiplyDeBruijnBitPosition_Psnip = internal unnamed_addr constant [64 x i8] c"\00/\0180\1B\02<91)%\1C\10\03=6:#42*\15,& \1D\17\11\0B\04>.7\1A;($\0F5\223\14+\1F\16\0A-\19'\0E!\13\1E\09\18\0D\12\08\0C\07\06\05?", align 16
@DeBruijnClz32 = internal unnamed_addr constant [32 x i32] [i32 0, i32 9, i32 1, i32 10, i32 13, i32 21, i32 2, i32 29, i32 11, i32 14, i32 16, i32 18, i32 22, i32 25, i32 3, i32 30, i32 8, i32 12, i32 20, i32 28, i32 15, i32 17, i32 24, i32 7, i32 19, i32 27, i32 23, i32 6, i32 26, i32 5, i32 4, i32 31], align 16

; CHECK-LABEL: define i8 @debruijn64_terriberry(i64 %v)
; CHECK: call i64 @llvm.ctlz.i64(i64 %v, i1 false)
; CHECK: xor i64
; CHECK-NOT: load i8, ptr @DEBRUIJN_IDX64
define i8 @debruijn64_terriberry(i64 %v) {
entry:
  %shr = lshr i64 %v, 1
  %or = or i64 %shr, %v
  %shr1 = lshr i64 %or, 2
  %or2 = or i64 %shr1, %or
  %shr3 = lshr i64 %or2, 4
  %or4 = or i64 %shr3, %or2
  %shr5 = lshr i64 %or4, 8
  %or6 = or i64 %shr5, %or4
  %shr7 = lshr i64 %or6, 16
  %or8 = or i64 %shr7, %or6
  %0 = lshr i64 %or8, 33
  %1 = lshr i64 %or8, 1
  %shr11 = or i64 %0, %1
  %2 = mul i64 %shr11, 151050438420815295
  %mul = add i64 %2, 151050438420815295
  %shr12 = lshr i64 %mul, 58
  %arrayidx = getelementptr inbounds nuw i8, ptr @DEBRUIJN_IDX64, i64 %shr12
  %3 = load i8, ptr %arrayidx, align 1
  ret i8 %3
}

; CHECK-LABEL: define i64 @debruijn64_seal(i64 %v)
; CHECK: call i64 @llvm.ctlz.i64(i64 %v, i1 false)
; CHECK: xor i64
; CHECK-NOT: load i64, ptr @deBruijnTable64_SEAL
define i64 @debruijn64_seal(i64 %v) {
entry:
  %shr = lshr i64 %v, 1
  %or = or i64 %shr, %v
  %shr1 = lshr i64 %or, 2
  %or2 = or i64 %shr1, %or
  %shr3 = lshr i64 %or2, 4
  %or4 = or i64 %shr3, %or2
  %shr5 = lshr i64 %or4, 8
  %or6 = or i64 %shr5, %or4
  %shr7 = lshr i64 %or6, 16
  %or8 = or i64 %shr7, %or6
  %shr9 = lshr i64 %or8, 32
  %or10 = or i64 %shr9, %or8
  %shr11 = lshr i64 %or10, 1
  %sub = sub i64 %or10, %shr11
  %mul = mul i64 %sub, 571347909858961602
  %shr12 = lshr i64 %mul, 58
  %arrayidx = getelementptr inbounds nuw [8 x i8], ptr @deBruijnTable64_SEAL, i64 %shr12
  %0 = load i64, ptr %arrayidx, align 8
  ret i64 %0
}

; CHECK-LABEL: define i8 @debruijn64_psnip(i64 %v)
; CHECK: call i64 @llvm.ctlz.i64(i64 %v, i1 false)
; CHECK: xor i64
; CHECK-NOT: load i8, ptr @MultiplyDeBruijnBitPosition_Psnip
define i8 @debruijn64_psnip(i64 %v) {
entry:
  %shr = lshr i64 %v, 1
  %or = or i64 %shr, %v
  %shr1 = lshr i64 %or, 2
  %or2 = or i64 %shr1, %or
  %shr3 = lshr i64 %or2, 4
  %or4 = or i64 %shr3, %or2
  %shr5 = lshr i64 %or4, 8
  %or6 = or i64 %shr5, %or4
  %shr7 = lshr i64 %or6, 16
  %or8 = or i64 %shr7, %or6
  %shr9 = lshr i64 %or8, 32
  %or10 = or i64 %shr9, %or8
  %mul = mul i64 %or10, 285870213051386505
  %shr11 = lshr i64 %mul, 58
  %arrayidx = getelementptr inbounds nuw i8, ptr @MultiplyDeBruijnBitPosition_Psnip, i64 %shr11
  %0 = load i8, ptr %arrayidx, align 1
  ret i8 %0
}

; CHECK-LABEL: define i32 @debruijn32_clz(i32 %v)
; CHECK: call i32 @llvm.ctlz.i32(i32 %v, i1 false)
; CHECK: xor i32
; CHECK-NOT: load i32, ptr @DeBruijnClz32
define i32 @debruijn32_clz(i32 %v) {
entry:
  %shr = lshr i32 %v, 1
  %or = or i32 %shr, %v
  %shr1 = lshr i32 %or, 2
  %or2 = or i32 %shr1, %or
  %shr3 = lshr i32 %or2, 4
  %or4 = or i32 %shr3, %or2
  %shr5 = lshr i32 %or4, 8
  %or6 = or i32 %shr5, %or4
  %shr7 = lshr i32 %or6, 16
  %or8 = or i32 %shr7, %or6
  %mul = mul i32 %or8, 130329821
  %shr9 = lshr i32 %mul, 27
  %idxprom = zext nneg i32 %shr9 to i64
  %arrayidx = getelementptr inbounds nuw [4 x i8], ptr @DeBruijnClz32, i64 %idxprom
  %0 = load i32, ptr %arrayidx, align 4
  ret i32 %0
}
