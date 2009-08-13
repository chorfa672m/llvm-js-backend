//=====-- PPCTargetAsmInfo.h - PPC asm properties -------------*- C++ -*--====//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the declaration of the DarwinTargetAsmInfo class.
//
//===----------------------------------------------------------------------===//

#ifndef PPCTARGETASMINFO_H
#define PPCTARGETASMINFO_H

#include "llvm/Target/DarwinTargetAsmInfo.h"

namespace llvm {

  struct PPCDarwinTargetAsmInfo : public DarwinTargetAsmInfo {
    explicit PPCDarwinTargetAsmInfo(const Triple &TheTriple);
  };

  struct PPCLinuxTargetAsmInfo : public TargetAsmInfo {
    explicit PPCLinuxTargetAsmInfo(const Triple &TheTriple);
  };

} // namespace llvm

#endif
