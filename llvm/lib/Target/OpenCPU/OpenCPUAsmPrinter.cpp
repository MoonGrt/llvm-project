// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Flat-image assembly emission, deliberately separate from ELF/GAS emission.
#include "OpenCPU.h"
#include "TargetInfo/OpenCPUTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include <algorithm>
using namespace llvm;
namespace llvm {
class OpenCPUAsmPrinter : public AsmPrinter {
  uint64_t NextSkip = 0;
  bool PrintedMetadata = false;
  std::string blockLabel(const MachineBasicBlock &BB) const {
    return (Twine(".Locpu") + Twine(BB.getParent()->getFunctionNumber()) + "_" + Twine(BB.getNumber())).str();
  }
  std::string operand(const MachineOperand &MO) const {
    if (MO.isReg()) {
      switch (MO.getReg().id()) {
      case OpenCPU::ZERO: return "zero"; case OpenCPU::A1: return "a1";
      case OpenCPU::A2: return "a2"; case OpenCPU::A3: return "a3";
      case OpenCPU::A4: return "a4"; case OpenCPU::FP: return "fp";
      case OpenCPU::RA: return "ra"; case OpenCPU::SP: return "sp";
      default: reportOpenCPUError("unallocated register reached assembly emission");
      }
    }
    if (MO.isImm()) return std::to_string(MO.getImm());
    if (MO.isMBB()) return blockLabel(*MO.getMBB());
    if (MO.isGlobal()) return MO.getGlobal()->getName().str();
    if (MO.isSymbol()) return MO.getSymbolName();
    reportOpenCPUError("unsupported assembly operand");
  }
  void line(const Twine &Text) { OutStreamer->emitRawText(Text); }
public:
  OpenCPUAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> S) : AsmPrinter(TM, std::move(S)) {}
  StringRef getPassName() const override { return "OpenCPU flat word assembly printer"; }
  // This flat-image ABI has no object sections, DWARF or global data. Emit
  // only final allocated machine instructions through the MC text streamer.
  bool doInitialization(Module &) override { PrintedMetadata = false; return false; }
  bool doFinalization(Module &) override { OutStreamer->finish(); return false; }
  bool runOnMachineFunction(MachineFunction &Function) override {
    MF = &Function;
    if (!PrintedMetadata) {
      line("# opencpu-data-size: " + Twine(getOpenCPUDataEnd(*Function.getFunction().getParent())));
      PrintedMetadata = true;
    }
    line(Function.getName() + ":");
    for (MachineBasicBlock &BB : Function) {
      line(blockLabel(BB) + ":");
      for (MachineInstr &MI : BB) if (!MI.isDebugInstr()) emitInstruction(&MI);
    }
    return false;
  }
  void emitInstruction(const MachineInstr *MI) override;
};
void OpenCPUAsmPrinter::emitInstruction(const MachineInstr *MI) {
  auto O = [&](unsigned I) { return operand(MI->getOperand(I)); };
  StringRef Name;
  switch (MI->getOpcode()) {
  case OpenCPU::ADD: Name = "add"; break; case OpenCPU::SUB: Name = "sub"; break;
  case OpenCPU::MUL: Name = "mul"; break; case OpenCPU::AND: Name = "and"; break;
  case OpenCPU::OR: Name = "or"; break; case OpenCPU::XOR: Name = "xor"; break;
  case OpenCPU::SHL: Name = "sll"; break; case OpenCPU::SRL: Name = "srl"; break;
  case OpenCPU::LI: line("    li " + O(0) + ", " + O(1)); return;
  case OpenCPU::FRAMEADDR: {
    line("    mov " + O(0) + ", " + O(1));
    line("    slli " + O(0) + ", 1");
    int64_t Bytes = MI->getOperand(2).getImm();
    while (Bytes) {
      int64_t Part = std::clamp<int64_t>(Bytes, -128, 127);
      line("    addi " + O(0) + ", " + Twine(Part)); Bytes -= Part;
    }
    return;
  }
  case OpenCPU::CHECKSP: {
    std::string Fail = (Twine(".LocpuStackFail") + Twine(NextSkip++)).str();
    std::string Done = Fail + "Done";
    int64_t Limit = MI->getOperand(0).getImm();
    auto Adjust = [&](int64_t Amount) {
      while (Amount) {
        int64_t Part = std::clamp<int64_t>(Amount, -128, 127);
        line("    addi sp, " + Twine(Part)); Amount -= Part;
      }
    };
    Adjust(-Limit);
    line("    blt sp, zero, " + Fail);
    Adjust(Limit);
    line("    j " + Done);
    line(Fail + ":"); line("    .word 14"); line(Done + ":");
    return;
  }
  case OpenCPU::MOV: line("    mov " + O(0) + ", " + O(1)); return;
  case OpenCPU::ADDI: line("    addi " + O(0) + ", " + O(2)); return;
  case OpenCPU::LOAD: line("    lw " + O(0) + ", " + O(2) + "(" + O(1) + ")"); return;
  case OpenCPU::STORE: line("    sw " + O(0) + ", " + O(2) + "(" + O(1) + ")"); return;
  case OpenCPU::J: line("    j " + O(0)); return;
  case OpenCPU::CALL: line("    call " + O(0)); return;
  case OpenCPU::RET: line("    ret"); return;
  case TargetOpcode::IMPLICIT_DEF: line("    li " + O(0) + ", 0"); return;
  case TargetOpcode::KILL:
  case TargetOpcode::LIFETIME_START:
  case TargetOpcode::LIFETIME_END: return;
  case OpenCPU::BR_CC: {
    auto CC = static_cast<ISD::CondCode>(MI->getOperand(2).getImm());
    std::string A = O(0), B = O(1), Target = O(3);
    if (CC == ISD::SETEQ) { line("    beq " + A + ", " + B + ", " + Target); return; }
    if (CC == ISD::SETLT || CC == ISD::SETGT) {
      if (CC == ISD::SETGT) std::swap(A, B);
      line("    blt " + A + ", " + B + ", " + Target); return;
    }
    std::string Skip = (Twine(".LocpuSkip") + Twine(NextSkip++)).str();
    if (CC == ISD::SETNE) line("    beq " + A + ", " + B + ", " + Skip);
    else if (CC == ISD::SETGE) line("    blt " + A + ", " + B + ", " + Skip);
    else if (CC == ISD::SETLE) line("    blt " + B + ", " + A + ", " + Skip);
    else reportOpenCPUError("unsupported branch comparison reached emission");
    line("    j " + Target); line(Skip + ":"); return;
  }
  default: reportOpenCPUError(Twine("unexpanded machine instruction: ") + Twine(MI->getOpcode()));
  }
  line("    " + Name + " " + O(0) + ", " + O(2));
}

} // namespace llvm

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeOpenCPUAsmPrinter() {
  RegisterAsmPrinter<OpenCPUAsmPrinter> X(getTheOpenCPUTarget());
}
