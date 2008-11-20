//===-- SPUAsmPrinter.cpp - Print machine instrs to Cell SPU assembly -------=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to Cell SPU assembly language. This printer
// is the output mechanism used by `llc'.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "asmprinter"
#include "SPU.h"
#include "SPUTargetMachine.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/Assembly/Writer.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/DwarfWriter.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Support/Mangler.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetAsmInfo.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include <set>
using namespace llvm;

namespace {
  STATISTIC(EmittedInsts, "Number of machine instrs printed");

  const std::string bss_section(".bss");

  struct VISIBILITY_HIDDEN SPUAsmPrinter : public AsmPrinter {
    std::set<std::string> FnStubs, GVStubs;

    SPUAsmPrinter(raw_ostream &O, TargetMachine &TM, const TargetAsmInfo *T) :
      AsmPrinter(O, TM, T)
    {
    }

    virtual const char *getPassName() const {
      return "STI CBEA SPU Assembly Printer";
    }

    SPUTargetMachine &getTM() {
      return static_cast<SPUTargetMachine&>(TM);
    }

    /// printInstruction - This method is automatically generated by tablegen
    /// from the instruction set description.  This method returns true if the
    /// machine instruction was sufficiently described to print it, otherwise it
    /// returns false.
    bool printInstruction(const MachineInstr *MI);

    void printMachineInstruction(const MachineInstr *MI);
    void printOp(const MachineOperand &MO);

    /// printRegister - Print register according to target requirements.
    ///
    void printRegister(const MachineOperand &MO, bool R0AsZero) {
      unsigned RegNo = MO.getReg();
      assert(TargetRegisterInfo::isPhysicalRegister(RegNo) &&
             "Not physreg??");
      O << TM.getRegisterInfo()->get(RegNo).AsmName;
    }

    void printOperand(const MachineInstr *MI, unsigned OpNo) {
      const MachineOperand &MO = MI->getOperand(OpNo);
      if (MO.isReg()) {
        assert(TargetRegisterInfo::isPhysicalRegister(MO.getReg())&&"Not physreg??");
        O << TM.getRegisterInfo()->get(MO.getReg()).AsmName;
      } else if (MO.isImm()) {
        O << MO.getImm();
      } else {
        printOp(MO);
      }
    }
    
    bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                         unsigned AsmVariant, const char *ExtraCode);
    bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                               unsigned AsmVariant, const char *ExtraCode);
   
   
    void
    printS7ImmOperand(const MachineInstr *MI, unsigned OpNo)
    {
      int value = MI->getOperand(OpNo).getImm();
      value = (value << (32 - 7)) >> (32 - 7);

      assert((value >= -(1 << 8) && value <= (1 << 7) - 1)
             && "Invalid s7 argument");
      O << value;
    }

    void
    printU7ImmOperand(const MachineInstr *MI, unsigned OpNo)
    {
      unsigned int value = MI->getOperand(OpNo).getImm();
      assert(value < (1 << 8) && "Invalid u7 argument");
      O << value;
    }
 
    void
    printMemRegImmS7(const MachineInstr *MI, unsigned OpNo)
    {
      char value = MI->getOperand(OpNo).getImm();
      O << (int) value;
      O << "(";
      printOperand(MI, OpNo+1);
      O << ")";
    }

    void
    printS16ImmOperand(const MachineInstr *MI, unsigned OpNo)
    {
      O << (short) MI->getOperand(OpNo).getImm();
    }

    void
    printU16ImmOperand(const MachineInstr *MI, unsigned OpNo)
    {
      O << (unsigned short)MI->getOperand(OpNo).getImm();
    }

    void
    printU32ImmOperand(const MachineInstr *MI, unsigned OpNo)
    {
      O << (unsigned)MI->getOperand(OpNo).getImm();
    }
    
    void
    printMemRegReg(const MachineInstr *MI, unsigned OpNo) {
      // When used as the base register, r0 reads constant zero rather than
      // the value contained in the register.  For this reason, the darwin
      // assembler requires that we print r0 as 0 (no r) when used as the base.
      const MachineOperand &MO = MI->getOperand(OpNo);
      O << TM.getRegisterInfo()->get(MO.getReg()).AsmName;
      O << ", ";
      printOperand(MI, OpNo+1);
    }

    void
    printU18ImmOperand(const MachineInstr *MI, unsigned OpNo)
    {
      unsigned int value = MI->getOperand(OpNo).getImm();
      assert(value <= (1 << 19) - 1 && "Invalid u18 argument");
      O << value;
    }

    void
    printS10ImmOperand(const MachineInstr *MI, unsigned OpNo)
    {
      short value = (short) (((int) MI->getOperand(OpNo).getImm() << 16)
                             >> 16);
      assert((value >= -(1 << 9) && value <= (1 << 9) - 1)
             && "Invalid s10 argument");
      O << value;
    }

    void
    printU10ImmOperand(const MachineInstr *MI, unsigned OpNo)
    {
      short value = (short) (((int) MI->getOperand(OpNo).getImm() << 16)
                             >> 16);
      assert((value <= (1 << 10) - 1) && "Invalid u10 argument");
      O << value;
    }

    void
    printMemRegImmS10(const MachineInstr *MI, unsigned OpNo)
    {
      const MachineOperand &MO = MI->getOperand(OpNo);
      assert(MO.isImm() &&
             "printMemRegImmS10 first operand is not immedate");
      int64_t value = int64_t(MI->getOperand(OpNo).getImm());
      int16_t value16 = int16_t(value);
      assert((value16 >= -(1 << (9+4)) && value16 <= (1 << (9+4)) - 1)
             && "Invalid dform s10 offset argument");
      O << value16 << "(";
      printOperand(MI, OpNo+1);
      O << ")";
    }

    void
    printAddr256K(const MachineInstr *MI, unsigned OpNo)
    {
      /* Note: operand 1 is an offset or symbol name. */
      if (MI->getOperand(OpNo).isImm()) {
        printS16ImmOperand(MI, OpNo);
      } else {
        printOp(MI->getOperand(OpNo));
        if (MI->getOperand(OpNo+1).isImm()) {
          int displ = int(MI->getOperand(OpNo+1).getImm());
          if (displ > 0)
            O << "+" << displ;
          else if (displ < 0)
            O << displ;
        }
      }
    }

    void printCallOperand(const MachineInstr *MI, unsigned OpNo) {
      printOp(MI->getOperand(OpNo));
    }

    void printPCRelativeOperand(const MachineInstr *MI, unsigned OpNo) {
      printOp(MI->getOperand(OpNo));
      O << "-.";
    }

    void printSymbolHi(const MachineInstr *MI, unsigned OpNo) {
      if (MI->getOperand(OpNo).isImm()) {
        printS16ImmOperand(MI, OpNo);
      } else {
        printOp(MI->getOperand(OpNo));
        O << "@h";
      }
    }

    void printSymbolLo(const MachineInstr *MI, unsigned OpNo) {
      if (MI->getOperand(OpNo).isImm()) {
        printS16ImmOperand(MI, OpNo);
      } else {
        printOp(MI->getOperand(OpNo));
        O << "@l";
      }
    }

    /// Print local store address
    void printSymbolLSA(const MachineInstr *MI, unsigned OpNo) {
      printOp(MI->getOperand(OpNo));
    }

    void printROTHNeg7Imm(const MachineInstr *MI, unsigned OpNo) {
      if (MI->getOperand(OpNo).isImm()) {
        int value = (int) MI->getOperand(OpNo).getImm();
        assert((value >= 0 && value < 16)
               && "Invalid negated immediate rotate 7-bit argument");
        O << -value;
      } else {
        assert(0 &&"Invalid/non-immediate rotate amount in printRotateNeg7Imm");
      }
    }

    void printROTNeg7Imm(const MachineInstr *MI, unsigned OpNo) {
      if (MI->getOperand(OpNo).isImm()) {
        int value = (int) MI->getOperand(OpNo).getImm();
        assert((value >= 0 && value < 32)
               && "Invalid negated immediate rotate 7-bit argument");
        O << -value;
      } else {
        assert(0 &&"Invalid/non-immediate rotate amount in printRotateNeg7Imm");
      }
    }

    virtual bool runOnMachineFunction(MachineFunction &F) = 0;
    //! Assembly printer cleanup after function has been emitted
    virtual bool doFinalization(Module &M) = 0;
  };

  /// LinuxAsmPrinter - SPU assembly printer, customized for Linux
  struct VISIBILITY_HIDDEN LinuxAsmPrinter : public SPUAsmPrinter {
  
    DwarfWriter DW;
    MachineModuleInfo *MMI;

    LinuxAsmPrinter(raw_ostream &O, SPUTargetMachine &TM,
                    const TargetAsmInfo *T) :
      SPUAsmPrinter(O, TM, T),
      DW(O, this, T),
      MMI(0)
    { }

    virtual const char *getPassName() const {
      return "STI CBEA SPU Assembly Printer";
    }
    
    bool runOnMachineFunction(MachineFunction &F);
    bool doInitialization(Module &M);
    //! Dump globals, perform cleanup after function emission
    bool doFinalization(Module &M);
    
    void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.setPreservesAll();
      AU.addRequired<MachineModuleInfo>();
      SPUAsmPrinter::getAnalysisUsage(AU);
    }

    //! Emit a global variable according to its section and type
    void printModuleLevelGV(const GlobalVariable* GVar);
  };
} // end of anonymous namespace

