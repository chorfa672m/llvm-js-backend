@ RUN: llvm-mc -mcpu=cortex-a8 -triple thumb-unknown-unkown -show-encoding < %s | FileCheck %s

.code 16

@ CHECK: vrecpe.u32	d16, d16        @ encoding: [0x20,0x04,0xfb,0xff]
	vrecpe.u32	d16, d16
@ CHECK: vrecpe.u32	q8, q8          @ encoding: [0x60,0x04,0xfb,0xff]
	vrecpe.u32	q8, q8
@ CHECK: vrecpe.f32	d16, d16        @ encoding: [0x20,0x05,0xfb,0xff]
	vrecpe.f32	d16, d16
@ CHECK: vrecpe.f32	q8, q8          @ encoding: [0x60,0x05,0xfb,0xff]
	vrecpe.f32	q8, q8
@ CHECK: vrecps.f32	d16, d16, d17   @ encoding: [0xb1,0x0f,0x40,0xef]
	vrecps.f32	d16, d16, d17
@ CHECK: vrecps.f32	q8, q8, q9      @ encoding: [0xf2,0x0f,0x40,0xef]
	vrecps.f32	q8, q8, q9
@ CHECK: vrsqrte.u32	d16, d16        @ encoding: [0xa0,0x04,0xfb,0xff]
	vrsqrte.u32	d16, d16
@ CHECK: vrsqrte.u32	q8, q8          @ encoding: [0xe0,0x04,0xfb,0xff]
	vrsqrte.u32	q8, q8
@ CHECK: vrsqrte.f32	d16, d16        @ encoding: [0xa0,0x05,0xfb,0xff]
	vrsqrte.f32	d16, d16
@ CHECK: vrsqrte.f32	q8, q8          @ encoding: [0xe0,0x05,0xfb,0xff]
	vrsqrte.f32	q8, q8
@ CHECK: vrsqrts.f32	d16, d16, d17   @ encoding: [0xb1,0x0f,0x60,0xef]
	vrsqrts.f32	d16, d16, d17
@ CHECK: vrsqrts.f32	q8, q8, q9      @ encoding: [0xf2,0x0f,0x60,0xef]
	vrsqrts.f32	q8, q8, q9
