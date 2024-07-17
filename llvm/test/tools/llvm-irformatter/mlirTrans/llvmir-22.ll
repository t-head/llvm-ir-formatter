; RUN: llvm-irformatter -S -verify-pointers < %s

; ModuleID = 'LLVMDialectModule'
source_filename = "LLVMDialectModule"

declare void @foo()

declare i32 @__gxx_personality_v0(...)

define i32 @invoke_branch_weights() personality ptr @__gxx_personality_v0 {
  invoke void @foo()
          to label %3 unwind label %1, !prof !1

1:                                                ; preds = %0
  %2 = landingpad { ptr, i32 }
          cleanup
  br label %3

3:                                                ; preds = %1, %0
  ret i32 1
}

!llvm.module.flags = !{!0}

!0 = !{i32 2, !"Debug Info Version", i32 3}
!1 = !{!"branch_weights", i32 42, i32 99}

