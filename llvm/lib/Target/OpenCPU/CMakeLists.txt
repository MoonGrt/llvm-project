add_llvm_component_group(OpenCPU)
set(LLVM_TARGET_DEFINITIONS OpenCPU.td)
tablegen(LLVM OpenCPUGenDAGISel.inc -gen-dag-isel)
tablegen(LLVM OpenCPUGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM OpenCPUGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM OpenCPUGenSubtargetInfo.inc -gen-subtarget)
tablegen(LLVM OpenCPUGenSDNodeInfo.inc -gen-sd-node-info)
add_public_tablegen_target(OpenCPUCommonTableGen)
add_llvm_target(OpenCPUCodeGen
  OpenCPU.cpp
  OpenCPUPrepare.cpp
  OpenCPUAsmPrinter.cpp
  LINK_COMPONENTS Analysis AsmParser BitReader IRReader Linker IPO InstCombine AsmPrinter CodeGen CodeGenTypes Core MC
  OpenCPUDesc OpenCPUInfo Scalar SelectionDAG Support Target TargetParser TransformUtils
  ADD_TO_COMPONENT OpenCPU
)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
