//===-- RegAllocCommon.h --------------------------------------------------===//
// 
//  Shared declarations for register allocation.
// 
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_REGALLOCCOMMON_H
#define LLVM_CODEGEN_REGALLOCCOMMON_H

#include "Support/CommandLine.h"

enum RegAllocDebugLevel_t {
  RA_DEBUG_None         = 0,
  RA_DEBUG_Results      = 1,
  RA_DEBUG_Coloring     = 2,
  RA_DEBUG_Interference = 3,
  RA_DEBUG_LiveRanges   = 4,
  RA_DEBUG_Verbose      = 5
};

extern RegAllocDebugLevel_t DEBUG_RA;

#endif
