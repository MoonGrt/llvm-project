// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// OpenCPU ISA v2 native LLVM code generator. ABI v2 exposes byte-addressed
// C data pointers while the hardware stack and memory ports use word addresses.
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
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
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
  explicit OpenCPUInstrInfo(const TargetSubtargetInfo &ST) : OpenCPUGenInstrInfo(ST, RI, OpenCPU::ADJCALLSTACKDOWN, OpenCPU::ADJCALLSTACKUP) {}
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
  bool hasReservedCallFrame(const MachineFunction &) const override { return false; }
  MachineBasicBlock::iterator eliminateCallFramePseudoInstr(MachineFunction &, MachineBasicBlock &, MachineBasicBlock::iterator) const override;
};

namespace OCISD {
enum { CALL = ISD::BUILTIN_OP_END, RET, BR_CC, SELECT_CC, RAW_LOAD, RAW_STORE };
}
class OpenCPUDAGInfo : public SelectionDAGTargetInfo {
public:
  const char *getTargetNodeName(unsigned Op) const override {
    switch (Op) {
    case OCISD::CALL: return "OpenCPU::CALL";
    case OCISD::RET: return "OpenCPU::RET";
    case OCISD::BR_CC: return "OpenCPU::BR_CC";
    case OCISD::SELECT_CC: return "OpenCPU::SELECT_CC";
    case OCISD::RAW_LOAD: return "OpenCPU::RAW_LOAD";
    case OCISD::RAW_STORE: return "OpenCPU::RAW_STORE";
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
      : CodeGenTargetMachineImpl(T, TT.computeDataLayout(), TT, CPU, FS, Options,
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
  if (Dst == Src) return;
  if (Src == OpenCPU::ZERO) {
    BuildMI(MBB, I, DL, get(OpenCPU::LI), Dst).addImm(0);
    return;
  }
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
  (void)SPAdj; // All frame references use FP, independent of outgoing arguments.
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
  if (MI.getOpcode() == OpenCPU::FRAMEADDR) {
    MI.getOperand(FIOp).ChangeToRegister(OpenCPU::FP, false);
    MI.getOperand(FIOp + 1).ChangeToImmediate(Bytes);
    return false;
  }
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
  if (Bytes % 2 || Bytes > 2040 || Bytes + 4 + getOpenCPUDataEnd(*MF.getFunction().getParent()) > 2048)
    reportOpenCPUError("static stack frame and global data exceed the 1024-word RAM or are unaligned");
  adjustRegister(MBB, I, DL, TII, OpenCPU::SP, -int64_t(Bytes / 2 + 2));
  BuildMI(MBB, I, DL, TII.get(OpenCPU::CHECKSP)).addImm(getOpenCPUDataEnd(*MF.getFunction().getParent()) / 2);
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

MachineBasicBlock::iterator OpenCPUFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB, MachineBasicBlock::iterator I) const {
  const auto &TII = *MF.getSubtarget().getInstrInfo();
  int64_t Bytes = I->getOperand(0).getImm();
  if (Bytes % 2 || Bytes > 2040) reportOpenCPUError("outgoing argument area exceeds RAM");
  bool Down = I->getOpcode() == OpenCPU::ADJCALLSTACKDOWN;
  adjustRegister(MBB, I, I->getDebugLoc(), TII, OpenCPU::SP, (Down ? -1 : 1) * Bytes / 2);
  if (Down && Bytes)
    BuildMI(MBB, I, I->getDebugLoc(), TII.get(OpenCPU::CHECKSP)).addImm(getOpenCPUDataEnd(*MF.getFunction().getParent()) / 2);
  return MBB.erase(I);
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
  for (MVT From : {MVT::i1, MVT::i8, MVT::i16})
    setOperationAction(ISD::SIGN_EXTEND_INREG, From, Custom);
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
                      ISD::SSUBSAT, ISD::USUBSAT, ISD::UADDO_CARRY, ISD::USUBO_CARRY, ISD::SADDO_CARRY, ISD::SSUBO_CARRY,
                      ISD::SHL_PARTS, ISD::SRL_PARTS, ISD::SRA_PARTS,
                      ISD::SMUL_LOHI, ISD::UMUL_LOHI})
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
    if (From == MVT::i8) {
      SDValue V = DAG.getNode(ISD::AND, DL, MVT::i16, Op.getOperand(0), DAG.getConstant(255, DL, MVT::i16));
      SDValue Shifted = DAG.getNode(ISD::XOR, DL, MVT::i16, V, DAG.getConstant(128, DL, MVT::i16));
      return DAG.getNode(ISD::SUB, DL, MVT::i16, Shifted, DAG.getConstant(128, DL, MVT::i16));
    }
    if (From != MVT::i1) reportOpenCPUError("unsupported sign extension width");
    SDValue Bit = DAG.getNode(ISD::AND, DL, MVT::i16, Op.getOperand(0), DAG.getConstant(1, DL, MVT::i16));
    return DAG.getNode(ISD::SUB, DL, MVT::i16, DAG.getConstant(0, DL, MVT::i16), Bit);
  }
  case ISD::SRA: {
    // Arithmetic shifts and sign extension use only existing ISA operations.
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

static SDValue slotValue(SDValue V, const ISD::ArgFlagsTy &Flags,
                         const SDLoc &DL, SelectionDAG &DAG) {
  if (V.getValueType() == MVT::i16) return V;
  unsigned Op = Flags.isSExt() ? ISD::SIGN_EXTEND : ISD::ZERO_EXTEND;
  return DAG.getNode(Op, DL, MVT::i16, V);
}
SDValue OpenCPUTargetLowering::LowerFormalArguments(SDValue Chain, CallingConv::ID,
    bool VarArg, const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &Values) const {
  if (VarArg) reportOpenCPUError("variadic functions are unsupported");
  MachineFunction &MF = DAG.getMachineFunction();
  for (unsigned I = 0; I < Ins.size(); ++I) {
    SDValue V;
    if (I < 4) {
      Register VReg = MF.addLiveIn(ArgRegs[I], &OpenCPU::GPRRegClass);
      V = DAG.getCopyFromReg(Chain, DL, VReg, MVT::i16);
    } else {
      int FI = MF.getFrameInfo().CreateFixedObject(2, (I - 4) * 2, true);
      V = DAG.getLoad(MVT::i16, DL, Chain, DAG.getFrameIndex(FI, MVT::i16),
                     MachinePointerInfo::getFixedStack(MF, FI), Align(2));
    }
    Chain = V.getValue(1);
    if (Ins[I].VT != MVT::i16) V = DAG.getNode(ISD::TRUNCATE, DL, Ins[I].VT, V);
    Values.push_back(V);
  }
  return Chain;
}
SDValue OpenCPUTargetLowering::LowerReturn(SDValue Chain, CallingConv::ID, bool,
    const SmallVectorImpl<ISD::OutputArg> &Outs, const SmallVectorImpl<SDValue> &Values,
    const SDLoc &DL, SelectionDAG &DAG) const {
  if (Outs.size() > 4) reportOpenCPUError("return value exceeds 64 bits");
  SDValue Glue;
  SmallVector<SDValue, 8> Ops;
  for (unsigned I = 0; I < Outs.size(); ++I) {
    Chain = DAG.getCopyToReg(Chain, DL, ArgRegs[I], slotValue(Values[I], Outs[I].Flags, DL, DAG), Glue);
    Glue = Chain.getValue(1);
  }
  Ops.push_back(Chain);
  for (unsigned I = 0; I < Outs.size(); ++I) Ops.push_back(DAG.getRegister(ArgRegs[I], MVT::i16));
  if (Glue) Ops.push_back(Glue);
  return DAG.getNode(OCISD::RET, DL, MVT::Other, Ops);
}
SDValue OpenCPUTargetLowering::LowerCall(CallLoweringInfo &CLI,
    SmallVectorImpl<SDValue> &Values) const {
  auto &DAG = CLI.DAG;
  auto &DL = CLI.DL;
  CLI.IsTailCall = false;
  auto *GA = dyn_cast<GlobalAddressSDNode>(CLI.Callee);
  if (!GA || !isa<Function>(GA->getGlobal())) reportOpenCPUError("indirect calls are unsupported");
  const Function *CalleeFunction = cast<Function>(GA->getGlobal());
  StringRef Name = CalleeFunction->getName();
  if (Name == "__opencpu_raw_load16" || Name == "__opencpu_mmio_read16") {
    SDValue V = DAG.getNode(OCISD::RAW_LOAD, DL, DAG.getVTList(MVT::i16, MVT::Other),
                           CLI.Chain, CLI.OutVals[0]);
    Values.push_back(V);
    return V.getValue(1);
  }
  if (Name == "__opencpu_raw_store16" || Name == "__opencpu_mmio_write16")
    return DAG.getNode(OCISD::RAW_STORE, DL, MVT::Other,
                       CLI.Chain, CLI.OutVals[0], CLI.OutVals[1]);
  if (CalleeFunction->isDeclaration()) reportOpenCPUError("unresolved function or runtime helper: " + Name);
  unsigned StackBytes = CLI.Outs.size() > 4 ? (CLI.Outs.size() - 4) * 2 : 0;
  SDValue Chain = DAG.getCALLSEQ_START(CLI.Chain, StackBytes, 0, DL), Glue;
  if (StackBytes) {
    SDValue SP = DAG.getCopyFromReg(Chain, DL, OpenCPU::SP, MVT::i16);
    Chain = SP.getValue(1);
    for (unsigned I = 4; I < CLI.OutVals.size(); ++I) {
      SDValue Address = DAG.getNode(ISD::ADD, DL, MVT::i16, SP, DAG.getConstant(I - 4, DL, MVT::i16));
      Chain = DAG.getNode(OCISD::RAW_STORE, DL, MVT::Other, Chain, Address,
                          slotValue(CLI.OutVals[I], CLI.Outs[I].Flags, DL, DAG));
    }
  }
  for (unsigned I = 0; I < std::min<unsigned>(4, CLI.OutVals.size()); ++I) {
    Chain = DAG.getCopyToReg(Chain, DL, ArgRegs[I], slotValue(CLI.OutVals[I], CLI.Outs[I].Flags, DL, DAG), Glue);
    Glue = Chain.getValue(1);
  }
  SDValue Callee = DAG.getTargetGlobalAddress(GA->getGlobal(), DL, MVT::i16);
  SmallVector<SDValue, 10> Ops{Chain, Callee};
  for (unsigned I = 0; I < std::min<unsigned>(4, CLI.OutVals.size()); ++I)
    Ops.push_back(DAG.getRegister(ArgRegs[I], MVT::i16));
  Ops.push_back(DAG.getRegisterMask(DAG.getSubtarget().getRegisterInfo()->getCallPreservedMask(DAG.getMachineFunction(), CLI.CallConv)));
  if (Glue) Ops.push_back(Glue);
  Chain = DAG.getNode(OCISD::CALL, DL, DAG.getVTList(MVT::Other, MVT::Glue), Ops);
  Glue = Chain.getValue(1);
  Chain = DAG.getCALLSEQ_END(Chain, StackBytes, 0, Glue, DL);
  Glue = Chain.getValue(1);
  if (CLI.Ins.size() > 4) reportOpenCPUError("call return value exceeds 64 bits");
  for (unsigned I = 0; I < CLI.Ins.size(); ++I) {
    SDValue V = DAG.getCopyFromReg(Chain, DL, ArgRegs[I], MVT::i16, Glue);
    Chain = V.getValue(1); Glue = V.getValue(2);
    if (CLI.Ins[I].VT != MVT::i16) V = DAG.getNode(ISD::TRUNCATE, DL, CLI.Ins[I].VT, V);
    Values.push_back(V);
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
      .addReg(MI.getOperand(1).getReg()).addReg(MI.getOperand(2).getReg()).add(MI.getOperand(3)).addMBB(TrueBB);
  BuildMI(*FalseBB, FalseBB->end(), MI.getDebugLoc(), TII.get(OpenCPU::J)).addMBB(Sink);
  Head->addSuccessor(FalseBB); Head->addSuccessor(TrueBB);
  FalseBB->addSuccessor(Sink); TrueBB->addSuccessor(Sink);
  BuildMI(*Sink, Sink->begin(), MI.getDebugLoc(), TII.get(TargetOpcode::PHI), MI.getOperand(0).getReg())
      .addReg(MI.getOperand(4).getReg()).addMBB(TrueBB).addReg(MI.getOperand(5).getReg()).addMBB(FalseBB);
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
  case ISD::Constant:
    if (N->getValueType(0) == MVT::i16 && cast<ConstantSDNode>(N)->isZero()) {
      SDValue Zero = CurDAG->getCopyFromReg(CurDAG->getEntryNode(), DL,
                                          OpenCPU::ZERO, MVT::i16);
      ReplaceNode(N, Zero.getNode());
      return;
    }
    break;
  case ISD::CALLSEQ_START:
  case ISD::CALLSEQ_END: {
    SmallVector<SDValue, 4> Ops{N->getOperand(1), N->getOperand(2), N->getOperand(0)};
    if (N->getNumOperands() > 3) Ops.push_back(N->getOperand(3));
    CurDAG->SelectNodeTo(N, N->getOpcode() == ISD::CALLSEQ_START ? OpenCPU::ADJCALLSTACKDOWN : OpenCPU::ADJCALLSTACKUP,
                        CurDAG->getVTList(MVT::Other, MVT::Glue), Ops); return;
  }
  case ISD::FrameIndex: {
    auto *FI = cast<FrameIndexSDNode>(N);
    CurDAG->SelectNodeTo(N, OpenCPU::FRAMEADDR, MVT::i16,
      CurDAG->getTargetFrameIndex(FI->getIndex(), MVT::i16), CurDAG->getTargetConstant(0, DL, MVT::i16)); return;
  }
  case OCISD::RAW_LOAD:
    CurDAG->SelectNodeTo(N, OpenCPU::LOAD, CurDAG->getVTList(MVT::i16, MVT::Other),
      {N->getOperand(1), CurDAG->getTargetConstant(0, DL, MVT::i16), N->getOperand(0)}); return;
  case OCISD::RAW_STORE:
    CurDAG->SelectNodeTo(N, OpenCPU::STORE, MVT::Other,
      {N->getOperand(2), N->getOperand(1), CurDAG->getTargetConstant(0, DL, MVT::i16), N->getOperand(0)}); return;
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
  default: break;
  }
  SelectCode(N);
}

class OpenCPUDAGToDAGISelLegacy : public SelectionDAGISelLegacy {
public:
  static char ID;
  OpenCPUDAGToDAGISelLegacy(OpenCPUTargetMachine &TM, CodeGenOptLevel OL)
      : SelectionDAGISelLegacy(ID, std::make_unique<OpenCPUDAGToDAGISel>(TM, OL)) {}
};
char OpenCPUDAGToDAGISelLegacy::ID;

// Validate the freestanding scalar ABI after target-specific preparation.
class OpenCPUValidate : public FunctionPass {
public:
  static char ID;
  OpenCPUValidate() : FunctionPass(ID) {}
  StringRef getPassName() const override { return "OpenCPU freestanding ABI validation"; }
  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override { AU.setPreservesAll(); }
};
char OpenCPUValidate::ID;

static bool supportedScalar(Type *T, bool Void = false) {
  if (T->isPointerTy() || (Void && T->isVoidTy())) return true;
  auto *I = dyn_cast<IntegerType>(T);
  if (!I) return false;
  unsigned N = I->getBitWidth(); return N == 1 || N == 8 || N == 16 || N == 32 || N == 64;
}
static bool supportedObject(Type *T) {
  if (auto *A = dyn_cast<ArrayType>(T)) return A->getNumElements() && supportedObject(A->getElementType());
  return supportedScalar(T);
}
bool allowedIntrinsic(const Function &F) {
  if (F.getName() == "__opencpu_raw_load16" || F.getName() == "__opencpu_raw_store16" ||
      F.getName() == "__opencpu_mmio_read16" || F.getName() == "__opencpu_mmio_write16") return true;
  if (!F.isIntrinsic()) return false;
  return F.getIntrinsicID() == Intrinsic::lifetime_start || F.getIntrinsicID() == Intrinsic::lifetime_end ||
         F.getIntrinsicID() == Intrinsic::assume || F.getName().starts_with("llvm.dbg.");
}
bool OpenCPUValidate::runOnFunction(Function &F) {
  Module &M = *F.getParent();
  Function *Main = M.getFunction("main");
  if (!Main || Main->isDeclaration() || !Main->arg_empty() ||
      !(Main->getReturnType()->isIntegerTy(16) || Main->getReturnType()->isVoidTy()))
    reportOpenCPUError("module must define main() returning i16 or void");
  if (F.isDeclaration()) {
    if (!F.use_empty() && !allowedIntrinsic(F)) reportOpenCPUError("unresolved external function: " + F.getName());
    return false;
  }
  if (F.getName().empty() || F.getName() == "_start" || F.getName().starts_with(".Locpu"))
    reportOpenCPUError("reserved or unrepresentable function name");
  for (char C : F.getName())
    if (!(std::isalnum(static_cast<unsigned char>(C)) || C == '_' || C == '.' || C == '$'))
      reportOpenCPUError("function name is not representable in OpenCPU assembly");
  if (std::isdigit(static_cast<unsigned char>(F.getName().front()))) reportOpenCPUError("function name cannot start with a digit");
  if (F.getCallingConv() != CallingConv::C || F.isVarArg() || !supportedScalar(F.getReturnType(), true))
    reportOpenCPUError("functions require C calling convention and scalar integer/pointer parameters and return values");
  for (Argument &A : F.args()) {
    if (!supportedScalar(A.getType())) reportOpenCPUError("unsupported function argument type");
    if (A.hasByValAttr() || A.hasStructRetAttr() || A.hasInAllocaAttr() || A.hasByRefAttr())
      reportOpenCPUError("aggregate arguments and returns are unsupported");
  }
  const DataLayout &DL = M.getDataLayout();
  if (DL.getPointerSizeInBits() != 16 || DL.getTypeAllocSize(Type::getInt16Ty(F.getContext())) != 2)
    reportOpenCPUError("incompatible module data layout");
  for (BasicBlock &BB : F) for (Instruction &I : BB) {
    if (auto *CB = dyn_cast<CallInst>(&I)) {
      Function *Callee = CB->getCalledFunction();
      if (!Callee) reportOpenCPUError("indirect calls and inline assembly are unsupported");
      if (CB->isMustTailCall() || CB->getCallingConv() != CallingConv::C ||
          CB->getFunctionType() != Callee->getFunctionType()) reportOpenCPUError("incompatible call signature or calling convention");
      if (allowedIntrinsic(*Callee)) {
        StringRef Name = Callee->getName();
        if (Name.starts_with("__opencpu_")) {
          bool Read = Name.ends_with("load16") || Name.ends_with("read16");
          unsigned Count = Read ? 1 : 2;
          if (CB->arg_size() != Count || (Read ? !CB->getType()->isIntegerTy(16) : !CB->getType()->isVoidTy()))
            reportOpenCPUError("invalid raw-memory or MMIO signature");
          for (Value *A : CB->args()) if (!A->getType()->isIntegerTy(16))
            reportOpenCPUError("raw-memory and MMIO operands must be i16");
        }
        continue;
      }
      if (Callee->isDeclaration()) reportOpenCPUError("unresolved external function: " + Callee->getName());
      if (!supportedScalar(CB->getType(), true)) reportOpenCPUError("unsupported call result type");
      for (Value *A : CB->args()) if (!supportedScalar(A->getType())) reportOpenCPUError("unsupported call argument type");
      continue;
    }
    if (auto *A = dyn_cast<AllocaInst>(&I)) {
      auto *Count = dyn_cast<ConstantInt>(A->getArraySize());
      if (!Count || Count->isZero() || !supportedObject(A->getAllocatedType()) || A->getAlign() > Align(2))
        reportOpenCPUError("alloca requires a fixed integer/pointer object aligned to at most two bytes");
      uint64_t Size = DL.getTypeAllocSize(A->getAllocatedType());
      if (Size > 2040 || Count->getValue().getLimitedValue(2041) > 2040 / Size)
        reportOpenCPUError("static stack object exceeds RAM");
      continue;
    }
    if (isa<GetElementPtrInst>(I)) continue;
    switch (I.getOpcode()) {
    case Instruction::Add: case Instruction::Sub: case Instruction::Mul:
    case Instruction::And: case Instruction::Or: case Instruction::Xor:
    case Instruction::Shl: case Instruction::LShr: case Instruction::AShr:
    case Instruction::ICmp: case Instruction::PHI: case Instruction::Select:
    case Instruction::ZExt: case Instruction::SExt: case Instruction::Trunc:
    case Instruction::PtrToInt: case Instruction::IntToPtr: case Instruction::BitCast:
    case Instruction::Freeze:
      if (!supportedScalar(I.getType())) reportOpenCPUError("unsupported scalar width or floating-point operation");
      for (Value *V : I.operands())
        if (!isa<BasicBlock>(V) && !supportedScalar(V->getType())) reportOpenCPUError("unsupported operand type");
      break;
    case Instruction::UncondBr: case Instruction::CondBr: case Instruction::Ret:
    case Instruction::Unreachable: break;
    default: reportOpenCPUError(Twine("unsupported IR instruction: ") + I.getOpcodeName());
    }
  }
  return false;
}

class OpenCPUPassConfig : public TargetPassConfig {
public:
  OpenCPUPassConfig(OpenCPUTargetMachine &TM, PassManagerBase &PM) : TargetPassConfig(TM, PM) {}
  void addIRPasses() override {
    addPass(createPromoteMemoryToRegisterPass());
    addPass(createCFGSimplificationPass());
    addPass(createLowerSwitchPass());
    addPass(createOpenCPUPreparePass());
    addPass(createGlobalDCEPass());
    addPass(new OpenCPUValidate);
    TargetPassConfig::addIRPasses();
  }
  // Four allocatable registers make local fast allocation needlessly exceed
  // the board ROM for wide runtime routines. Coalescing/greedy allocation is
  // required even with -O0; source-level optimizations remain disabled.
  void addFastRegAlloc() override { addOptimizedRegAlloc(); }
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
