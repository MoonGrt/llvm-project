// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Turn the byte-addressed, freestanding ABI into the word machine's primitives.
#include "OpenCPU.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/IPO.h"
#include <algorithm>
#include <map>
using namespace llvm;
static cl::opt<std::string> RuntimePath("opencpu-runtime", cl::desc("OpenCPU ABI v2 runtime bitcode"), cl::init(""));

namespace llvm {
unsigned getOpenCPUDataEnd(const Module &M) {
  if (auto *MD = M.getModuleFlag("opencpu.data_end"))
    return cast<ConstantInt>(cast<ConstantAsMetadata>(MD)->getValue())->getZExtValue();
  return 2;
}
namespace {
class OpenCPUPrepare : public ModulePass {
  std::map<const GlobalVariable *, uint16_t> Addresses;
  SmallVector<uint8_t, 2048> Data;
  bool NeedRuntime = false;
  uint64_t pointerConstant(const Constant *C, const DataLayout &DL) {
    if (C->isNullValue()) return 0;
    if (auto *G = dyn_cast<GlobalVariable>(C)) {
      auto I = Addresses.find(G);
      if (I == Addresses.end()) reportOpenCPUError("global pointer refers to an unallocated object");
      return I->second;
    }
    if (auto *E = dyn_cast<ConstantExpr>(C)) {
      if (auto *G = dyn_cast<GEPOperator>(E)) {
        APInt Offset(16, 0);
        if (!G->accumulateConstantOffset(DL, Offset)) reportOpenCPUError("global pointer initializer must be constant");
        return uint16_t(pointerConstant(cast<Constant>(G->getPointerOperand()), DL) + Offset.getSExtValue());
      }
      if (E->isCast()) {
        if (auto *I = dyn_cast<ConstantInt>(E->getOperand(0))) return I->getValue().zextOrTrunc(16).getZExtValue();
        return pointerConstant(cast<Constant>(E->getOperand(0)), DL);
      }
    }
    reportOpenCPUError("unsupported global pointer initializer");
  }
  bool objectType(Type *T) {
    if (T->isPointerTy()) return true;
    if (auto *I = dyn_cast<IntegerType>(T)) return I->getBitWidth() == 1 || I->getBitWidth() == 8 || I->getBitWidth() == 16 || I->getBitWidth() == 32 || I->getBitWidth() == 64;
    if (auto *A = dyn_cast<ArrayType>(T)) return A->getNumElements() && objectType(A->getElementType());
    return false;
  }
  void serialize(const Constant *C, Type *T, unsigned Offset, const DataLayout &DL) {
    if (C->isNullValue() || isa<UndefValue>(C) || isa<PoisonValue>(C)) return;
    if (T->isIntegerTy() || T->isPointerTy()) {
      unsigned N = DL.getTypeAllocSize(T);
      uint64_t V = isa<ConstantInt>(C) ? cast<ConstantInt>(C)->getZExtValue() : pointerConstant(C, DL);
      for (unsigned I = 0; I < N; ++I) Data[Offset + I] = V >> (8 * I);
      return;
    }
    if (auto *A = dyn_cast<ArrayType>(T)) {
      unsigned Step = DL.getTypeAllocSize(A->getElementType());
      for (unsigned I = 0; I < A->getNumElements(); ++I)
        serialize(C->getAggregateElement(I), A->getElementType(), Offset + I * Step, DL);
      return;
    }
    reportOpenCPUError("global objects require integer, pointer or array types");
  }
  FunctionCallee helper(Module &M, StringRef Name, Type *Result, ArrayRef<Type *> Args) {
    NeedRuntime = true;
    return M.getOrInsertFunction(Name, FunctionType::get(Result, Args, false));
  }
  Value *memoryLength(MemIntrinsic *MI, IRBuilder<> &B) {
    Value *N = MI->getLength();
    if (auto *C = dyn_cast<ConstantInt>(N)) {
      if (C->getValue().ugt(2048)) reportOpenCPUError("memory intrinsic length exceeds the 2048-byte RAM");
    } else if (N->getType()->getIntegerBitWidth() > 16) {
      reportOpenCPUError("dynamic memory intrinsic lengths require at most 16 bits");
    }
    return B.CreateZExtOrTrunc(N, B.getInt16Ty());
  }
  void inlineMemory(MemIntrinsic *MI, Value *Length) {
    auto *Start = MI->getParent(); auto *F = Start->getParent();
    auto &C = F->getContext();
    IRBuilder<> B(MI);
    Value *Backward = B.getFalse();
    auto *Transfer = dyn_cast<MemTransferInst>(MI);
    if (isa<MemMoveInst>(MI)) Backward = B.CreateICmpUGT(Transfer->getDest(), Transfer->getSource());
    auto *Done = Start->splitBasicBlock(MI->getIterator(), "memory.done");
    Start->getTerminator()->eraseFromParent();
    auto *Loop = BasicBlock::Create(C, "memory.loop", F, Done);
    auto *Copy = BasicBlock::Create(C, "memory.byte", F, Done);
    B.SetInsertPoint(Start); B.CreateBr(Loop);
    B.SetInsertPoint(Loop);
    auto *Index = B.CreatePHI(B.getInt16Ty(), 2); Index->addIncoming(B.getInt16(0), Start);
    B.CreateCondBr(B.CreateICmpULT(Index, Length), Copy, Done);
    B.SetInsertPoint(Copy);
    Value *Position = B.CreateSelect(Backward, B.CreateSub(B.CreateSub(Length, B.getInt16(1)), Index), Index);
    Value *Dest = B.CreateGEP(B.getInt8Ty(), MI->getDest(), Position);
    Value *Byte;
    if (Transfer) {
      Value *Source = B.CreateGEP(B.getInt8Ty(), Transfer->getSource(), Position);
      auto *Load = B.CreateLoad(B.getInt8Ty(), Source); Load->setAlignment(Align(1)); Load->setVolatile(MI->isVolatile()); Byte = Load;
    } else Byte = cast<MemSetInst>(MI)->getValue();
    auto *Store = B.CreateStore(Byte, Dest); Store->setAlignment(Align(1)); Store->setVolatile(MI->isVolatile());
    Value *Next = B.CreateAdd(Index, B.getInt16(1)); Index->addIncoming(Next, Copy); B.CreateBr(Loop);
    MI->eraseFromParent();
  }
  bool lowerRuntime(Module &M) {
    bool Changed = false;
    SmallVector<Instruction *, 64> Work;
    for (Function &F : M) if (!F.isDeclaration() && !F.hasFnAttribute("opencpu-runtime"))
      for (Instruction &I : instructions(F)) Work.push_back(&I);
    for (Instruction *I : Work) {
      IRBuilder<> B(I);
      if (auto *BO = dyn_cast<BinaryOperator>(I)) {
        auto *Ty = dyn_cast<IntegerType>(BO->getType());
        if (!Ty) continue;
        unsigned Width = Ty->getBitWidth();
        StringRef Op;
        switch (BO->getOpcode()) {
        case Instruction::UDiv: Op = "udiv"; break;
        case Instruction::SDiv: Op = "sdiv"; break;
        case Instruction::URem: Op = "urem"; break;
        case Instruction::SRem: Op = "srem"; break;
        case Instruction::Mul: if (Width > 16) Op = "mul"; break;
        case Instruction::Shl: if (Width > 16 && !isa<ConstantInt>(BO->getOperand(1))) Op = "shl"; break;
        case Instruction::LShr: if (Width > 16 && !isa<ConstantInt>(BO->getOperand(1))) Op = "lshr"; break;
        case Instruction::AShr: if (Width > 16 && !isa<ConstantInt>(BO->getOperand(1))) Op = "ashr"; break;
        default: break;
        }
        if (Op.empty()) continue;
        unsigned HelperWidth = std::max(16u, Width);
        Type *HT = B.getIntNTy(HelperWidth);
        bool Signed = Op == "sdiv" || Op == "srem";
        Value *A = B.CreateIntCast(BO->getOperand(0), HT, Signed);
        Value *V = B.CreateIntCast(BO->getOperand(1), HT, Signed);
        auto Fn = helper(M, (Twine("__opencpu_") + Op + Twine(HelperWidth)).str(), HT, {HT, HT});
        Value *Call = B.CreateCall(Fn, {A, V});
        BO->replaceAllUsesWith(B.CreateIntCast(Call, Ty, Signed));
        BO->eraseFromParent(); Changed = true;
      } else if (auto *MI = dyn_cast<MemIntrinsic>(I)) {
        auto *I16 = B.getInt16Ty(); auto *Ptr = B.getPtrTy();
        Value *Length = memoryLength(MI, B);
        if (MI->isVolatile() || MI->isForceInlined()) {
          inlineMemory(MI, Length); Changed = true; continue;
        }
        if (auto *MT = dyn_cast<MemTransferInst>(MI)) {
          StringRef Name = isa<MemMoveInst>(MT) ? "memmove" : "memcpy";
          B.CreateCall(helper(M, Name, Ptr, {Ptr, Ptr, I16}), {MT->getDest(), MT->getSource(), Length});
        } else {
          auto *MS = cast<MemSetInst>(MI);
          B.CreateCall(helper(M, "memset", Ptr, {Ptr, I16, I16}),
                       {MS->getDest(), B.CreateZExt(MS->getValue(), I16), Length});
        }
        MI->eraseFromParent(); Changed = true;
      }
    }
    for (Function &F : M)
      if (F.isDeclaration() && !F.use_empty() &&
          (F.getName().starts_with("__opencpu_") || F.getName() == "memcpy" || F.getName() == "memmove" ||
           F.getName() == "memset" || F.getName() == "memcmp")) {
        if (F.getName().contains("mmio") || F.getName().contains("raw_")) continue;
        NeedRuntime = true;
      }
    return Changed;
  }
  void lowerMemory(Module &M) {
    SmallVector<Instruction *, 64> Work;
    for (Function &F : M) for (Instruction &I : instructions(F))
      if (isa<LoadInst>(I) || isa<StoreInst>(I)) Work.push_back(&I);
    auto &C = M.getContext(); Type *I16 = Type::getInt16Ty(C);
    auto Read = M.getOrInsertFunction("__opencpu_raw_load16", I16, I16);
    auto Write = M.getOrInsertFunction("__opencpu_raw_store16", Type::getVoidTy(C), I16, I16);
    for (Instruction *I : Work) {
      IRBuilder<> B(I);
      auto *L = dyn_cast<LoadInst>(I); auto *S = dyn_cast<StoreInst>(I);
      Type *T = L ? L->getType() : S->getValueOperand()->getType();
      bool Pointer = T->isPointerTy();
      if (!(Pointer || T->isIntegerTy(1) || T->isIntegerTy(8) || T->isIntegerTy(16) || T->isIntegerTy(32) || T->isIntegerTy(64)))
        reportOpenCPUError("memory supports scalar integers and data pointers only");
      if ((L && L->isAtomic()) || (S && S->isAtomic())) reportOpenCPUError("atomic memory is unsupported");
      unsigned Bits = Pointer ? 16 : T->getIntegerBitWidth();
      auto Alignment = L ? L->getAlign() : S->getAlign();
      if (Bits >= 16 && Alignment < Align(2)) reportOpenCPUError("wide scalar memory access requires two-byte alignment");
      Value *ByteAddress = B.CreatePtrToInt(L ? L->getPointerOperand() : S->getPointerOperand(), I16);
      Value *WordAddress = B.CreateLShr(ByteAddress, B.getInt16(1));
      if (Bits < 16) {
        Value *Shift = B.CreateShl(B.CreateAnd(ByteAddress, B.getInt16(1)), B.getInt16(3));
        Value *Old = B.CreateCall(Read, {WordAddress});
        if (L) {
          L->replaceAllUsesWith(B.CreateTrunc(B.CreateLShr(Old, Shift), T));
        } else {
          Value *Mask = B.CreateShl(B.getInt16(255), Shift);
          Value *V = B.CreateShl(B.CreateZExt(S->getValueOperand(), I16), Shift);
          Value *New = B.CreateOr(B.CreateAnd(Old, B.CreateNot(Mask)), V);
          B.CreateCall(Write, {WordAddress, New});
        }
      } else {
        Type *PackedType = Pointer ? I16 : T;
        Value *Result = ConstantInt::get(PackedType, 0);
        Value *Stored = L ? nullptr : (Pointer ? B.CreatePtrToInt(S->getValueOperand(), I16) : S->getValueOperand());
        for (unsigned Offset = 0; Offset < Bits / 16; ++Offset) {
          Value *Address = B.CreateAdd(WordAddress, B.getInt16(Offset));
          if (L) {
            Value *Word = B.CreateZExtOrTrunc(B.CreateCall(Read, {Address}), PackedType);
            if (Offset) Word = B.CreateShl(Word, ConstantInt::get(PackedType, Offset * 16));
            Result = B.CreateOr(Result, Word);
          } else {
            Value *Word = Offset ? B.CreateLShr(Stored, ConstantInt::get(PackedType, Offset * 16)) : Stored;
            B.CreateCall(Write, {Address, B.CreateZExtOrTrunc(Word, I16)});
          }
        }
        if (L) L->replaceAllUsesWith(Pointer ? B.CreateIntToPtr(Result, T) : Result);
      }
      I->eraseFromParent();
    }
  }
public:
  static char ID;
  OpenCPUPrepare() : ModulePass(ID) {}
  StringRef getPassName() const override { return "OpenCPU freestanding ABI preparation"; }
  bool runOnModule(Module &M) override {
    NeedRuntime = false; Addresses.clear(); Data.clear();
    for (Function &F : M)
      if (!F.isDeclaration() && F.getName().starts_with("__opencpu_"))
        reportOpenCPUError("function definition uses the reserved __opencpu_ prefix");
    if (!M.alias_empty() || !M.ifunc_empty() || !M.getModuleInlineAsm().empty())
      reportOpenCPUError("aliases, indirect functions and inline assembly are unsupported");
    lowerRuntime(M);
    if (NeedRuntime) {
      if (RuntimePath.empty()) reportOpenCPUError("this module requires -opencpu-runtime=<runtime.bc>");
      SMDiagnostic Error;
      auto Runtime = parseIRFile(RuntimePath, Error, M.getContext());
      if (!Runtime) reportOpenCPUError("unable to read runtime bitcode: " + RuntimePath);
      for (Function &F : *Runtime) if (!F.isDeclaration()) F.addFnAttr("opencpu-runtime");
      if (Linker::linkModules(M, std::move(Runtime), Linker::Flags::LinkOnlyNeeded))
        reportOpenCPUError("unable to link the required runtime helpers");
      for (Function &F : M) if (F.hasFnAttribute("opencpu-runtime") && !F.isDeclaration())
        F.setLinkage(GlobalValue::InternalLinkage);
    }
    const DataLayout &DL = M.getDataLayout();
    SmallVector<GlobalVariable *, 16> Globals;
    for (GlobalVariable &G : M.globals()) {
      if (G.getName().starts_with("__opencpu_") || G.getName() == "_start")
        reportOpenCPUError("global definition uses a reserved OpenCPU name");
      if (G.isDeclaration() || G.isThreadLocal()) reportOpenCPUError("unresolved or thread-local global: " + G.getName());
      if (!G.hasInitializer()) reportOpenCPUError("global has no initializer");
      if (!objectType(G.getValueType())) reportOpenCPUError("global objects require scalar integer, data pointer or array types");
      Globals.push_back(&G);
    }
    llvm::sort(Globals, [](auto *A, auto *B) { return A->getName() < B->getName(); });
    unsigned End = 2;
    for (auto *G : Globals) {
      Align A = G->getAlign().value_or(DL.getABITypeAlign(G->getValueType()));
      if (A > Align(2)) reportOpenCPUError("global alignment above two bytes is unsupported");
      End = alignTo(End, A.value()); Addresses[G] = End;
      auto Size = DL.getTypeAllocSize(G->getValueType());
      if (Size.isScalable() || Size > 2040 || End + Size > 2044) reportOpenCPUError("static data exceeds the 1024-word RAM");
      End += Size;
    }
    End = alignTo(End, 2);
    Data.resize(End, 0);
    for (auto *G : Globals) serialize(G->getInitializer(), G->getValueType(), Addresses[G], DL);
    M.addModuleFlag(Module::Override, "opencpu.data_end", End);
    for (auto *G : Globals) G->replaceAllUsesWith(ConstantExpr::getIntToPtr(ConstantInt::get(Type::getInt16Ty(M.getContext()), Addresses[G]), G->getType()));
    for (auto *G : Globals) G->eraseFromParent();
    if (M.getFunction("__opencpu_init_data")) reportOpenCPUError("reserved initialization function name");
    auto *Init = Function::Create(FunctionType::get(Type::getVoidTy(M.getContext()), false), GlobalValue::ExternalLinkage, "__opencpu_init_data", M);
    IRBuilder<> B(BasicBlock::Create(M.getContext(), "entry", Init));
    auto Write = M.getOrInsertFunction("__opencpu_raw_store16", B.getVoidTy(), B.getInt16Ty(), B.getInt16Ty());
    if (End > 2) {
      auto *Loop = BasicBlock::Create(M.getContext(), "zero", Init);
      auto *Done = BasicBlock::Create(M.getContext(), "values", Init);
      BasicBlock *Entry = B.GetInsertBlock(); B.CreateBr(Loop); B.SetInsertPoint(Loop);
      auto *Index = B.CreatePHI(B.getInt16Ty(), 2); Index->addIncoming(B.getInt16(1), Entry);
      B.CreateCall(Write, {Index, B.getInt16(0)});
      Value *Next = B.CreateAdd(Index, B.getInt16(1)); Index->addIncoming(Next, Loop);
      B.CreateCondBr(B.CreateICmpULT(Next, B.getInt16(End / 2)), Loop, Done);
      B.SetInsertPoint(Done);
      for (unsigned I = 2; I < End; I += 2) {
        unsigned V = Data[I] | (Data[I + 1] << 8);
        if (V) B.CreateCall(Write, {B.getInt16(I / 2), B.getInt16(V)});
      }
    }
    B.CreateRetVoid();
    lowerMemory(M);
    return true;
  }
};
char OpenCPUPrepare::ID;
} // namespace
ModulePass *createOpenCPUPreparePass() { return new OpenCPUPrepare; }
} // namespace llvm
