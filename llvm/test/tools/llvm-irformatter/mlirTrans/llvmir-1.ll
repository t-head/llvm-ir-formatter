; RUN: llvm-irformatter -S -verify-pointers < %s

; ModuleID = 'LLVMDialectModule'
source_filename = "LLVMDialectModule"

declare i8 @foo()

declare i32 @__gxx_personality_v0(...)

define void @invoke_result(ptr %0) personality ptr @__gxx_personality_v0 {
  %2 = invoke i8 @foo()
          to label %3 unwind label %4

3:                                                ; preds = %1
  store i8 %2, ptr %0, align 1
  ret void

4:                                                ; preds = %1
  %5 = landingpad { ptr, i32 }
          cleanup
  ret void
}

!llvm.module.flags = !{!0}

!0 = !{i32 2, !"Debug Info Version", i32 3}

