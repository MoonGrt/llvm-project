// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// OpenCPU ISA v2 native LLVM code generator. The initial ABI deliberately
// permits only non-escaping, aligned i16 frame objects; it does not pretend
// that the machine's word pointers are arbitrary LLVM byte pointers.
#include "OpenCPU.h"
#include "TargetInfo/OpenCPUTargetInfo.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/SelectionDAGTargetInfo.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Debug.h"
#define DEBUG_TYPE "opencpu"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
#include <cctype>

#define GET_REGINFO_HEADER
#include "OpenCPUGenRegisterInfo.inc"
#define GET_INSTRINFO_HEADER
#include "OpenCPUGenInstrInfo.inc"
#define GET_SUBTARGETINFO_HEADER
#include "OpenCPUGenSubtargetInfo.inc"

using namespace llvm;

namespace llvm {
void initializeOpenCPUDAGToDAGISelLegacyPass(PassRegistry &);
}

namespace llvm {
[[noreturn]] void reportOpenCPUError(const Twine &Message) {
  report_fatal_error(Twine("OpenCPU: ") + Message, false);
}
constexpr const char *Layout = "e-p:16:16-i16:16-n16-S16";
constexpr MCPhysReg ArgRegs[] = {OpenCPU::A1, OpenCPU::A2, OpenCPU::A3, OpenCPU::A4};

class OpenCPUTargetMachine;
class OpenCPUSubtarget;

class OpenCPURegisterInfo : public OpenCPUGenRegisterInfo {
public:
  OpenCPURegisterInfo() : OpenCPUGenRegisterInfo(OpenCPU::RA) {}
  const MCPhysReg *getCalleeSavedRegs(const MachineFunction *) const override {
    // FP/RA are saved explicitly by our invariant prologue, not PEI's optional
    // callee-saved register discovery.
    static const MCPhysReg None[] = {0};
    return None;
  }
  const uint32_t *getCallPreservedMask(const MachineFunction &, CallingConv::ID) const override {
    static const uint32_t Mask[] = {(1u << OpenCPU::ZERO) | (1u << OpenCPU::FP) | (1u << OpenCPU::SP)};
    return Mask;
  }
  BitVector getReservedRegs(const MachineFunction &) const override {
    BitVector Bits(getNumRegs());
    for (unsigned R : {OpenCPU::ZERO, OpenCPU::FP, OpenCPU::RA, OpenCPU::SP}) Bits.set(R);
    return Bits;
  }
  const TargetRegisterClass *getPointerRegClass(unsigned = 0) const override { return &OpenCPU::GPRRegClass; }
  Register getFrameRegister(const MachineFunction &) const override { return OpenCPU::FP; }
  bool eliminateFrameIndex(MachineBasicBlock::iterator, int, unsigned, RegScavenger *) const override;
};

class OpenCPUInstrInfo : public OpenCPUGenInstrInfo {
  OpenCPURegisterInfo RI;
public:
  explicit OpenCPUInstrInfo(const TargetSubtargetInfo &ST) : OpenCPUGenInstrInfo(ST, RI) {}
  const OpenCPURegisterInfo &getRegisterInfo() const { return RI; }
  void copyPhysReg(MachineBasicBlock &, MachineBasicBlock::iterator, const DebugLoc &,
                   Register, Register, bool, bool = false, bool = false) const override;
  void storeRegToStackSlot(MachineBasicBlock &, MachineBasicBlock::iterator, Register,
                          bool, int, const TargetRegisterClass *, Register,
                          MachineInstr::MIFlag = MachineInstr::NoFlags) const override;
  void loadRegFromStackSlot(MachineBasicBlock &, MachineBasicBlock::iterator, Register,
                           int, const TargetRegisterClass *, Register, unsigned = 0,
                           MachineInstr::MIFlag = MachineInstr::NoFlags) const override;
  bool analyzeBranch(MachineBasicBlock &, MachineBasicBlock *&, MachineBasicBlock *&,
                     SmallVectorImpl<MachineOperand> &, bool) const override;
  unsigned removeBranch(MachineBasicBlock &, int * = nullptr) const override;
  unsigned insertBranch(MachineBasicBlock &, MachineBasicBlock *, MachineBasicBlock *,
                        ArrayRef<MachineOperand>, const DebugLoc &, int * = nullptr) const override;
  bool reverseBranchCondition(SmallVectorImpl<MachineOperand> &) const override;
};

class OpenCPUFrameLowering : public TargetFrameLowering {
protected:
  bool hasFPImpl(const MachineFunction &) const override { return true; }
public:
  OpenCPUFrameLowering() : TargetFrameLowering(StackGrowsDown, Align(2), 0, Align(2)) {}
  void emitPrologue(MachineFunction &, MachineBasicBlock &) const override;
  void emitEpilogue(MachineFunction &, MachineBasicBlock &) const override;
  bool hasReservedCallFrame(const MachineFunction &) const override { return true; }
};

namespace OCISD {
enum { CALL = ISD::BUILTIN_OP_END, RET, BR_CC, SELECT_CC };
}
class OpenCPUDAGInfo : public SelectionDAGTargetInfo {
public:
  const char *getTargetNodeName(unsigned Op) const override {
    switch (Op) {
    case OCISD::CALL: return "OpenCPU::CALL";
    case OCISD::RET: return "OpenCPU::RET";
    case OCISD::BR_CC: return "OpenCPU::BR_CC";
    case OCISD::SELECT_CC: return "OpenCPU::SELECT_CC";
    default: return nullptr;
    }
  }
};

class OpenCPUTargetLowering : public TargetLowering {
public:
  OpenCPUTargetLowering(const TargetMachine &, const OpenCPUSubtarget &);
  SDValue LowerOperation(SDValue, SelectionDAG &) const override;
  EVT getSetCCResultType(const DataLayout &, LLVMContext &, EVT) const override { return MVT::i16; }
  MVT getScalarShiftAmountTy(const DataLayout &, EVT) const override { return MVT::i16; }
  bool shouldReduceLoadWidth(SDNode *, ISD::LoadExtType, EVT,
      std::optional<unsigned> = std::nullopt) const override { return false; }
  SDValue LowerFormalArguments(SDValue, CallingConv::ID, bool,
      const SmallVectorImpl<ISD::InputArg> &, const SDLoc &, SelectionDAG &,
      SmallVectorImpl<SDValue> &) const override;
  SDValue LowerReturn(SDValue, CallingConv::ID, bool,
      const SmallVectorImpl<ISD::OutputArg> &, const SmallVectorImpl<SDValue> &,
      const SDLoc &, SelectionDAG &) const override;
  SDValue LowerCall(CallLoweringInfo &, SmallVectorImpl<SDValue> &) const override;
  MachineBasicBlock *EmitInstrWithCustomInserter(MachineInstr &, MachineBasicBlock *) const override;
};

class OpenCPUSubtarget : public OpenCPUGenSubtargetInfo {
  OpenCPUInstrInfo InstrInfo;
  OpenCPUFrameLowering Frame;
  OpenCPUTargetLowering Lowering;
  OpenCPUDAGInfo DAGInfo;
public:
  OpenCPUSubtarget(const Triple &TT, StringRef CPU, StringRef FS, const TargetMachine &TM)
      : OpenCPUGenSubtargetInfo(TT, CPU.empty() ? "generic" : CPU, CPU.empty() ? "generic" : CPU, FS),
        InstrInfo(*this), Lowering(TM, *this) {}
  void ParseSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS);
  const OpenCPUInstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const OpenCPURegisterInfo *getRegisterInfo() const override { return &InstrInfo.getRegisterInfo(); }
  const OpenCPUFrameLowering *getFrameLowering() const override { return &Frame; }
  const OpenCPUTargetLowering *getTargetLowering() const override { return &Lowering; }
  const SelectionDAGTargetInfo *getSelectionDAGInfo() const override { return &DAGInfo; }
};