// Include the auto-generated portion of the assembly writer
#include "SPUGenAsmWriter.inc"

void SPUAsmPrinter::printOp(const MachineOperand &MO) {
  switch (MO.getType()) {
  case MachineOperand::MO_Immediate:
    cerr << "printOp() does not handle immediate values\n";
    abort();
    return;

  case MachineOperand::MO_MachineBasicBlock:
    printBasicBlockLabel(MO.getMBB());
    return;
  case MachineOperand::MO_JumpTableIndex:
    O << TAI->getPrivateGlobalPrefix() << "JTI" << getFunctionNumber()
      << '_' << MO.getIndex();
    return;
  case MachineOperand::MO_ConstantPoolIndex:
    O << TAI->getPrivateGlobalPrefix() << "CPI" << getFunctionNumber()
      << '_' << MO.getIndex();
    return;
  case MachineOperand::MO_ExternalSymbol:
    // Computing the address of an external symbol, not calling it.
    if (TM.getRelocationModel() != Reloc::Static) {
      std::string Name(TAI->getGlobalPrefix()); Name += MO.getSymbolName();
      GVStubs.insert(Name);
      O << "L" << Name << "$non_lazy_ptr";
      return;
    }
    O << TAI->getGlobalPrefix() << MO.getSymbolName();
    return;
  case MachineOperand::MO_GlobalAddress: {
    // Computing the address of a global symbol, not calling it.
    GlobalValue *GV = MO.getGlobal();
    std::string Name = Mang->getValueName(GV);

    // External or weakly linked global variables need non-lazily-resolved
    // stubs
    if (TM.getRelocationModel() != Reloc::Static) {
      if (((GV->isDeclaration() || GV->hasWeakLinkage() ||
            GV->hasLinkOnceLinkage() || GV->hasCommonLinkage()))) {
        GVStubs.insert(Name);
        O << "L" << Name << "$non_lazy_ptr";
        return;
      }
    }
    O << Name;
    
    if (GV->hasExternalWeakLinkage())
      ExtWeakSymbols.insert(GV);
    return;
  }

  default:
    O << "<unknown operand type: " << MO.getType() << ">";
    return;
  }
}

