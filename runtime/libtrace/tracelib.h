/*===-- Libraries/tracelib.h - Runtime routines for tracing -----*- C++ -*--===
 *
 * Runtime routines for supporting tracing of execution
 * for code generated by LLVM.
 *
 *===---------------------------------------------------------------------===*/

#ifndef _TEST_LIBRARIES_LIBINSTR_TRACELIB_
#define _TEST_LIBRARIES_LIBINSTR_TRACELIB_

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/*===---------------------------------------------------------------------=====
 * Support for tracing pointers
 *===---------------------------------------------------------------------===*/

typedef unsigned int SequenceNumber;

extern SequenceNumber HashPointerToSeqNum( char* ptr);

extern void           ReleasePointerSeqNum(char* ptr);

extern void           RecordPointer(char* ptr);

extern void           PushPointerSet();

extern void           ReleasePointersPopSet();


#ifdef __cplusplus
}
#endif

/*===---------------------------------------------------------------------===*/

#endif 