class OpenCPUTargetMachine : public CodeGenTargetMachineImpl {
  std::unique_ptr<TargetLoweringObjectFile> ObjectFile;
  OpenCPUSubtarget Subtarget;
public:
  OpenCPUTargetMachine(const Target &T, const Triple &TT, StringRef CPU, StringRef FS,
      const TargetOptions &Options, std::optional<Reloc::Model> RM,
      std::optional<CodeModel::Model> CM, CodeGenOptLevel OL, bool)
      : CodeGenTargetMachineImpl(T, Layout, TT, CPU, FS, Options,
                                 RM.value_or(Reloc::Static), CM.value_or(CodeModel::Small), OL),
        ObjectFile(std::make_unique<TargetLoweringObjectFileELF>()),
        Subtarget(TT, CPU, FS, *this) { initAsmInfo(); }
  const OpenCPUSubtarget *getSubtargetImpl(const Function &) const override { return &Subtarget; }
  TargetLoweringObjectFile *getObjFileLowering() const override { return ObjectFile.get(); }
  TargetPassConfig *createPassConfig(PassManagerBase &) override;
};

void adjustRegister(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                    const DebugLoc &DL, const TargetInstrInfo &TII,
                    Register Reg, int64_t Words) {
  while (Words) {
    int64_t Part = std::clamp<int64_t>(Words, -128, 127);
    BuildMI(MBB, I, DL, TII.get(OpenCPU::ADDI), Reg).addReg(Reg).addImm(Part);
    Words -= Part;
  }
}

void OpenCPUInstrInfo::copyPhysReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
    const DebugLoc &DL, Register Dst, Register Src, bool Kill, bool, bool) const {
  BuildMI(MBB, I, DL, get(OpenCPU::MOV), Dst).addReg(Src, getKillRegState(Kill));
}
void OpenCPUInstrInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
    MachineBasicBlock::iterator I, Register Src, bool Kill, int FI,
    const TargetRegisterClass *, Register, MachineInstr::MIFlag Flags) const {
  auto &MF = *MBB.getParent();
  auto *MMO = MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(MF, FI),
                                    MachineMemOperand::MOStore, 2, Align(2));
  BuildMI(MBB, I, DebugLoc(), get(OpenCPU::STORE))
      .addReg(Src, getKillRegState(Kill)).addFrameIndex(FI).addImm(0).addMemOperand(MMO).setMIFlag(Flags);
}
void OpenCPUInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
    MachineBasicBlock::iterator I, Register Dst, int FI, const TargetRegisterClass *,
    Register, unsigned SubReg, MachineInstr::MIFlag Flags) const {
  if (SubReg) reportOpenCPUError("subregister reload is unsupported");
  auto &MF = *MBB.getParent();
  auto *MMO = MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(MF, FI),
                                    MachineMemOperand::MOLoad, 2, Align(2));
  BuildMI(MBB, I, DebugLoc(), get(OpenCPU::LOAD), Dst)
      .addFrameIndex(FI).addImm(0).addMemOperand(MMO).setMIFlag(Flags);
}