/// PrintAsmOperand - Print out an operand for an inline asm expression.
///
bool SPUAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                    unsigned AsmVariant, 
                                    const char *ExtraCode) {
  // Does this asm operand have a single letter operand modifier?
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0) return true; // Unknown modifier.
    
    switch (ExtraCode[0]) {
    default: return true;  // Unknown modifier.
    case 'L': // Write second word of DImode reference.  
      // Verify that this operand has two consecutive registers.
      if (!MI->getOperand(OpNo).isReg() ||
          OpNo+1 == MI->getNumOperands() ||
          !MI->getOperand(OpNo+1).isReg())
        return true;
      ++OpNo;   // Return the high-part.
      break;
    }
  }
  
  printOperand(MI, OpNo);
  return false;
}

bool SPUAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                          unsigned OpNo,
                                          unsigned AsmVariant, 
                                          const char *ExtraCode) {
  if (ExtraCode && ExtraCode[0])
    return true; // Unknown modifier.
  printMemRegReg(MI, OpNo);
  return false;
}

/// printMachineInstruction -- Print out a single PowerPC MI in Darwin syntax
/// to the current output stream.
///
void SPUAsmPrinter::printMachineInstruction(const MachineInstr *MI) {
  ++EmittedInsts;
  printInstruction(MI);
}

/// runOnMachineFunction - This uses the printMachineInstruction()
/// method to print assembly for each instruction.
///
bool
LinuxAsmPrinter::runOnMachineFunction(MachineFunction &MF)
{
  SetupMachineFunction(MF);
  O << "\n\n";
  
  // Print out constants referenced by the function
  EmitConstantPool(MF.getConstantPool());

  // Print out labels for the function.
  const Function *F = MF.getFunction();

  SwitchToSection(TAI->SectionForGlobal(F));
  EmitAlignment(3, F);

  switch (F->getLinkage()) {
  default: assert(0 && "Unknown linkage type!");
  case Function::InternalLinkage:  // Symbols default to internal.
    break;
  case Function::ExternalLinkage:
    O << "\t.global\t" << CurrentFnName << "\n"
      << "\t.type\t" << CurrentFnName << ", @function\n";
    break;
  case Function::WeakLinkage:
  case Function::LinkOnceLinkage:
    O << "\t.global\t" << CurrentFnName << "\n";
    O << "\t.weak_definition\t" << CurrentFnName << "\n";
    break;
  }
  O << CurrentFnName << ":\n";

  // Emit pre-function debug information.
  DW.BeginFunction(&MF);

  // Print out code for the function.
  for (MachineFunction::const_iterator I = MF.begin(), E = MF.end();
       I != E; ++I) {
    // Print a label for the basic block.
    if (I != MF.begin()) {
      printBasicBlockLabel(I, true, true);
      O << '\n';
    }
    for (MachineBasicBlock::const_iterator II = I->begin(), E = I->end();
         II != E; ++II) {
      // Print the assembly for the instruction.
      printMachineInstruction(II);
    }
  }

  O << "\t.size\t" << CurrentFnName << ",.-" << CurrentFnName << "\n";

  // Print out jump tables referenced by the function.
  EmitJumpTableInfo(MF.getJumpTableInfo(), MF);
  
  // Emit post-function debug information.
  DW.EndFunction(&MF);
  
  // We didn't modify anything.
  return false;
}


bool LinuxAsmPrinter::doInitialization(Module &M) {
  bool Result = AsmPrinter::doInitialization(M);
  SwitchToTextSection("\t.text");
  // Emit initial debug information.
  DW.BeginModule(&M);
  MMI = getAnalysisToUpdate<MachineModuleInfo>();
  DW.SetModuleInfo(MMI);
  return Result;
}

/// PrintUnmangledNameSafely - Print out the printable characters in the name.
/// Don't print things like \n or \0.
static void PrintUnmangledNameSafely(const Value *V, raw_ostream &OS) {
  for (const char *Name = V->getNameStart(), *E = Name+V->getNameLen();
       Name != E; ++Name)
    if (isprint(*Name))
      OS << *Name;
}

