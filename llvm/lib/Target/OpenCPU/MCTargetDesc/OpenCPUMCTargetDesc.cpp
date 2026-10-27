// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "../OpenCPU.h"
#include "../TargetInfo/OpenCPUTargetInfo.h"
#include "llvm/MC/MCAsmInfoELF.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
using namespace llvm;
#define GET_INSTRINFO_MC_DESC
#include "OpenCPUGenInstrInfo.inc"
#define GET_REGINFO_MC_DESC
#include "OpenCPUGenRegisterInfo.inc"
#define GET_SUBTARGETINFO_MC_DESC
#include "OpenCPUGenSubtargetInfo.inc"
namespace {
class OpenCPUAsmInfo : public MCAsmInfoELF {
public:
  explicit OpenCPUAsmInfo(const Triple &TT, const MCTargetOptions &Options) : MCAsmInfoELF(Options) {
    ExceptionsType = TT.getDefaultExceptionHandling();
    CodePointerSize = 2;
    CalleeSaveStackSlotSize = 2;
    CommentString = ";";
    MinInstAlignment = 2;
    MaxInstLength = 4;
    SupportsDebugInformation = false;
  }
};
class OpenCPUInstPrinter : public MCInstPrinter {
  void operand(const MCOperand &Op, raw_ostream &OS) {
    if (Op.isReg()) printRegName(OS, Op.getReg());
    else if (Op.isImm()) OS << Op.getImm();
    else if (Op.isExpr()) MAI.printExpr(OS, *Op.getExpr());
    else report_fatal_error("OpenCPU: unsupported MC operand", false);
  }
public:
  using MCInstPrinter::MCInstPrinter;
  std::pair<const char *, uint64_t> getMnemonic(const MCInst &I) const override {
    switch (I.getOpcode()) {
    case OpenCPU::ADD: return {"add", 0}; case OpenCPU::SUB: return {"sub", 0};
    case OpenCPU::MUL: return {"mul", 0}; case OpenCPU::AND: return {"and", 0};
    case OpenCPU::OR: return {"or", 0}; case OpenCPU::XOR: return {"xor", 0};
    case OpenCPU::SHL: return {"sll", 0}; case OpenCPU::SRL: return {"srl", 0};
    case OpenCPU::LI: return {"li", 0}; case OpenCPU::MOV: return {"mov", 0};
    case OpenCPU::ADDI: return {"addi", 0}; case OpenCPU::LOAD: return {"lw", 0};
    case OpenCPU::STORE: return {"sw", 0}; case OpenCPU::J: return {"j", 0};
    case OpenCPU::CALL: return {"call", 0}; case OpenCPU::RET: return {"ret", 0};
    default: report_fatal_error("OpenCPU: target pseudo requires flat assembly lowering", false);
    }
  }
  void printRegName(raw_ostream &OS, MCRegister Reg) override { OS << StringRef(MRI.getName(Reg)).lower(); }
  void printInst(const MCInst *I, uint64_t, StringRef Annotation, const MCSubtargetInfo &, raw_ostream &OS) override {
    OS << getMnemonic(*I).first;
    unsigned Op = I->getOpcode();
    if (Op == OpenCPU::RET) return;
    OS << ' '; operand(I->getOperand(0), OS);
    if (Op == OpenCPU::J || Op == OpenCPU::CALL) return;
    OS << ", ";
    if (Op == OpenCPU::LOAD || Op == OpenCPU::STORE) {
      operand(I->getOperand(2), OS); OS << '('; operand(I->getOperand(1), OS); OS << ')';
    } else if (Op == OpenCPU::LI || Op == OpenCPU::MOV) operand(I->getOperand(1), OS);
    else operand(I->getOperand(2), OS);
    printAnnotation(OS, Annotation);
  }
};}
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeOpenCPUTargetMC() {
  Target &T = getTheOpenCPUTarget();
  TargetRegistry::RegisterMCAsmInfo(T, [](const MCRegisterInfo &, const Triple &TT, const MCTargetOptions &Options) -> MCAsmInfo * { return new OpenCPUAsmInfo(TT, Options); });
  TargetRegistry::RegisterMCInstrInfo(T, []() { auto *I = new MCInstrInfo; InitOpenCPUMCInstrInfo(I); return I; });
  TargetRegistry::RegisterMCRegInfo(T, [](const Triple &) { auto *I = new MCRegisterInfo; InitOpenCPUMCRegisterInfo(I, OpenCPU::RA); return I; });
  TargetRegistry::RegisterMCSubtargetInfo(T, [](const Triple &TT, StringRef CPU, StringRef FS) { if (CPU.empty()) CPU = "generic"; return createOpenCPUMCSubtargetInfoImpl(TT, CPU, CPU, FS); });
  TargetRegistry::RegisterMCInstPrinter(T, [](const Triple &, unsigned, const MCAsmInfo &MAI, const MCInstrInfo &MII, const MCRegisterInfo &MRI) -> MCInstPrinter * { return new OpenCPUInstPrinter(MAI, MII, MRI); });
}
