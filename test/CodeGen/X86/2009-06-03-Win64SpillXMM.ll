; RUN: llvm-as < %s | llc -o %t1
; RUN: grep "subq.*\\\$72, \\\%rsp" %t1
; RUN: grep "movaps	\\\%xmm8, 32\\\(\\\%rsp\\\)" %t1
; RUN: grep "movaps	\\\%xmm7, 48\\\(\\\%rsp\\\)" %t1
target triple = "x86_64-mingw64"

define i32 @a() nounwind {
entry:
	tail call void asm sideeffect "", "~{xmm7},~{xmm8},~{dirflag},~{fpsr},~{flags}"() nounwind
	ret i32 undef
}

