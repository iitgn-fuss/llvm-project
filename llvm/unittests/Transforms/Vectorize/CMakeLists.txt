add_subdirectory(SandboxVectorizer)

set(LLVM_LINK_COMPONENTS
  Analysis
  Core
  Vectorize
  AsmParser
  TargetParser
  )

add_llvm_unittest(VectorizeTests
  VPlanTest.cpp
  VPDomTreeTest.cpp
  VPPostDomFrontierTest.cpp
  VPlanHCFGTest.cpp
  VPlanPatternMatchTest.cpp
  SAVIOPortDescriptionTest.cpp
  VPlanVerifierTest.cpp
  )

target_compile_definitions(VectorizeTests PRIVATE
  SAVIO_TEST_INPUTS="${LLVM_MAIN_SRC_DIR}/test/Transforms/SAVIO/Inputs")
