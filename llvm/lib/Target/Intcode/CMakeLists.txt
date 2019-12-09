set(LLVM_TARGET_DEFINITIONS Intcode.td)

tablegen(LLVM IntcodeGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM IntcodeGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM IntcodeGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM IntcodeGenDAGISel.inc -gen-dag-isel)
tablegen(LLVM IntcodeGenCallingConv.inc -gen-callingconv)
tablegen(LLVM IntcodeGenSubtargetInfo.inc -gen-subtarget)
tablegen(LLVM IntcodeGenMCCodeEmitter.inc -gen-emitter)
add_public_tablegen_target(IntcodeCommonTableGen)

add_llvm_target(IntcodeCodeGen
  IntcodeSubtarget.cpp
  IntcodeTargetMachine.cpp
  IntcodeRegisterInfo.cpp
  IntcodeInstrInfo.cpp
  IntcodeFrameLowering.cpp
  IntcodeISelLowering.cpp
  IntcodeSelectionDAGInfo.cpp
  IntcodeISelDAGToDAG.cpp
  IntcodeAsmPrinter.cpp
  IntcodeMCInstLower.cpp
  )

add_subdirectory(InstPrinter)
add_subdirectory(TargetInfo)
add_subdirectory(MCTargetDesc)
