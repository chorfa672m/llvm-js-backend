//===-- ARMTargetMachine.h - Define TargetMachine for ARM -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the "Instituto Nokia de Tecnologia" and
// is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares the ARM specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//

#ifndef ARMTARGETMACHINE_H
#define ARMTARGETMACHINE_H

#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetFrameInfo.h"
#include "ARMInstrInfo.h"
#include "ARMFrameInfo.h"
#include "ARMJITInfo.h"
#include "ARMSubtarget.h"
#include "ARMISelLowering.h"

namespace llvm {

class Module;

class ARMTargetMachine : public LLVMTargetMachine {
  ARMSubtarget      Subtarget;
  const TargetData  DataLayout;       // Calculates type size & alignment
  ARMInstrInfo      InstrInfo;
  ARMFrameInfo      FrameInfo;
  ARMJITInfo        JITInfo;
  ARMTargetLowering TLInfo;

public:
  ARMTargetMachine(const Module &M, const std::string &FS, bool isThumb = false);

  virtual const ARMInstrInfo *getInstrInfo() const { return &InstrInfo; }
  virtual const TargetFrameInfo  *getFrameInfo() const { return &FrameInfo; }
  virtual       TargetJITInfo    *getJITInfo()         { return &JITInfo; }
  virtual const MRegisterInfo *getRegisterInfo() const {
    return &InstrInfo.getRegisterInfo();
  }
  virtual const TargetData       *getTargetData() const { return &DataLayout; }
  virtual const ARMSubtarget  *getSubtargetImpl() const { return &Subtarget; }
  virtual       ARMTargetLowering *getTargetLowering() const { 
    return const_cast<ARMTargetLowering*>(&TLInfo); 
  }
  static unsigned getModuleMatchQuality(const Module &M);
  static unsigned getJITMatchQuality();

  virtual const TargetAsmInfo *createTargetAsmInfo() const;
  
  // Pass Pipeline Configuration
  virtual bool addInstSelector(FunctionPassManager &PM, bool Fast);
  virtual bool addPreEmitPass(FunctionPassManager &PM, bool Fast);
  virtual bool addAssemblyEmitter(FunctionPassManager &PM, bool Fast, 
                                  std::ostream &Out);
  virtual bool addCodeEmitter(FunctionPassManager &PM, bool Fast,
                              MachineCodeEmitter &MCE);
  virtual bool addSimpleCodeEmitter(FunctionPassManager &PM, bool Fast,
                                    MachineCodeEmitter &MCE);
};

/// ThumbTargetMachine - Thumb target machine.
///
class ThumbTargetMachine : public ARMTargetMachine {
public:
  ThumbTargetMachine(const Module &M, const std::string &FS);

  static unsigned getJITMatchQuality();
  static unsigned getModuleMatchQuality(const Module &M);
};

} // end namespace llvm

#endif
