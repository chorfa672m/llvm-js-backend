; RUN: llvm-as < %s | llc -march=c | not grep fixarray_array3
; PR918

%structtype_s = type { i32 }
%fixarray_array3 = type [3 x %structtype_s]

define i32 %witness(%fixarray_array3* %p) {
    %q = getelementptr %fixarray_array3* %p, i32 0, i32 0, i32 0
    %v = load i32* %q
    ret i32 %v
}
