//===-- PIC16AsmPrinter.h - PIC16 LLVM assembly writer ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to PIC16 assembly language.
//
//===----------------------------------------------------------------------===//

#ifndef PIC16ASMPRINTER_H
#define PIC16ASMPRINTER_H

#include "PIC16.h"
#include "PIC16TargetMachine.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetAsmInfo.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {
  struct VISIBILITY_HIDDEN PIC16AsmPrinter : public AsmPrinter {
    explicit PIC16AsmPrinter(raw_ostream &O, PIC16TargetMachine &TM,
                             const TargetAsmInfo *T, CodeGenOpt::Level OL,
                             bool V)
      : AsmPrinter(O, TM, T, OL, V) {
      CurBank = "";
      FunctionLabelBegin = '@';
      IsRomData = false;
      PTLI = TM.getTargetLowering();
    }
    private :
    virtual const char *getPassName() const {
      return "PIC16 Assembly Printer";
    }

    bool runOnMachineFunction(MachineFunction &F);
    void printOperand(const MachineInstr *MI, int opNum);
    void printCCOperand(const MachineInstr *MI, int opNum);
    bool printInstruction(const MachineInstr *MI); // definition autogenerated.
    bool printMachineInstruction(const MachineInstr *MI);
    void EmitExternsAndGlobals (Module &M);
    void EmitInitData (Module &M);
    void EmitUnInitData (Module &M);
    void EmitRomData (Module &M);
    void emitFunctionData(MachineFunction &MF);

    protected:
    bool doInitialization(Module &M);
    bool doFinalization(Module &M);

    private:
    PIC16TargetLowering *PTLI;
    std::string CurBank;
    bool IsRomData;
    char FunctionLabelBegin;
  };
} // end of namespace

#endif
