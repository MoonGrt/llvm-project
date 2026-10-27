// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#ifndef LLVM_CLANG_LIB_BASIC_TARGETS_OPENCPU_H
#define LLVM_CLANG_LIB_BASIC_TARGETS_OPENCPU_H
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
namespace clang::targets {
class LLVM_LIBRARY_VISIBILITY OpenCPUTargetInfo : public TargetInfo {
public:
  OpenCPUTargetInfo(const llvm::Triple &T, const TargetOptions &) : TargetInfo(T) {
    TLSSupported = false;
    VLASupported = HasMustTail = false;
    IntWidth = IntAlign = ShortWidth = ShortAlign = 16;
    LongWidth = 32; LongLongWidth = 64;
    LongAlign = LongLongAlign = 16;
    FloatWidth = 32; FloatAlign = 16;
    DoubleWidth = LongDoubleWidth = 64;
    DoubleAlign = LongDoubleAlign = 16;
    LongDoubleFormat = &llvm::APFloat::IEEEdouble();
    PointerWidth = PointerAlign = SuitableAlign = 16;
    SizeType = UnsignedInt; PtrDiffType = IntPtrType = SignedInt;
    IntMaxType = SignedLongLong; WCharType = SignedInt;
    Char16Type = UnsignedShort; Char32Type = UnsignedLong;
    MaxAtomicPromoteWidth = MaxAtomicInlineWidth = 0;
    resetDataLayout();
  }
  void getTargetDefines(const LangOptions &, MacroBuilder &) const override;
  llvm::SmallVector<Builtin::InfosShard> getTargetBuiltins() const override;
  ArrayRef<const char *> getGCCRegNames() const override;
  ArrayRef<GCCRegAlias> getGCCRegAliases() const override { return {}; }
  bool validateAsmConstraint(const char *&, ConstraintInfo &) const override { return false; }
  std::string_view getClobbers() const override { return ""; }
  BuiltinVaListKind getBuiltinVaListKind() const override { return CharPtrBuiltinVaList; }
  bool allowsLargerPreferedTypeAlignment() const override { return false; }
  bool hasFeature(StringRef F) const override { return F == "opencpu"; }
};
}
#endif