bool OpenCPURegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator I,
    int SPAdj, unsigned FIOp, RegScavenger *) const {
  if (SPAdj) reportOpenCPUError("dynamic stack adjustment is unsupported");
  MachineInstr &MI = *I;
  if (MI.getOpcode() == TargetOpcode::LIFETIME_START || MI.getOpcode() == TargetOpcode::LIFETIME_END) {
    MI.eraseFromParent();
    return true;
  }
  MachineBasicBlock &MBB = *MI.getParent();
  const MachineFrameInfo &MFI = MBB.getParent()->getFrameInfo();
  int FI = MI.getOperand(FIOp).getIndex();
  // FP is at the bottom of the allocated frame. Two extra words contain
  // the caller FP/RA; LLVM's object offsets remain standard byte offsets.
  int64_t Bytes = MFI.getObjectOffset(FI) + MFI.getStackSize() + 4 + MI.getOperand(FIOp + 1).getImm();
  if (Bytes % 2) reportOpenCPUError("unaligned frame offset");
  int64_t Words = Bytes / 2;
  MI.getOperand(FIOp).ChangeToRegister(OpenCPU::FP, false);
  const auto &TII = *MBB.getParent()->getSubtarget().getInstrInfo();
  if (isInt<5>(Words)) {
    MI.getOperand(FIOp + 1).ChangeToImmediate(Words);
  } else {
    // FI elimination is after register allocation. Adjust the reserved FP,
    // never invent a virtual scratch or clobber one of four argument regs.
    MI.getOperand(FIOp + 1).ChangeToImmediate(0);
    adjustRegister(MBB, I, MI.getDebugLoc(), TII, OpenCPU::FP, Words);
    adjustRegister(MBB, std::next(I), MI.getDebugLoc(), TII, OpenCPU::FP, -Words);
  }
  return false;
}

void OpenCPUFrameLowering::emitPrologue(MachineFunction &MF, MachineBasicBlock &MBB) const {
  const auto &TII = *MF.getSubtarget().getInstrInfo();
  auto I = MBB.begin();
  DebugLoc DL;
  uint64_t Bytes = MF.getFrameInfo().getStackSize();
  if (Bytes % 2 || Bytes > 2040) reportOpenCPUError("static stack frame exceeds the 1024-word RAM or is unaligned");
  adjustRegister(MBB, I, DL, TII, OpenCPU::SP, -int64_t(Bytes / 2 + 2));
  BuildMI(MBB, I, DL, TII.get(OpenCPU::STORE)).addReg(OpenCPU::FP).addReg(OpenCPU::SP).addImm(0);
  BuildMI(MBB, I, DL, TII.get(OpenCPU::STORE)).addReg(OpenCPU::RA).addReg(OpenCPU::SP).addImm(1);
  BuildMI(MBB, I, DL, TII.get(OpenCPU::MOV), OpenCPU::FP).addReg(OpenCPU::SP);
}
void OpenCPUFrameLowering::emitEpilogue(MachineFunction &MF, MachineBasicBlock &MBB) const {
  const auto &TII = *MF.getSubtarget().getInstrInfo();
  auto I = MBB.getLastNonDebugInstr();
  DebugLoc DL = I->getDebugLoc();
  BuildMI(MBB, I, DL, TII.get(OpenCPU::MOV), OpenCPU::SP).addReg(OpenCPU::FP);
  BuildMI(MBB, I, DL, TII.get(OpenCPU::LOAD), OpenCPU::RA).addReg(OpenCPU::SP).addImm(1);
  BuildMI(MBB, I, DL, TII.get(OpenCPU::LOAD), OpenCPU::FP).addReg(OpenCPU::SP).addImm(0);
  adjustRegister(MBB, I, DL, TII, OpenCPU::SP, MF.getFrameInfo().getStackSize() / 2 + 2);
}

bool OpenCPUInstrInfo::analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
    MachineBasicBlock *&FBB, SmallVectorImpl<MachineOperand> &Cond, bool) const {
  TBB = FBB = nullptr;
  Cond.clear();
  for (auto I = MBB.getFirstTerminator(), E = MBB.end(); I != E; ++I) {
    if (I->isDebugInstr()) continue;
    if (I->getOpcode() == OpenCPU::J) {
      if (!TBB) TBB = I->getOperand(0).getMBB();
      else if (!FBB) FBB = I->getOperand(0).getMBB();
      else return true;
    } else if (I->getOpcode() == OpenCPU::BR_CC && !TBB) {
      TBB = I->getOperand(3).getMBB();
      Cond.push_back(I->getOperand(2));
      Cond.push_back(I->getOperand(0));
      Cond.push_back(I->getOperand(1));
    } else return true;
  }
  return false;
}
unsigned OpenCPUInstrInfo::removeBranch(MachineBasicBlock &MBB, int *Bytes) const {
  unsigned N = 0;
  while (!MBB.empty()) {
    auto I = MBB.getLastNonDebugInstr();
    if (I == MBB.end() || (I->getOpcode() != OpenCPU::J && I->getOpcode() != OpenCPU::BR_CC)) break;
    I->eraseFromParent(); ++N;
  }
  if (Bytes) *Bytes = N * 2;
  return N;
}
unsigned OpenCPUInstrInfo::insertBranch(MachineBasicBlock &MBB, MachineBasicBlock *TBB,
    MachineBasicBlock *FBB, ArrayRef<MachineOperand> Cond, const DebugLoc &DL, int *Bytes) const {
  unsigned N = 1;
  if (Cond.empty()) BuildMI(MBB, MBB.end(), DL, get(OpenCPU::J)).addMBB(TBB);
  else {
    BuildMI(MBB, MBB.end(), DL, get(OpenCPU::BR_CC)).add(Cond[1]).add(Cond[2]).add(Cond[0]).addMBB(TBB);
    if (FBB) { BuildMI(MBB, MBB.end(), DL, get(OpenCPU::J)).addMBB(FBB); ++N; }
  }
  if (Bytes) *Bytes = N * 2;
  return N;
}
bool OpenCPUInstrInfo::reverseBranchCondition(SmallVectorImpl<MachineOperand> &Cond) const {
  if (Cond.size() != 3) return true;
  Cond[0].setImm(ISD::getSetCCInverse(static_cast<ISD::CondCode>(Cond[0].getImm()), MVT::i16));
  return false;
}

