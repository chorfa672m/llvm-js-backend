; RUN: llvm-as %s -o /dev/null -f
	
%Hosp = type { i32, i32, i32, { \2*, { i32, i32, i32, { [4 x \3], \2, \5, %Hosp, i32, i32 }* }*, \2* }, { \2*, { i32, i32, i32, { [4 x \3], \2, \5, %Hosp, i32, i32 }* }*, \2* }, { \2*, { i32, i32, i32, { [4 x \3], \2, \5, %Hosp, i32, i32 }* }*, \2* }, { \2*, { i32, i32, i32, { [4 x \3], \2, \5, %Hosp, i32, i32 }* }*, \2* } }
