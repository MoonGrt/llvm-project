// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#ifndef LLVM_LIB_TARGET_OPENCPU_H
#define LLVM_LIB_TARGET_OPENCPU_H
#define GET_REGINFO_ENUM
#include "OpenCPUGenRegisterInfo.inc"
#define GET_INSTRINFO_ENUM
#include "OpenCPUGenInstrInfo.inc"
#define GET_SUBTARGETINFO_ENUM
#include "OpenCPUGenSubtargetInfo.inc"
namespace llvm {
class Twine;
[[noreturn]] void reportOpenCPUError(const Twine &Message);
}
#endif