OpenCPUTargetLowering::OpenCPUTargetLowering(const TargetMachine &TM, const OpenCPUSubtarget &ST)
    : TargetLowering(TM, ST) {
  addRegisterClass(MVT::i16, &OpenCPU::GPRRegClass);
  computeRegisterProperties(ST.getRegisterInfo());
  setStackPointerRegisterToSaveRestore(OpenCPU::SP);
  setBooleanContents(ZeroOrOneBooleanContent);
  for (MVT MemVT : {MVT::i1, MVT::i8}) {
    setLoadExtAction({ISD::EXTLOAD, ISD::ZEXTLOAD, ISD::SEXTLOAD}, MVT::i16, MemVT, Expand);
    setTruncStoreAction(MVT::i16, MemVT, Expand);
  }
  setOperationAction(ISD::BRCOND, MVT::Other, Expand);
  setOperationAction(ISD::BR_CC, MVT::i16, Custom);
  setOperationAction(ISD::SETCC, MVT::i16, Custom);
  setOperationAction(ISD::SELECT, MVT::i16, Expand);
  setOperationAction(ISD::SELECT_CC, MVT::i16, Custom);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i16, Custom);
  setOperationAction(ISD::SRA, MVT::i16, Custom);
  setOperationAction(ISD::BR_JT, MVT::Other, Expand);
  // Default-legal operations are unsafe on a small target: DAGCombine may
  // create min/max, abs, rotates and bit-count nodes from permitted IR even
  // when the user never writes an intrinsic. Keep them expanded into the
  // primitive i16 ISA instead of reaching CannotSelect or a runtime libcall.
  for (unsigned Op : {ISD::SMIN, ISD::SMAX, ISD::UMIN, ISD::UMAX, ISD::ABS, ISD::ABS_MIN_POISON,
                      ISD::BSWAP, ISD::BITREVERSE, ISD::CTPOP, ISD::CTLZ,
                      ISD::CTTZ, ISD::CTLZ_ZERO_POISON, ISD::CTTZ_ZERO_POISON,
                      ISD::ROTL, ISD::ROTR, ISD::FSHL, ISD::FSHR,
                      ISD::MULHU, ISD::MULHS, ISD::UADDO, ISD::SADDO,
                      ISD::USUBO, ISD::SSUBO, ISD::ADDC, ISD::ADDE,
                      ISD::SUBC, ISD::SUBE, ISD::SADDSAT, ISD::UADDSAT,
                      ISD::SSUBSAT, ISD::USUBSAT})
    setOperationAction(Op, MVT::i16, Expand);
  setMinFunctionAlignment(Align(2));
  setPrefFunctionAlignment(Align(2));
}

static ISD::CondCode normalizeComparison(SDValue &LHS, SDValue &RHS,
    ISD::CondCode CC, const SDLoc &DL, SelectionDAG &DAG) {
  switch (CC) {
  case ISD::SETULT: CC = ISD::SETLT; break;
  case ISD::SETULE: CC = ISD::SETLE; break;
  case ISD::SETUGT: CC = ISD::SETGT; break;
  case ISD::SETUGE: CC = ISD::SETGE; break;
  default: return CC;
  }
  SDValue Sign = DAG.getConstant(0x8000, DL, MVT::i16);
  LHS = DAG.getNode(ISD::XOR, DL, MVT::i16, LHS, Sign);
  RHS = DAG.getNode(ISD::XOR, DL, MVT::i16, RHS, Sign);
  return CC;
}
SDValue OpenCPUTargetLowering::LowerOperation(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  switch (Op.getOpcode()) {
  case ISD::BR_CC: {
    SDValue LHS = Op.getOperand(2), RHS = Op.getOperand(3);
    auto CC = normalizeComparison(LHS, RHS, cast<CondCodeSDNode>(Op.getOperand(1))->get(), DL, DAG);
    return DAG.getNode(OCISD::BR_CC, DL, MVT::Other, Op.getOperand(0), LHS, RHS,
                       DAG.getTargetConstant(CC, DL, MVT::i16), Op.getOperand(4));
  }
  case ISD::SETCC: {
    SDValue LHS = Op.getOperand(0), RHS = Op.getOperand(1);
    auto CC = normalizeComparison(LHS, RHS, cast<CondCodeSDNode>(Op.getOperand(2))->get(), DL, DAG);
    return DAG.getNode(OCISD::SELECT_CC, DL, MVT::i16, LHS, RHS,
       DAG.getTargetConstant(CC, DL, MVT::i16), DAG.getConstant(1, DL, MVT::i16), DAG.getConstant(0, DL, MVT::i16));
  }
  case ISD::SELECT_CC: {
    SDValue LHS = Op.getOperand(0), RHS = Op.getOperand(1);
    auto CC = normalizeComparison(LHS, RHS, cast<CondCodeSDNode>(Op.getOperand(4))->get(), DL, DAG);
    return DAG.getNode(OCISD::SELECT_CC, DL, MVT::i16, LHS, RHS,
                       DAG.getTargetConstant(CC, DL, MVT::i16), Op.getOperand(2), Op.getOperand(3));
  }
  case ISD::SIGN_EXTEND_INREG: {
    EVT From = cast<VTSDNode>(Op.getOperand(1))->getVT();
    if (From != MVT::i1) reportOpenCPUError("unsupported sign extension width");
    SDValue Bit = DAG.getNode(ISD::AND, DL, MVT::i16, Op.getOperand(0), DAG.getConstant(1, DL, MVT::i16));
    return DAG.getNode(ISD::SUB, DL, MVT::i16, DAG.getConstant(0, DL, MVT::i16), Bit);
  }
  case ISD::SRA: {
    // Used internally when LLVM legalizes sext i1. Source IR ashr remains
    // rejected, but legalization must never accidentally call a runtime.
    SDValue V = Op.getOperand(0), N = Op.getOperand(1);
    SDValue Low = DAG.getNode(ISD::SRL, DL, MVT::i16, V, N);
    SDValue Sign = DAG.getNode(OCISD::SELECT_CC, DL, MVT::i16, V,
      DAG.getConstant(0, DL, MVT::i16), DAG.getTargetConstant(ISD::SETLT, DL, MVT::i16),
      DAG.getConstant(0xffff, DL, MVT::i16), DAG.getConstant(0, DL, MVT::i16));
    SDValue Ones = DAG.getConstant(0xffff, DL, MVT::i16);
    SDValue HighMask = DAG.getNode(ISD::XOR, DL, MVT::i16, Ones,
                                  DAG.getNode(ISD::SRL, DL, MVT::i16, Ones, N));
    SDValue High = DAG.getNode(ISD::AND, DL, MVT::i16, Sign, HighMask);
    return DAG.getNode(ISD::OR, DL, MVT::i16, Low, High);
  }
  default: reportOpenCPUError("unsupported SelectionDAG operation");
  }
}