/*!
  Emit a global variable according to its section, alignment, etc.
  
  \note This code was shamelessly copied from the PowerPC's assembly printer,
  which sort of screams for some kind of refactorization of common code.
 */
void LinuxAsmPrinter::printModuleLevelGV(const GlobalVariable* GVar) {
  const TargetData *TD = TM.getTargetData();

  if (!GVar->hasInitializer())
    return;

  // Check to see if this is a special global used by LLVM, if so, emit it.
  if (EmitSpecialLLVMGlobal(GVar))
    return;

  std::string name = Mang->getValueName(GVar);

  printVisibility(name, GVar->getVisibility());

  Constant *C = GVar->getInitializer();
  const Type *Type = C->getType();
  unsigned Size = TD->getABITypeSize(Type);
  unsigned Align = TD->getPreferredAlignmentLog(GVar);

  SwitchToSection(TAI->SectionForGlobal(GVar));

  if (C->isNullValue() && /* FIXME: Verify correct */
      !GVar->hasSection() &&
      (GVar->hasInternalLinkage() || GVar->hasExternalLinkage() ||
       GVar->mayBeOverridden())) {
      if (Size == 0) Size = 1;   // .comm Foo, 0 is undefined, avoid it.

      if (GVar->hasExternalLinkage()) {
        O << "\t.global " << name << '\n';
        O << "\t.type " << name << ", @object\n";
        O << name << ":\n";
        O << "\t.zero " << Size << '\n';
      } else if (GVar->hasInternalLinkage()) {
        O << TAI->getLCOMMDirective() << name << ',' << Size;
      } else {
        O << ".comm " << name << ',' << Size;
      }
      O << "\t\t" << TAI->getCommentString() << " '";
      PrintUnmangledNameSafely(GVar, O);
      O << "'\n";
      return;
  }

  switch (GVar->getLinkage()) {
    // Should never be seen for the CellSPU platform...
   case GlobalValue::LinkOnceLinkage:
   case GlobalValue::WeakLinkage:
   case GlobalValue::CommonLinkage:
    O << "\t.global " << name << '\n'
      << "\t.type " << name << ", @object\n"
      << "\t.weak " << name << '\n';
    break;
   case GlobalValue::AppendingLinkage:
    // FIXME: appending linkage variables should go into a section of
    // their name or something.  For now, just emit them as external.
   case GlobalValue::ExternalLinkage:
    // If external or appending, declare as a global symbol
    O << "\t.global " << name << '\n'
      << "\t.type " << name << ", @object\n";
    // FALL THROUGH
   case GlobalValue::InternalLinkage:
    break;
   default:
    cerr << "Unknown linkage type!";
    abort();
  }

  EmitAlignment(Align, GVar);
  O << name << ":\t\t\t\t" << TAI->getCommentString() << " '";
  PrintUnmangledNameSafely(GVar, O);
  O << "'\n";

  // If the initializer is a extern weak symbol, remember to emit the weak
  // reference!
  if (const GlobalValue *GV = dyn_cast<GlobalValue>(C))
    if (GV->hasExternalWeakLinkage())
      ExtWeakSymbols.insert(GV);

  EmitGlobalConstant(C);
  O << '\n';
}

bool LinuxAsmPrinter::doFinalization(Module &M) {
  // Print out module-level global variables here.
  for (Module::const_global_iterator I = M.global_begin(), E = M.global_end();
       I != E; ++I)
    printModuleLevelGV(I);

  // TODO

  // Emit initial debug information.
  DW.EndModule();

  return AsmPrinter::doFinalization(M);
}

/// createSPUCodePrinterPass - Returns a pass that prints the Cell SPU
/// assembly code for a MachineFunction to the given output stream, in a format
/// that the Linux SPU assembler can deal with.
///
FunctionPass *llvm::createSPUAsmPrinterPass(raw_ostream &o,
                                            SPUTargetMachine &tm) {
  return new LinuxAsmPrinter(o, tm, tm.getTargetAsmInfo());
}

