; This test makes sure that these instructions are properly eliminated.
;

; RUN: if as < %s | opt -instcombine | dis | grep or
; RUN: then exit 1
; RUN: else exit 0
; RUN: fi

implementation

int "test1"(int %A) {
	%B = or int %A, 0
	ret int %B
}

int "test2"(int %A) {
	%B = or int %A, -1
	ret int %B
}

bool "test3"(bool %A) {
	%B = or bool %A, false
	ret bool %B
}

bool "test4"(bool %A) {
	%B = or bool %A, true
	ret bool %B
}

bool "test5"(bool %A) {
	%B = xor bool %A, false
	ret bool %B
}

int "test6"(int %A) {
	%B = xor int %A, 0
	ret int %B
}

bool "test7"(bool %A) {
	%B = xor bool %A, %A
	ret bool %B
}

int "test8"(int %A) {
	%B = xor int %A, %A
	ret int %B
}

bool "test9"(bool %A) {
	%B = or bool %A, %A
	ret bool %B
}

int "test10"(int %A) {
	%B = or int %A, %A
	ret int %B
}