SDValue OpenCPUTargetLowering::LowerFormalArguments(SDValue Chain, CallingConv::ID,
    bool VarArg, const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &Values) const {
  if (VarArg || Ins.size() > 4) reportOpenCPUError("at most four i16 arguments are supported");
  MachineFunction &MF = DAG.getMachineFunction();
  for (unsigned I = 0; I < Ins.size(); ++I) {
    if (Ins[I].VT != MVT::i16) reportOpenCPUError("function arguments must be i16");
    Register VReg = MF.addLiveIn(ArgRegs[I], &OpenCPU::GPRRegClass);
    SDValue V = DAG.getCopyFromReg(Chain, DL, VReg, MVT::i16);
    Values.push_back(V); Chain = V.getValue(1);
  }
  return Chain;
}
SDValue OpenCPUTargetLowering::LowerReturn(SDValue Chain, CallingConv::ID, bool,
    const SmallVectorImpl<ISD::OutputArg> &Outs, const SmallVectorImpl<SDValue> &Values,
    const SDLoc &DL, SelectionDAG &DAG) const {
  SDValue Glue;
  SmallVector<SDValue, 4> Ops;
  if (!Outs.empty()) {
    if (Outs.size() != 1 || Outs[0].VT != MVT::i16) reportOpenCPUError("return value must be i16 or void");
    Chain = DAG.getCopyToReg(Chain, DL, OpenCPU::A1, Values[0], Glue);
    Glue = Chain.getValue(1);
  }
  Ops.push_back(Chain);
  if (!Outs.empty()) Ops.push_back(DAG.getRegister(OpenCPU::A1, MVT::i16));
  if (Glue) Ops.push_back(Glue);
  return DAG.getNode(OCISD::RET, DL, MVT::Other, Ops);
}
SDValue OpenCPUTargetLowering::LowerCall(CallLoweringInfo &CLI,
    SmallVectorImpl<SDValue> &Values) const {
  auto &DAG = CLI.DAG;
  auto &DL = CLI.DL;
  if (CLI.Outs.size() > 4) reportOpenCPUError("at most four call arguments are supported");
  CLI.IsTailCall = false;
  SDValue Chain = CLI.Chain, Glue;
  for (unsigned I = 0; I < CLI.OutVals.size(); ++I) {
    if (CLI.Outs[I].VT != MVT::i16) reportOpenCPUError("call arguments must be i16");
    Chain = DAG.getCopyToReg(Chain, DL, ArgRegs[I], CLI.OutVals[I], Glue);
    Glue = Chain.getValue(1);
  }
  auto *GA = dyn_cast<GlobalAddressSDNode>(CLI.Callee);
  if (!GA || !isa<Function>(GA->getGlobal()) || cast<Function>(GA->getGlobal())->isDeclaration())
    reportOpenCPUError("only direct calls to defined module functions are supported; runtime libcalls are unavailable");
  SDValue Callee = DAG.getTargetGlobalAddress(GA->getGlobal(), DL, MVT::i16);
  SmallVector<SDValue, 10> Ops{Chain, Callee};
  for (unsigned I = 0; I < CLI.OutVals.size(); ++I) Ops.push_back(DAG.getRegister(ArgRegs[I], MVT::i16));
  Ops.push_back(DAG.getRegisterMask(DAG.getSubtarget().getRegisterInfo()->getCallPreservedMask(DAG.getMachineFunction(), CLI.CallConv)));
  if (Glue) Ops.push_back(Glue);
  Chain = DAG.getNode(OCISD::CALL, DL, DAG.getVTList(MVT::Other, MVT::Glue), Ops);
  Glue = Chain.getValue(1);
  if (!CLI.Ins.empty()) {
    if (CLI.Ins.size() != 1 || CLI.Ins[0].VT != MVT::i16) reportOpenCPUError("call return value must be i16 or void");
    SDValue V = DAG.getCopyFromReg(Chain, DL, OpenCPU::A1, MVT::i16, Glue);
    Values.push_back(V); Chain = V.getValue(1);
  }
  return Chain;
}

