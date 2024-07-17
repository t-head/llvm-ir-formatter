; RUN: llvm-irformatter -S -verify-pointers < %s

; ModuleID = 'LLVMDialectModule'
source_filename = "LLVMDialectModule"

declare i32 @__gxx_personality_v0(...)

declare fastcc void @cconv_fastcc()

declare void @cconv_ccc()

declare tailcc void @cconv_tailcc()

declare ghccc void @cconv_ghccc()

define void @test_ccs() {
  call fastcc void @cconv_fastcc()
  call void @cconv_ccc()
  call void @cconv_ccc()
  call tailcc void @cconv_tailcc()
  call ghccc void @cconv_ghccc()
  ret void
}

define void @test_ccs_invoke() personality ptr @__gxx_personality_v0 {
  invoke fastcc void @cconv_fastcc()
          to label %1 unwind label %6

1:                                                ; preds = %0
  invoke void @cconv_ccc()
          to label %2 unwind label %6

2:                                                ; preds = %1
  invoke void @cconv_ccc()
          to label %3 unwind label %6

3:                                                ; preds = %2
  invoke tailcc void @cconv_tailcc()
          to label %4 unwind label %6

4:                                                ; preds = %3
  invoke ghccc void @cconv_ghccc()
          to label %5 unwind label %6

5:                                                ; preds = %4
  ret void

6:                                                ; preds = %4, %3, %2, %1, %0
  %7 = landingpad { ptr, i32 }
          cleanup
  ret void
}

!llvm.module.flags = !{!0}

!0 = !{i32 2, !"Debug Info Version", i32 3}
