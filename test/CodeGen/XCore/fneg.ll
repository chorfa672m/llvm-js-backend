; RUN: llvm-as < %s | llc -march=xcore > %t1.s
; RUN: grep "xor" %t1.s | count 1
define i1 @test(double %F) nounwind {
entry:
	%0 = fsub double -0.000000e+00, %F
	%1 = fcmp olt double 0.000000e+00, %0
	ret i1 %1
}
