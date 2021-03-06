@ RUN: llvm-mc -triple thumbv6-apple-darwin -show-encoding < %s | FileCheck %s
        .code 16

@ CHECK: cmp	r1, r2               @ encoding: [0x91,0x42]
        cmp     r1, r2

@ CHECK: pop    {r1, r2, r4}         @ encoding: [0x16,0xbc]
        pop     {r1, r2, r4}

@ CHECK: trap                        @ encoding: [0xfe,0xde]
        trap

@ CHECK: blx	r9                   @ encoding: [0xc8,0x47]
	blx	r9

@ CHECK: rev	r2, r3               @ encoding: [0x1a,0xba]
@ CHECK: rev16	r3, r4               @ encoding: [0x63,0xba]
@ CHECK: revsh	r5, r6               @ encoding: [0xf5,0xba]
        rev     r2, r3
        rev16   r3, r4
        revsh   r5, r6

@ CHECK: sxtb	r2, r3               @ encoding: [0x5a,0xb2]
@ CHECK: sxth	r2, r3               @ encoding: [0x1a,0xb2]
	sxtb	r2, r3
	sxth	r2, r3

@ CHECK: tst	r4, r5               @ encoding: [0x2c,0x42]
	tst	r4, r5

@ CHECK: uxtb	r3, r6               @ encoding: [0xf3,0xb2]
@ CHECK: uxth	r3, r6               @ encoding: [0xb3,0xb2]
	uxtb	r3, r6
	uxth	r3, r6

@ CHECK: ldr	r3, [r1, r2]         @ encoding: [0x8b,0x58]
	ldr	r3, [r1, r2]