MachineBasicBlock *OpenCPUTargetLowering::EmitInstrWithCustomInserter(
    MachineInstr &MI, MachineBasicBlock *Head) const {
  if (MI.getOpcode() != OpenCPU::SELECT_CC) reportOpenCPUError("unknown custom pseudo instruction");
  MachineFunction *MF = Head->getParent();
  const TargetInstrInfo &TII = *MF->getSubtarget().getInstrInfo();
  const BasicBlock *IRBB = Head->getBasicBlock();
  auto *FalseBB = MF->CreateMachineBasicBlock(IRBB);
  auto *TrueBB = MF->CreateMachineBasicBlock(IRBB);
  auto *Sink = MF->CreateMachineBasicBlock(IRBB);
  auto Position = std::next(Head->getIterator());
  MF->insert(Position, FalseBB); MF->insert(Position, TrueBB); MF->insert(Position, Sink);
  Sink->splice(Sink->begin(), Head, std::next(MI.getIterator()), Head->end());
  Sink->transferSuccessorsAndUpdatePHIs(Head);
  BuildMI(*Head, MI, MI.getDebugLoc(), TII.get(OpenCPU::BR_CC))
      .add(MI.getOperand(1)).add(MI.getOperand(2)).add(MI.getOperand(3)).addMBB(TrueBB);
  BuildMI(*FalseBB, FalseBB->end(), MI.getDebugLoc(), TII.get(OpenCPU::J)).addMBB(Sink);
  Head->addSuccessor(FalseBB); Head->addSuccessor(TrueBB);
  FalseBB->addSuccessor(Sink); TrueBB->addSuccessor(Sink);
  BuildMI(*Sink, Sink->begin(), MI.getDebugLoc(), TII.get(TargetOpcode::PHI), MI.getOperand(0).getReg())
      .add(MI.getOperand(4)).addMBB(TrueBB).add(MI.getOperand(5)).addMBB(FalseBB);
  MI.eraseFromParent();
  return Sink;
}

class OpenCPUDAGToDAGISel : public SelectionDAGISel {
public:
  OpenCPUDAGToDAGISel(OpenCPUTargetMachine &TM, CodeGenOptLevel OL) : SelectionDAGISel(TM, OL) {}
  void Select(SDNode *N) override;
private:
  void frameAddress(SDValue Address, SDValue &Base, SDValue &Offset, const SDLoc &DL) {
    int64_t Bytes = 0;
    while (Address.getOpcode() == ISD::ADD || Address.getOpcode() == ISD::OR) {
      if (auto *C = dyn_cast<ConstantSDNode>(Address.getOperand(1))) {
        Bytes += C->getSExtValue(); Address = Address.getOperand(0);
      } else if (auto *C = dyn_cast<ConstantSDNode>(Address.getOperand(0))) {
        Bytes += C->getSExtValue(); Address = Address.getOperand(1);
      } else break;
    }
    auto *FI = dyn_cast<FrameIndexSDNode>(Address);
    if (!FI || Bytes % 2) reportOpenCPUError("memory address must be an aligned constant frame address");
    Base = CurDAG->getTargetFrameIndex(FI->getIndex(), MVT::i16);
    Offset = CurDAG->getTargetConstant(Bytes, DL, MVT::i16);
  }
#include "OpenCPUGenDAGISel.inc"
};

void OpenCPUDAGToDAGISel::Select(SDNode *N) {
  if (N->isMachineOpcode()) { N->setNodeId(-1); return; }
  SDLoc DL(N);
  switch (N->getOpcode()) {
  case ISD::UNDEF:
    // A concrete zero is a valid choice for LLVM undef; never let an
    // IMPLICIT_DEF pseudo reach the flat assembly writer.
    CurDAG->SelectNodeTo(N, OpenCPU::LI, MVT::i16, CurDAG->getTargetConstant(0, DL, MVT::i16)); return;
  case ISD::LOAD: {
    auto *L = cast<LoadSDNode>(N);
    if (L->getMemoryVT() != MVT::i16 || L->getExtensionType() != ISD::NON_EXTLOAD)
      reportOpenCPUError("only aligned i16 loads are supported");
    SDValue Base, Offset;
    frameAddress(L->getBasePtr(), Base, Offset, DL);
    SDNode *R = CurDAG->getMachineNode(OpenCPU::LOAD, DL, MVT::i16, MVT::Other, Base, Offset, L->getChain());
    CurDAG->setNodeMemRefs(cast<MachineSDNode>(R), {L->getMemOperand()});
    ReplaceNode(N, R); return;
  }
  case ISD::STORE: {
    auto *S = cast<StoreSDNode>(N);
    if (S->getMemoryVT() != MVT::i16 || S->isTruncatingStore()) reportOpenCPUError("only aligned i16 stores are supported");
    SDValue Base, Offset;
    frameAddress(S->getBasePtr(), Base, Offset, DL);
    SDNode *R = CurDAG->getMachineNode(OpenCPU::STORE, DL, MVT::Other,
                                    {S->getValue(), Base, Offset, S->getChain()});
    CurDAG->setNodeMemRefs(cast<MachineSDNode>(R), {S->getMemOperand()});
    ReplaceNode(N, R); return;
  }
  case OCISD::BR_CC:
    CurDAG->SelectNodeTo(N, OpenCPU::BR_CC, MVT::Other,
      {N->getOperand(1), N->getOperand(2), N->getOperand(3), N->getOperand(4), N->getOperand(0)}); return;
  case OCISD::SELECT_CC:
    CurDAG->SelectNodeTo(N, OpenCPU::SELECT_CC, MVT::i16,
      {N->getOperand(0), N->getOperand(1), N->getOperand(2), N->getOperand(3), N->getOperand(4)}); return;
  case OCISD::CALL:
  case OCISD::RET: {
    SmallVector<SDValue, 10> Ops;
    for (unsigned I = 1; I < N->getNumOperands(); ++I)
      if (N->getOperand(I).getValueType() != MVT::Glue) Ops.push_back(N->getOperand(I));
    Ops.push_back(N->getOperand(0));
    if (N->getNumOperands() > 1 && N->getOperand(N->getNumOperands() - 1).getValueType() == MVT::Glue)
      Ops.push_back(N->getOperand(N->getNumOperands() - 1));
    if (N->getOpcode() == OCISD::CALL)
      CurDAG->SelectNodeTo(N, OpenCPU::CALL, CurDAG->getVTList(MVT::Other, MVT::Glue), Ops);
    else CurDAG->SelectNodeTo(N, OpenCPU::RET, MVT::Other, Ops);
    return;
  }
  default: SelectCode(N); return;
  }
}

