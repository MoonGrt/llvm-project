// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "OpenCPUTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
using namespace llvm;
Target &llvm::getTheOpenCPUTarget() { static Target T; return T; }
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeOpenCPUTargetInfo() {
  RegisterTarget<Triple::opencpu> X(getTheOpenCPUTarget(), "opencpu",
                                       "OpenCPU ISA v2 (LLVM ABI v2)", "OpenCPU");
}
