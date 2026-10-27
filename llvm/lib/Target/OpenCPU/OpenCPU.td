// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
include "llvm/Target/Target.td"
class OCReg<string name, bits<16> encoding> : Register<name> {
  let Namespace = "OpenCPU";
  let HWEncoding = encoding;
}
def ZERO : OCReg<"zero", 0> { let isConstant = true; }
def A1 : OCReg<"a1", 1>;
def A2 : OCReg<"a2", 2>;
def A3 : OCReg<"a3", 3>;
def A4 : OCReg<"a4", 4>;
def FP : OCReg<"fp", 5>;
def RA : OCReg<"ra", 6>;
def SP : OCReg<"sp", 7>;
def GPR : RegisterClass<"OpenCPU", [i16], 16, (add A1, A2, A3, A4, ZERO, FP, RA, SP)>;
def : Processor<"generic", NoItineraries, []>;
def : Processor<"opencpu", NoItineraries, []>;
def brtarget : Operand<OtherVT>;
class OCInst<dag outs, dag ins, string asm, list<dag> patterns = []> : Instruction {
  let Namespace = "OpenCPU";
  let hasNoSchedulingInfo = 1;
  let OutOperandList = outs;
  let InOperandList = ins;
  let AsmString = asm;
  let Pattern = patterns;
  let Size = 2;
  let hasSideEffects = 0;
  let mayLoad = 0;
  let mayStore = 0;
}
class Binary<SDNode op, string asm> : OCInst<(outs GPR:$dst), (ins GPR:$lhs, GPR:$rhs),
                                          asm # " $dst, $rhs", [(set GPR:$dst, (op GPR:$lhs, GPR:$rhs))]> {
  let Constraints = "$dst = $lhs";
}
def ADD : Binary<add, "add">;
def SUB : Binary<sub, "sub">;
def MUL : Binary<mul, "mul">;
def AND : Binary<and, "and">;
def OR : Binary<or, "or">;
def XOR : Binary<xor, "xor">;
def SHL : Binary<shl, "sll">;
def SRL : Binary<srl, "srl">;
def LI : OCInst<(outs GPR:$dst), (ins i16imm:$value), "li $dst, $value",
                [(set GPR:$dst, imm:$value)]> {
  let isReMaterializable = 1;
  let isAsCheapAsAMove = 1;
}
def FRAMEADDR : OCInst<(outs GPR:$dst), (ins GPR:$base, i16imm:$offset), ""> {
  let isPseudo = 1;
}
def CHECKSP : OCInst<(outs), (ins i16imm:$limit), ""> {
  let isPseudo = 1;
  let hasSideEffects = 1;
  let Uses = [SP];
  let Defs = [SP];
}
def ADJCALLSTACKDOWN : OCInst<(outs), (ins i16imm:$amt, i16imm:$unused), ""> {
  let isPseudo = 1;
  let hasSideEffects = 1;
  let Uses = [SP];
  let Defs = [SP];
}
def ADJCALLSTACKUP : OCInst<(outs), (ins i16imm:$amt, i16imm:$unused), ""> {
  let isPseudo = 1;
  let hasSideEffects = 1;
  let Uses = [SP];
  let Defs = [SP];
}
def MOV : OCInst<(outs GPR:$dst), (ins GPR:$src), "mov $dst, $src">;
def ADDI : OCInst<(outs GPR:$dst), (ins GPR:$src, i16imm:$value), "addi $dst, $value"> {
  let Constraints = "$dst = $src";
}
def LOAD : OCInst<(outs GPR:$dst), (ins GPR:$base, i16imm:$offset), "lw $dst, $offset($base)"> {
  let mayLoad = 1;
}
def STORE : OCInst<(outs), (ins GPR:$src, GPR:$base, i16imm:$offset), "sw $src, $offset($base)"> {
  let mayStore = 1;
}
def J : OCInst<(outs), (ins brtarget:$target), "j $target", [(br bb:$target)]> {
  let isBranch = 1;
  let isTerminator = 1;
  let isBarrier = 1;
}
def BR_CC : OCInst<(outs), (ins GPR:$lhs, GPR:$rhs, i16imm:$cc, brtarget:$target), ""> {
  let isBranch = 1;
  let isTerminator = 1;
}
def SELECT_CC : OCInst<(outs GPR:$dst),
  (ins GPR:$lhs, GPR:$rhs, i16imm:$cc, GPR:$trueval, GPR:$falseval), ""> {
  let isPseudo = 1;
  let usesCustomInserter = 1;
}
def CALL : OCInst<(outs), (ins i16imm:$callee, variable_ops), "call $callee"> {
  let isCall = 1;
  let hasSideEffects = 1;
  let Defs = [RA];
  let Uses = [SP];
}
def RET : OCInst<(outs), (ins variable_ops), "ret"> {
  let isReturn = 1;
  let isTerminator = 1;
  let isBarrier = 1;
  let Uses = [RA, SP];
}
defm : RemapAllTargetPseudoPointerOperands<GPR>;
def OpenCPUInstrInfo : InstrInfo;
def OpenCPU : Target { let InstructionSet = OpenCPUInstrInfo; }
