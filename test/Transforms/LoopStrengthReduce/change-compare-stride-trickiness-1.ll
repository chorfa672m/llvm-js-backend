; RUN: llc %s -o - --x86-asm-syntax=att | grep {cmpq	\$8}
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128"
target triple = "x86_64-apple-darwin9"

; This is like change-compare-stride-trickiness-0.ll except the comparison
; happens after the relevant use, so the comparison stride can be
; easily changed.

define void @foo() nounwind {
entry:
	br label %loop

loop:
	%indvar = phi i32 [ 0, %entry ], [ %i.2.0.us1534, %loop ]		; <i32> [#uses=1]
	%i.2.0.us1534 = add i32 %indvar, 1		; <i32> [#uses=3]
	%tmp628.us1540 = shl i32 %i.2.0.us1534, 1		; <i32> [#uses=1]
	%tmp645646647.us1547 = sext i32 %tmp628.us1540 to i64		; <i64> [#uses=1]
	store i64 %tmp645646647.us1547, i64* null
	%tmp611.us1535 = icmp eq i32 %i.2.0.us1534, 4		; <i1> [#uses=2]
	%tmp623.us1538 = select i1 %tmp611.us1535, i32 6, i32 0		; <i32> [#uses=1]
	store i32 %tmp623.us1538, i32* null
	br i1 %tmp611.us1535, label %exit, label %loop

exit:
	ret void
}
