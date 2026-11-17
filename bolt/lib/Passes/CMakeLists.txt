add_llvm_library(LLVMBOLTPasses
  AArch64RelaxationPass.cpp
  Aligner.cpp
  AllocCombiner.cpp
  AsmDump.cpp
  BinaryPasses.cpp
  CMOVConversion.cpp
  CacheMetrics.cpp
  DataflowAnalysis.cpp
  DataflowInfoManager.cpp
  FrameAnalysis.cpp
  FrameOptimizer.cpp
  FixRelaxationPass.cpp
  FixRISCVCallsPass.cpp
  HFSort.cpp
  Hugify.cpp
  IdenticalCodeFolding.cpp
  IndirectCallPromotion.cpp
  Inliner.cpp
  Instrumentation.cpp
  JTFootprintReduction.cpp
  LongJmp.cpp
  LoopInversionPass.cpp
  LivenessAnalysis.cpp
  MCF.cpp
  PatchEntries.cpp
  PAuthGadgetScanner.cpp
  PresagePrefetch.cpp
  PettisAndHansen.cpp
  PLTCall.cpp
  PointerAuthCFIAnalyzer.cpp
  PointerAuthCFIFixup.cpp
  ProfileQualityStats.cpp
  RegAnalysis.cpp
  RegReAssign.cpp
  ReorderAlgorithm.cpp
  ReorderFunctions.cpp
  ReorderData.cpp
  ShrinkWrapping.cpp
  SplitFunctions.cpp
  StackAllocationAnalysis.cpp
  StackAvailableExpressions.cpp
  StackPointerTracking.cpp
  StackReachingUses.cpp
  StokeInfo.cpp
  TailDuplication.cpp
  ThreeWayBranch.cpp
  ValidateInternalCalls.cpp
  ValidateMemRefs.cpp
  VeneerElimination.cpp
  RetpolineInsertion.cpp

  NO_EXPORT
  DISABLE_LLVM_LINK_LLVM_DYLIB

  LINK_LIBS
  ${LLVM_PTHREAD_LIB}

  LINK_COMPONENTS
  AsmPrinter
  MC
  Support
  TargetParser
  TransformUtils
  )

target_link_libraries(LLVMBOLTPasses
  PRIVATE
  LLVMBOLTCore
  LLVMBOLTUtils
  )

# presage-repro: fixed x86-64 sequence emission needs X86 opcode definitions
# (pass is x86-64 only, mirrors the runtime BC.isX86() gate).  Both the
# source dir (MCTargetDesc/X86BaseInfo.h) and the tablegen output dir
# (X86Gen*.inc) are required.
target_include_directories(LLVMBOLTPasses PRIVATE
  ${LLVM_MAIN_SRC_DIR}/lib/Target/X86
  ${CMAKE_BINARY_DIR}/lib/Target/X86
  )
