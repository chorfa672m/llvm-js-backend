//===-- include/Support/DataTypes.h - Define fixed size types ----*- C++ -*--=//
//
// This file contains definitions to figure out the size of _HOST_ data types.
// This file is important because different host OS's define different macros,
// which makes portability tough.  This file exports the following definitions:
//
//   ENDIAN_LITTLE : is #define'd if the host is little endian
//   int64_t       : is a typedef for the signed 64 bit system type
//   uint64_t      : is a typedef for the unsigned 64 bit system type
//   INT64_MAX     : is a #define specifying the max value for int64_t's
//
// No library is required when using these functinons.
//
//===----------------------------------------------------------------------===//

// TODO: This file sucks.  Not only does it not work, but this stuff should be
// autoconfiscated anyways. Major FIXME

#ifndef SUPPORT_DATATYPES_H
#define SUPPORT_DATATYPES_H

#define __STDC_LIMIT_MACROS 1
#include <inttypes.h>

#ifdef __linux__
#  include <endian.h>
#  if BYTE_ORDER == LITTLE_ENDIAN
#    undef BIG_ENDIAN
#  else
#    undef LITTLE_ENDIAN
#  endif
#endif

#ifdef __FreeBSD__
#  include <machine/endian.h>
#  if _BYTE_ORDER == _LITTLE_ENDIAN
#    ifndef LITTLE_ENDIAN
#      define LITTLE_ENDIAN 1
#    endif
#    ifdef BIG_ENDIAN
#      undef BIG_ENDIAN
#    endif
#  else
#    ifndef BIG_ENDIAN
#      define BIG_ENDIAN 1
#    endif
#    ifdef LITTLE_ENDIAN
#      undef LITTLE_ENDIAN
#    endif
#  endif
#endif

#ifdef __sparc__
#  include <sys/types.h>
#  ifdef _LITTLE_ENDIAN
#    define LITTLE_ENDIAN 1
#  else
#    define BIG_ENDIAN 1
#  endif
#endif

//
// Convert the information from the header files into our own local
// endian macros.  We do this because various strange systems define both
// BIG_ENDIAN and LITTLE_ENDIAN, and we don't want to conflict with them.
//
// Don't worry; once we introduce autoconf, this will look a lot nicer.
// 
#ifdef LITTLE_ENDIAN
#define ENDIAN_LITTLE
#endif

#ifdef BIG_ENDIAN
#define ENDIAN_BIG
#endif

#if (defined(ENDIAN_LITTLE) && defined(ENDIAN_BIG))
#error "Cannot define both ENDIAN_LITTLE and ENDIAN_BIG!"
#endif

#if (!defined(ENDIAN_LITTLE) && !defined(ENDIAN_BIG)) || !defined(INT64_MAX)
#error "include/Support/DataTypes.h could not determine endianness!"
#endif

#endif  /* SUPPORT_DATATYPES_H */
