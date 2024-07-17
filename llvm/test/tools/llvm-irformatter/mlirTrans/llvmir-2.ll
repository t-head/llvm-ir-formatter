; RUN: llvm-irformatter -S -verify-pointers < %s

; ModuleID = 'LLVMDialectModule'
source_filename = "LLVMDialectModule"

declare void @foo()

declare i32 @__gxx_personality_v0(...)

define i32 @invoke_phis() personality ptr @__gxx_personality_v0 {
  invoke void @foo()
          to label %1 unwind label %3

1:                                                ; preds = %3, %0
  %2 = phi i32 [ 1, %3 ], [ 0, %0 ]
  ret i32 %2

3:                                                ; preds = %0
  %4 = landingpad { ptr, i32 }
          cleanup
  br label %1
}

!llvm.module.flags = !{!0}

!0 = !{i32 2, !"Debug Info Version", i32 3}

