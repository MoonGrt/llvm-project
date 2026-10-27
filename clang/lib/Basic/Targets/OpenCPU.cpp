// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "OpenCPU.h"
#include "clang/Basic/MacroBuilder.h"
#include "clang/Basic/TargetBuiltins.h"
using namespace clang;
using namespace clang::targets;
static constexpr int NumBuiltins = OpenCPU::LastTSBuiltin - Builtin::FirstTSBuiltin;
static constexpr llvm::StringTable BuiltinStrings =
    CLANG_BUILTIN_STR_TABLE_START
#define BUILTIN CLANG_BUILTIN_STR_TABLE
#include "clang/Basic/BuiltinsOpenCPU.def"
    ;
static constexpr auto BuiltinInfos = Builtin::MakeInfos<NumBuiltins>({
#define BUILTIN CLANG_BUILTIN_ENTRY
#include "clang/Basic/BuiltinsOpenCPU.def"
});
void OpenCPUTargetInfo::getTargetDefines(const LangOptions &, MacroBuilder &B) const {
  B.defineMacro("__opencpu__");
  B.defineMacro("__OPENCPU__");
  B.defineMacro("__OPENCPU_ABI__", "2");
  B.defineMacro("__STDC_NO_ATOMICS__", "1");
  B.defineMacro("__STDC_NO_THREADS__", "1");
}
llvm::SmallVector<Builtin::InfosShard> OpenCPUTargetInfo::getTargetBuiltins() const {
  return {{&BuiltinStrings, BuiltinInfos}};
}
ArrayRef<const char *> OpenCPUTargetInfo::getGCCRegNames() const {
  static const char *const Names[] = {"zero", "a1", "a2", "a3", "a4", "fp", "ra", "sp"};
  return Names;
}