class OpenCPUDAGToDAGISelLegacy : public SelectionDAGISelLegacy {
public:
  static char ID;
  OpenCPUDAGToDAGISelLegacy(OpenCPUTargetMachine &TM, CodeGenOptLevel OL)
      : SelectionDAGISelLegacy(ID, std::make_unique<OpenCPUDAGToDAGISel>(TM, OL)) {}
};
char OpenCPUDAGToDAGISelLegacy::ID;

// The target is intentionally a bounded initial ABI, checked using LLVM's IR
// types and DataLayout rather than by parsing textual IR in the Python driver.
class OpenCPUValidate : public FunctionPass {
public:
  static char ID;
  OpenCPUValidate() : FunctionPass(ID) {}
  StringRef getPassName() const override { return "OpenCPU initial ABI validation"; }
  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override { AU.setPreservesAll(); }
};
char OpenCPUValidate::ID;

uint64_t wordObjectBytes(Type *T) {
  if (T->isIntegerTy(16)) return 2;
  if (auto *A = dyn_cast<ArrayType>(T)) {
    uint64_t Child = wordObjectBytes(A->getElementType());
    uint64_t Count = A->getNumElements();
    if (Child && Count && Count <= 2040 / Child) return Child * Count;
  }
  return 0;
}
bool allowedIntrinsic(const Function &F) {
  if (!F.isIntrinsic()) return false;
  return F.getIntrinsicID() == Intrinsic::lifetime_start || F.getIntrinsicID() == Intrinsic::lifetime_end ||
         F.getName().starts_with("llvm.dbg.");
}
void validateFrameAddress(Value *P, const DataLayout &DL, StringRef FunctionName) {
  int64_t Offset = 0;
  Value *Base = GetPointerBaseWithConstantOffset(P, Offset, DL);
  auto *A = dyn_cast<AllocaInst>(Base);
  if (!A || !isa<ConstantInt>(A->getArraySize())) reportOpenCPUError("memory pointers must derive from a fixed alloca in " + FunctionName);
  uint64_t Size = wordObjectBytes(A->getAllocatedType());
  uint64_t Count = cast<ConstantInt>(A->getArraySize())->getValue().getLimitedValue(1021);
  if (!Size || Count > 2040 / Size || Offset < 0 || Offset % 2 || uint64_t(Offset) + 2 > Size * Count)
    reportOpenCPUError("frame access is unaligned or outside its allocated object in " + FunctionName);
}
bool OpenCPUValidate::runOnFunction(Function &F) {
  Module &M = *F.getParent();
  if (!M.global_empty() || !M.alias_empty() || !M.ifunc_empty()) reportOpenCPUError("global data, aliases and indirect functions are unsupported");
  if (!M.getModuleInlineAsm().empty()) reportOpenCPUError("module inline assembly is unsupported");
  for (Function &Other : M)
    if (Other.isDeclaration() && !allowedIntrinsic(Other))
      reportOpenCPUError("external functions are unsupported");
  Function *Main = M.getFunction("main");
  if (!Main || Main->isDeclaration() || !Main->arg_empty() ||
      !(Main->getReturnType()->isIntegerTy(16) || Main->getReturnType()->isVoidTy()))
    reportOpenCPUError("module must define main() returning i16 or void");
  if (F.isDeclaration()) { if (!allowedIntrinsic(F)) reportOpenCPUError("external functions are unsupported"); return false; }
  if (F.getName().empty()) reportOpenCPUError("functions require assembly-compatible names");
  if (F.getName() == "_start" || F.getName().starts_with(".Locpu")) reportOpenCPUError("reserved function name");
  if (F.getCallingConv() != CallingConv::C || F.isVarArg() || F.arg_size() > 4 ||
      !(F.getReturnType()->isIntegerTy(16) || F.getReturnType()->isVoidTy()))
    reportOpenCPUError("functions require C calling convention, up to four i16 arguments and i16/void return");
  for (char C : F.getName())
    if (!(std::isalnum(static_cast<unsigned char>(C)) || C == '_' || C == '.' || C == '$')) reportOpenCPUError("function name is not representable in OpenCPU assembly");
  if (!F.getName().empty() && std::isdigit(static_cast<unsigned char>(F.getName().front()))) reportOpenCPUError("function name cannot start with a digit");
  for (Argument &A : F.args()) if (!A.getType()->isIntegerTy(16)) reportOpenCPUError("function arguments must be i16");
  const DataLayout &DL = M.getDataLayout();
  if (DL.getPointerSizeInBits() != 16 || DL.getTypeAllocSize(Type::getInt16Ty(F.getContext())) != 2)
    reportOpenCPUError("incompatible module data layout");
  for (BasicBlock &BB : F) for (Instruction &I : BB) {
    if (auto *CB = dyn_cast<CallInst>(&I)) {
      Function *Callee = CB->getCalledFunction();
      if (!Callee) reportOpenCPUError("indirect calls and inline assembly are unsupported");
      if (allowedIntrinsic(*Callee)) continue;
      if (Callee->isDeclaration()) reportOpenCPUError("external functions are unsupported");
      if (CB->isMustTailCall() || CB->getCallingConv() != CallingConv::C) reportOpenCPUError("tail-call requirements and custom calling conventions are unsupported");
      if (CB->arg_size() > 4 || CB->getFunctionType() != Callee->getFunctionType() ||
          !(CB->getType()->isIntegerTy(16) || CB->getType()->isVoidTy()))
        reportOpenCPUError("call signature must match a function with up to four i16 arguments and i16/void return");
      for (Value *Argument : CB->args())
        if (!Argument->getType()->isIntegerTy(16)) reportOpenCPUError("call arguments must be i16");
      continue;
    }
    if (auto *A = dyn_cast<AllocaInst>(&I)) {
      auto *Count = dyn_cast<ConstantInt>(A->getArraySize());
      uint64_t Size = wordObjectBytes(A->getAllocatedType());
      if (!Count || Count->isZero() || !Size) reportOpenCPUError("alloca must have a fixed, bounded nonzero i16 object type");
      if (A->getAlign() != Align(2) || Count->getValue().getLimitedValue(1021) > 2040 / Size)
        reportOpenCPUError("alloca alignment or total size exceeds the initial ABI");
      continue;
    }
    if (auto *G = dyn_cast<GetElementPtrInst>(&I)) {
      for (Value *Index : G->indices()) if (!isa<ConstantInt>(Index)) reportOpenCPUError("dynamic GEP indices are unsupported");
      int64_t Offset = 0;
      if (!isa<AllocaInst>(GetPointerBaseWithConstantOffset(G, Offset, DL)) || Offset % 2)
        reportOpenCPUError("GEP must be an aligned constant frame address");
      continue;
    }
    if (auto *L = dyn_cast<LoadInst>(&I)) {
      if (!L->getType()->isIntegerTy(16) || L->isAtomic() || L->getAlign() != Align(2)) reportOpenCPUError("load requires aligned, non-atomic i16 memory");
      validateFrameAddress(L->getPointerOperand(), DL, F.getName()); continue;
    }
    if (auto *S = dyn_cast<StoreInst>(&I)) {
      if (!S->getValueOperand()->getType()->isIntegerTy(16) || S->isAtomic() || S->getAlign() != Align(2)) reportOpenCPUError("store requires aligned, non-atomic i16 memory");
      validateFrameAddress(S->getPointerOperand(), DL, F.getName()); continue;
    }
    switch (I.getOpcode()) {
    case Instruction::Add: case Instruction::Sub: case Instruction::Mul:
    case Instruction::And: case Instruction::Or: case Instruction::Xor:
    case Instruction::Shl: case Instruction::LShr:
      if (!I.getType()->isIntegerTy(16)) reportOpenCPUError("arithmetic values must be i16");
      break;
    case Instruction::ICmp:
      if (!I.getOperand(0)->getType()->isIntegerTy(16)) reportOpenCPUError("icmp operands must be i16");
      break;
    case Instruction::PHI: case Instruction::Select:
      if (!(I.getType()->isIntegerTy(16) || I.getType()->isIntegerTy(1))) reportOpenCPUError("phi/select values must be i16 or i1");
      break;
    case Instruction::ZExt: case Instruction::SExt:
      if (!I.getOperand(0)->getType()->isIntegerTy(1) || !I.getType()->isIntegerTy(16)) reportOpenCPUError("extensions must convert i1 to i16");
      break;
    case Instruction::Trunc:
      if (!I.getOperand(0)->getType()->isIntegerTy(16) || !I.getType()->isIntegerTy(1)) reportOpenCPUError("trunc must convert i16 to i1");
      break;
    case Instruction::UncondBr: case Instruction::CondBr: case Instruction::Ret: break;
    default: reportOpenCPUError(Twine("unsupported IR instruction: ") + I.getOpcodeName());
    }
  }
  return false;
}

