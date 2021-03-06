//===-- PTXTargetMachine.cpp - Define TargetMachine for PTX ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Top-level implementation for the PTX target.
//
//===----------------------------------------------------------------------===//

#include "PTX.h"
#include "PTXMCAsmInfo.h"
#include "PTXTargetMachine.h"
#include "llvm/PassManager.h"
#include "llvm/Target/TargetRegistry.h"

using namespace llvm;

namespace llvm {
  MCStreamer *createPTXAsmStreamer(MCContext &Ctx, formatted_raw_ostream &OS,
                                   bool isVerboseAsm, bool useLoc,
                                   MCInstPrinter *InstPrint,
                                   MCCodeEmitter *CE,
                                   bool ShowInst);
}

extern "C" void LLVMInitializePTXTarget() {
  RegisterTargetMachine<PTXTargetMachine> X(ThePTXTarget);
  RegisterAsmInfo<PTXMCAsmInfo> Y(ThePTXTarget);
  TargetRegistry::RegisterAsmStreamer(ThePTXTarget, createPTXAsmStreamer);
}

// DataLayout and FrameInfo are filled with dummy data
PTXTargetMachine::PTXTargetMachine(const Target &T,
                                   const std::string &TT,
                                   const std::string &FS)
  : LLVMTargetMachine(T, TT),
    DataLayout("e-p:32:32-i64:32:32-f64:32:32-v128:32:128-v64:32:64-n32:64"),
    FrameInfo(Subtarget),
    InstrInfo(*this),
    TLInfo(*this),
    Subtarget(TT, FS) {
}

bool PTXTargetMachine::addInstSelector(PassManagerBase &PM,
                                       CodeGenOpt::Level OptLevel) {
  PM.add(createPTXISelDag(*this, OptLevel));
  PM.add(createPTXMFInfoExtract(*this, OptLevel));
  return false;
}
