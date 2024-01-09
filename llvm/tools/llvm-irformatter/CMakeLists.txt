set(LLVM_LINK_COMPONENTS
  Analysis
  BitWriter
  Core
  IPO
  IRReader
  IRPrinter
  Passes
  Support
  )

add_llvm_tool(llvm-irformatter
  llvm-irformatter.cpp

  DEPENDS
  intrinsics_gen
  )