class OpenCPUPassConfig : public TargetPassConfig {
public:
  OpenCPUPassConfig(OpenCPUTargetMachine &TM, PassManagerBase &PM) : TargetPassConfig(TM, PM) {}
  void addIRPasses() override { addPass(new OpenCPUValidate); TargetPassConfig::addIRPasses(); }
  bool addInstSelector() override {
    addPass(new OpenCPUDAGToDAGISelLegacy(getTM<OpenCPUTargetMachine>(), getOptLevel()));
    return false;
  }
};
TargetPassConfig *OpenCPUTargetMachine::createPassConfig(PassManagerBase &PM) { return new OpenCPUPassConfig(*this, PM); }

} // namespace

INITIALIZE_PASS(OpenCPUDAGToDAGISelLegacy, "opencpu-isel", "OpenCPU DAG instruction selection", false, false)

#define GET_REGINFO_TARGET_DESC
#include "OpenCPUGenRegisterInfo.inc"
#define GET_INSTRINFO_CTOR_DTOR
#include "OpenCPUGenInstrInfo.inc"
#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "OpenCPUGenSubtargetInfo.inc"

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeOpenCPUTarget() {
  RegisterTargetMachine<OpenCPUTargetMachine> X(getTheOpenCPUTarget());
  initializeOpenCPUDAGToDAGISelLegacyPass(*PassRegistry::getPassRegistry());
}
