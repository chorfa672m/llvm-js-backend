//===-- PPCAsmPrinter.cpp - Print machine instrs to PowerPC assembly --------=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to PowerPC assembly language. This printer is
// the output mechanism used by `llc'.
//
// Documentation at http://developer.apple.com/documentation/DeveloperTools/
// Reference/Assembler/ASMIntroduction/chapter_1_section_1.html
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "asmprinter"
#include "PPC.h"
#include "PPCPredicates.h"
#include "PPCTargetMachine.h"
#include "PPCMCInstLower.h"
#include "PPCSubtarget.h"
#include "llvm/Analysis/DebugInfo.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/Assembly/Writer.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Target/Mangler.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/SmallString.h"
#include "InstPrinter/PPCInstPrinter.h"
using namespace llvm;

// This option tells the asmprinter to use the new (experimental) MCInstPrinter
// path.
static cl::opt<bool> UseInstPrinter("enable-ppc-inst-printer",
                                    cl::ReallyHidden);

namespace {
  class PPCAsmPrinter : public AsmPrinter {
  protected:
    DenseMap<MCSymbol*, MCSymbol*> TOC;
    const PPCSubtarget &Subtarget;
    uint64_t LabelID;
  public:
    explicit PPCAsmPrinter(TargetMachine &TM, MCStreamer &Streamer)
      : AsmPrinter(TM, Streamer),
        Subtarget(TM.getSubtarget<PPCSubtarget>()), LabelID(0) {}

    virtual const char *getPassName() const {
      return "PowerPC Assembly Printer";
    }

    unsigned enumRegToMachineReg(unsigned enumReg) {
      switch (enumReg) {
      default: llvm_unreachable("Unhandled register!");
      case PPC::CR0:  return  0;
      case PPC::CR1:  return  1;
      case PPC::CR2:  return  2;
      case PPC::CR3:  return  3;
      case PPC::CR4:  return  4;
      case PPC::CR5:  return  5;
      case PPC::CR6:  return  6;
      case PPC::CR7:  return  7;
      }
      llvm_unreachable(0);
    }

    /// printInstruction - This method is automatically generated by tablegen
    /// from the instruction set description.  This method returns true if the
    /// machine instruction was sufficiently described to print it, otherwise it
    /// returns false.
    void printInstruction(const MachineInstr *MI, raw_ostream &O);
    static const char *getRegisterName(unsigned RegNo);


    virtual void EmitInstruction(const MachineInstr *MI);
    void printOp(const MachineOperand &MO, raw_ostream &O);

    /// stripRegisterPrefix - This method strips the character prefix from a
    /// register name so that only the number is left.  Used by for linux asm.
    const char *stripRegisterPrefix(const char *RegName) {
      switch (RegName[0]) {
      case 'r':
      case 'f':
      case 'v': return RegName + 1;
      case 'c': if (RegName[1] == 'r') return RegName + 2;
      }

      return RegName;
    }

    /// printRegister - Print register according to target requirements.
    ///
    void printRegister(const MachineOperand &MO, bool R0AsZero, raw_ostream &O){
      unsigned RegNo = MO.getReg();
      assert(TargetRegisterInfo::isPhysicalRegister(RegNo) && "Not physreg??");

      // If we should use 0 for R0.
      if (R0AsZero && RegNo == PPC::R0) {
        O << "0";
        return;
      }

      const char *RegName = getRegisterName(RegNo);
      // Linux assembler (Others?) does not take register mnemonics.
      // FIXME - What about special registers used in mfspr/mtspr?
      if (!Subtarget.isDarwin()) RegName = stripRegisterPrefix(RegName);
      O << RegName;
    }

    void printOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      const MachineOperand &MO = MI->getOperand(OpNo);
      if (MO.isReg()) {
        printRegister(MO, false, O);
      } else if (MO.isImm()) {
        O << MO.getImm();
      } else {
        printOp(MO, O);
      }
    }

    bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                         unsigned AsmVariant, const char *ExtraCode,
                         raw_ostream &O);
    bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                               unsigned AsmVariant, const char *ExtraCode,
                               raw_ostream &O);


    void printS5ImmOperand(const MachineInstr *MI, unsigned OpNo,
                           raw_ostream &O) {
      char value = MI->getOperand(OpNo).getImm();
      value = (value << (32-5)) >> (32-5);
      O << (int)value;
    }
    void printU5ImmOperand(const MachineInstr *MI, unsigned OpNo,
                           raw_ostream &O) {
      unsigned char value = MI->getOperand(OpNo).getImm();
      assert(value <= 31 && "Invalid u5imm argument!");
      O << (unsigned int)value;
    }
    void printU6ImmOperand(const MachineInstr *MI, unsigned OpNo,
                           raw_ostream &O) {
      unsigned char value = MI->getOperand(OpNo).getImm();
      assert(value <= 63 && "Invalid u6imm argument!");
      O << (unsigned int)value;
    }
    void printS16ImmOperand(const MachineInstr *MI, unsigned OpNo, 
                            raw_ostream &O) {
      O << (short)MI->getOperand(OpNo).getImm();
    }
    void printU16ImmOperand(const MachineInstr *MI, unsigned OpNo,
                            raw_ostream &O) {
      O << (unsigned short)MI->getOperand(OpNo).getImm();
    }
    void printS16X4ImmOperand(const MachineInstr *MI, unsigned OpNo,
                              raw_ostream &O) {
      if (MI->getOperand(OpNo).isImm()) {
        O << (short)(MI->getOperand(OpNo).getImm()*4);
      } else {
        O << "lo16(";
        printOp(MI->getOperand(OpNo), O);
        if (TM.getRelocationModel() == Reloc::PIC_)
          O << "-\"L" << getFunctionNumber() << "$pb\")";
        else
          O << ')';
      }
    }
    void printBranchOperand(const MachineInstr *MI, unsigned OpNo,
                            raw_ostream &O) {
      // Branches can take an immediate operand.  This is used by the branch
      // selection pass to print $+8, an eight byte displacement from the PC.
      if (MI->getOperand(OpNo).isImm()) {
        O << "$+" << MI->getOperand(OpNo).getImm()*4;
      } else {
        printOp(MI->getOperand(OpNo), O);
      }
    }
    void printCallOperand(const MachineInstr *MI, unsigned OpNo,
                          raw_ostream &O) {
      const MachineOperand &MO = MI->getOperand(OpNo);
      if (TM.getRelocationModel() != Reloc::Static) {
        if (MO.isGlobal()) {
          const GlobalValue *GV = MO.getGlobal();
          if (GV->isDeclaration() || GV->isWeakForLinker()) {
            // Dynamically-resolved functions need a stub for the function.
            MCSymbol *Sym = GetSymbolWithGlobalValueBase(GV, "$stub");
            MachineModuleInfoImpl::StubValueTy &StubSym =
              MMI->getObjFileInfo<MachineModuleInfoMachO>().getFnStubEntry(Sym);
            if (StubSym.getPointer() == 0)
              StubSym = MachineModuleInfoImpl::
                StubValueTy(Mang->getSymbol(GV), !GV->hasInternalLinkage());
            O << *Sym;
            return;
          }
        }
        if (MO.isSymbol()) {
          SmallString<128> TempNameStr;
          TempNameStr += StringRef(MO.getSymbolName());
          TempNameStr += StringRef("$stub");
          
          MCSymbol *Sym = GetExternalSymbolSymbol(TempNameStr.str());
          MachineModuleInfoImpl::StubValueTy &StubSym =
            MMI->getObjFileInfo<MachineModuleInfoMachO>().getFnStubEntry(Sym);
          if (StubSym.getPointer() == 0)
            StubSym = MachineModuleInfoImpl::
              StubValueTy(GetExternalSymbolSymbol(MO.getSymbolName()), true);
          O << *Sym;
          return;
        }
      }

      printOp(MI->getOperand(OpNo), O);
    }
    void printAbsAddrOperand(const MachineInstr *MI, unsigned OpNo,
                             raw_ostream &O) {
     O << (int)MI->getOperand(OpNo).getImm()*4;
    }
    void printPICLabel(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      O << "\"L" << getFunctionNumber() << "$pb\"\n";
      O << "\"L" << getFunctionNumber() << "$pb\":";
    }
    void printSymbolHi(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      if (MI->getOperand(OpNo).isImm()) {
        printS16ImmOperand(MI, OpNo, O);
      } else {
        if (Subtarget.isDarwin()) O << "ha16(";
        printOp(MI->getOperand(OpNo), O);
        if (TM.getRelocationModel() == Reloc::PIC_)
          O << "-\"L" << getFunctionNumber() << "$pb\"";
        if (Subtarget.isDarwin())
          O << ')';
        else
          O << "@ha";
      }
    }
    void printSymbolLo(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      if (MI->getOperand(OpNo).isImm()) {
        printS16ImmOperand(MI, OpNo, O);
      } else {
        if (Subtarget.isDarwin()) O << "lo16(";
        printOp(MI->getOperand(OpNo), O);
        if (TM.getRelocationModel() == Reloc::PIC_)
          O << "-\"L" << getFunctionNumber() << "$pb\"";
        if (Subtarget.isDarwin())
          O << ')';
        else
          O << "@l";
      }
    }
    void printcrbitm(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      unsigned CCReg = MI->getOperand(OpNo).getReg();
      unsigned RegNo = enumRegToMachineReg(CCReg);
      O << (0x80 >> RegNo);
    }
    // The new addressing mode printers.
    void printMemRegImm(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      printSymbolLo(MI, OpNo, O);
      O << '(';
      if (MI->getOperand(OpNo+1).isReg() &&
          MI->getOperand(OpNo+1).getReg() == PPC::R0)
        O << "0";
      else
        printOperand(MI, OpNo+1, O);
      O << ')';
    }
    void printMemRegImmShifted(const MachineInstr *MI, unsigned OpNo,
                               raw_ostream &O) {
      if (MI->getOperand(OpNo).isImm())
        printS16X4ImmOperand(MI, OpNo, O);
      else
        printSymbolLo(MI, OpNo, O);
      O << '(';
      if (MI->getOperand(OpNo+1).isReg() &&
          MI->getOperand(OpNo+1).getReg() == PPC::R0)
        O << "0";
      else
        printOperand(MI, OpNo+1, O);
      O << ')';
    }

    void printMemRegReg(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      // When used as the base register, r0 reads constant zero rather than
      // the value contained in the register.  For this reason, the darwin
      // assembler requires that we print r0 as 0 (no r) when used as the base.
      const MachineOperand &MO = MI->getOperand(OpNo);
      printRegister(MO, true, O);
      O << ", ";
      printOperand(MI, OpNo+1, O);
    }

    void printTOCEntryLabel(const MachineInstr *MI, unsigned OpNo,
                            raw_ostream &O) {
      const MachineOperand &MO = MI->getOperand(OpNo);
      assert(MO.isGlobal());
      MCSymbol *Sym = Mang->getSymbol(MO.getGlobal());

      // Map symbol -> label of TOC entry.
      MCSymbol *&TOCEntry = TOC[Sym];
      if (TOCEntry == 0)
        TOCEntry = OutContext.
          GetOrCreateSymbol(StringRef(MAI->getPrivateGlobalPrefix()) +
                            "C" + Twine(LabelID++));

      O << *TOCEntry << "@toc";
    }

    void printPredicateOperand(const MachineInstr *MI, unsigned OpNo,
                               raw_ostream &O, const char *Modifier);

    MachineLocation getDebugValueLocation(const MachineInstr *MI) const {

      MachineLocation Location;
      assert (MI->getNumOperands() == 4 && "Invalid no. of machine operands!");
      // Frame address.  Currently handles register +- offset only.
      if (MI->getOperand(0).isReg() && MI->getOperand(2).isImm())
        Location.set(MI->getOperand(0).getReg(), MI->getOperand(2).getImm());
      else {
        DEBUG(dbgs() << "DBG_VALUE instruction ignored! " << *MI << "\n");
      }
      return Location;
    }
  };

  /// PPCLinuxAsmPrinter - PowerPC assembly printer, customized for Linux
  class PPCLinuxAsmPrinter : public PPCAsmPrinter {
  public:
    explicit PPCLinuxAsmPrinter(TargetMachine &TM, MCStreamer &Streamer)
      : PPCAsmPrinter(TM, Streamer) {}

    virtual const char *getPassName() const {
      return "Linux PPC Assembly Printer";
    }

    bool doFinalization(Module &M);

    virtual void EmitFunctionEntryLabel();
  };

  /// PPCDarwinAsmPrinter - PowerPC assembly printer, customized for Darwin/Mac
  /// OS X
  class PPCDarwinAsmPrinter : public PPCAsmPrinter {
  public:
    explicit PPCDarwinAsmPrinter(TargetMachine &TM, MCStreamer &Streamer)
      : PPCAsmPrinter(TM, Streamer) {}

    virtual const char *getPassName() const {
      return "Darwin PPC Assembly Printer";
    }

    bool doFinalization(Module &M);
    void EmitStartOfAsmFile(Module &M);

    void EmitFunctionStubs(const MachineModuleInfoMachO::SymbolListTy &Stubs);
  };
} // end of anonymous namespace

// Include the auto-generated portion of the assembly writer
#include "PPCGenAsmWriter.inc"

void PPCAsmPrinter::printOp(const MachineOperand &MO, raw_ostream &O) {
  switch (MO.getType()) {
  case MachineOperand::MO_Immediate:
    llvm_unreachable("printOp() does not handle immediate values");

  case MachineOperand::MO_MachineBasicBlock:
    O << *MO.getMBB()->getSymbol();
    return;
  case MachineOperand::MO_JumpTableIndex:
    O << MAI->getPrivateGlobalPrefix() << "JTI" << getFunctionNumber()
      << '_' << MO.getIndex();
    // FIXME: PIC relocation model
    return;
  case MachineOperand::MO_ConstantPoolIndex:
    O << MAI->getPrivateGlobalPrefix() << "CPI" << getFunctionNumber()
      << '_' << MO.getIndex();
    return;
  case MachineOperand::MO_BlockAddress:
    O << *GetBlockAddressSymbol(MO.getBlockAddress());
    return;
  case MachineOperand::MO_ExternalSymbol: {
    // Computing the address of an external symbol, not calling it.
    if (TM.getRelocationModel() == Reloc::Static) {
      O << *GetExternalSymbolSymbol(MO.getSymbolName());
      return;
    }

    MCSymbol *NLPSym = 
      OutContext.GetOrCreateSymbol(StringRef(MAI->getGlobalPrefix())+
                                   MO.getSymbolName()+"$non_lazy_ptr");
    MachineModuleInfoImpl::StubValueTy &StubSym = 
      MMI->getObjFileInfo<MachineModuleInfoMachO>().getGVStubEntry(NLPSym);
    if (StubSym.getPointer() == 0)
      StubSym = MachineModuleInfoImpl::
        StubValueTy(GetExternalSymbolSymbol(MO.getSymbolName()), true);
    
    O << *NLPSym;
    return;
  }
  case MachineOperand::MO_GlobalAddress: {
    // Computing the address of a global symbol, not calling it.
    const GlobalValue *GV = MO.getGlobal();
    MCSymbol *SymToPrint;

    // External or weakly linked global variables need non-lazily-resolved stubs
    if (TM.getRelocationModel() != Reloc::Static &&
        (GV->isDeclaration() || GV->isWeakForLinker())) {
      if (!GV->hasHiddenVisibility()) {
        SymToPrint = GetSymbolWithGlobalValueBase(GV, "$non_lazy_ptr");
        MachineModuleInfoImpl::StubValueTy &StubSym = 
          MMI->getObjFileInfo<MachineModuleInfoMachO>()
            .getGVStubEntry(SymToPrint);
        if (StubSym.getPointer() == 0)
          StubSym = MachineModuleInfoImpl::
            StubValueTy(Mang->getSymbol(GV), !GV->hasInternalLinkage());
      } else if (GV->isDeclaration() || GV->hasCommonLinkage() ||
                 GV->hasAvailableExternallyLinkage()) {
        SymToPrint = GetSymbolWithGlobalValueBase(GV, "$non_lazy_ptr");
        
        MachineModuleInfoImpl::StubValueTy &StubSym = 
          MMI->getObjFileInfo<MachineModuleInfoMachO>().
                    getHiddenGVStubEntry(SymToPrint);
        if (StubSym.getPointer() == 0)
          StubSym = MachineModuleInfoImpl::
            StubValueTy(Mang->getSymbol(GV), !GV->hasInternalLinkage());
      } else {
        SymToPrint = Mang->getSymbol(GV);
      }
    } else {
      SymToPrint = Mang->getSymbol(GV);
    }
    
    O << *SymToPrint;

    printOffset(MO.getOffset(), O);
    return;
  }

  default:
    O << "<unknown operand type: " << MO.getType() << ">";
    return;
  }
}

/// PrintAsmOperand - Print out an operand for an inline asm expression.
///
bool PPCAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                    unsigned AsmVariant,
                                    const char *ExtraCode, raw_ostream &O) {
  // Does this asm operand have a single letter operand modifier?
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0) return true; // Unknown modifier.

    switch (ExtraCode[0]) {
    default: return true;  // Unknown modifier.
    case 'c': // Don't print "$" before a global var name or constant.
      // PPC never has a prefix.
      printOperand(MI, OpNo, O);
      return false;
    case 'L': // Write second word of DImode reference.
      // Verify that this operand has two consecutive registers.
      if (!MI->getOperand(OpNo).isReg() ||
          OpNo+1 == MI->getNumOperands() ||
          !MI->getOperand(OpNo+1).isReg())
        return true;
      ++OpNo;   // Return the high-part.
      break;
    case 'I':
      // Write 'i' if an integer constant, otherwise nothing.  Used to print
      // addi vs add, etc.
      if (MI->getOperand(OpNo).isImm())
        O << "i";
      return false;
    }
  }

  printOperand(MI, OpNo, O);
  return false;
}

// At the moment, all inline asm memory operands are a single register.
// In any case, the output of this routine should always be just one
// assembler operand.

bool PPCAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                                          unsigned AsmVariant,
                                          const char *ExtraCode,
                                          raw_ostream &O) {
  if (ExtraCode && ExtraCode[0])
    return true; // Unknown modifier.
  assert (MI->getOperand(OpNo).isReg());
  O << "0(";
  printOperand(MI, OpNo, O);
  O << ")";
  return false;
}

void PPCAsmPrinter::printPredicateOperand(const MachineInstr *MI, unsigned OpNo,
                                          raw_ostream &O, const char *Modifier){
  assert(Modifier && "Must specify 'cc' or 'reg' as predicate op modifier!");
  unsigned Code = MI->getOperand(OpNo).getImm();
  if (!strcmp(Modifier, "cc")) {
    switch ((PPC::Predicate)Code) {
    case PPC::PRED_ALWAYS: return; // Don't print anything for always.
    case PPC::PRED_LT: O << "lt"; return;
    case PPC::PRED_LE: O << "le"; return;
    case PPC::PRED_EQ: O << "eq"; return;
    case PPC::PRED_GE: O << "ge"; return;
    case PPC::PRED_GT: O << "gt"; return;
    case PPC::PRED_NE: O << "ne"; return;
    case PPC::PRED_UN: O << "un"; return;
    case PPC::PRED_NU: O << "nu"; return;
    }

  } else {
    assert(!strcmp(Modifier, "reg") &&
           "Need to specify 'cc' or 'reg' as predicate op modifier!");
    // Don't print the register for 'always'.
    if (Code == PPC::PRED_ALWAYS) return;
    printOperand(MI, OpNo+1, O);
  }
}


/// EmitInstruction -- Print out a single PowerPC MI in Darwin syntax to
/// the current output stream.
///
void PPCAsmPrinter::EmitInstruction(const MachineInstr *MI) {
  if (UseInstPrinter) {
    PPCMCInstLower MCInstLowering(OutContext, *Mang, *this);
    
    // Lower multi-instruction pseudo operations.
    switch (MI->getOpcode()) {
    default: break;
    // TODO: implement me.
    }

    MCInst TmpInst;
    MCInstLowering.Lower(MI, TmpInst);
    OutStreamer.EmitInstruction(TmpInst);
    return;
  }
  
  
  SmallString<128> Str;
  raw_svector_ostream O(Str);

  if (MI->getOpcode() == TargetOpcode::DBG_VALUE) {
    unsigned NOps = MI->getNumOperands();
    assert(NOps==4);
    O << '\t' << MAI->getCommentString() << "DEBUG_VALUE: ";
    // cast away const; DIetc do not take const operands for some reason.
    DIVariable V(const_cast<MDNode *>(MI->getOperand(NOps-1).getMetadata()));
    O << V.getName();
    O << " <- ";
    // Frame address.  Currently handles register +- offset only.
    assert(MI->getOperand(0).isReg() && MI->getOperand(1).isImm());
    O << '['; printOperand(MI, 0, O); O << '+'; printOperand(MI, 1, O);
    O << ']';
    O << "+";
    printOperand(MI, NOps-2, O);
    OutStreamer.EmitRawText(O.str());
    return;
  }
  // Check for slwi/srwi mnemonics.
  if (MI->getOpcode() == PPC::RLWINM) {
    unsigned char SH = MI->getOperand(2).getImm();
    unsigned char MB = MI->getOperand(3).getImm();
    unsigned char ME = MI->getOperand(4).getImm();
    bool useSubstituteMnemonic = false;
    if (SH <= 31 && MB == 0 && ME == (31-SH)) {
      O << "\tslwi "; useSubstituteMnemonic = true;
    }
    if (SH <= 31 && MB == (32-SH) && ME == 31) {
      O << "\tsrwi "; useSubstituteMnemonic = true;
      SH = 32-SH;
    }
    if (useSubstituteMnemonic) {
      printOperand(MI, 0, O);
      O << ", ";
      printOperand(MI, 1, O);
      O << ", " << (unsigned int)SH;
      OutStreamer.EmitRawText(O.str());
      return;
    }
  }
  
  if ((MI->getOpcode() == PPC::OR || MI->getOpcode() == PPC::OR8) &&
      MI->getOperand(1).getReg() == MI->getOperand(2).getReg()) {
    O << "\tmr ";
    printOperand(MI, 0, O);
    O << ", ";
    printOperand(MI, 1, O);
    OutStreamer.EmitRawText(O.str());
    return;
  }
  
  if (MI->getOpcode() == PPC::RLDICR) {
    unsigned char SH = MI->getOperand(2).getImm();
    unsigned char ME = MI->getOperand(3).getImm();
    // rldicr RA, RS, SH, 63-SH == sldi RA, RS, SH
    if (63-SH == ME) {
      O << "\tsldi ";
      printOperand(MI, 0, O);
      O << ", ";
      printOperand(MI, 1, O);
      O << ", " << (unsigned int)SH;
      OutStreamer.EmitRawText(O.str());
      return;
    }
  }

  printInstruction(MI, O);
  OutStreamer.EmitRawText(O.str());
}

void PPCLinuxAsmPrinter::EmitFunctionEntryLabel() {
  if (!Subtarget.isPPC64())  // linux/ppc32 - Normal entry label.
    return AsmPrinter::EmitFunctionEntryLabel();
    
  // Emit an official procedure descriptor.
  // FIXME 64-bit SVR4: Use MCSection here!
  OutStreamer.EmitRawText(StringRef("\t.section\t\".opd\",\"aw\""));
  OutStreamer.EmitRawText(StringRef("\t.align 3"));
  OutStreamer.EmitLabel(CurrentFnSym);
  OutStreamer.EmitRawText("\t.quad .L." + Twine(CurrentFnSym->getName()) +
                          ",.TOC.@tocbase");
  OutStreamer.EmitRawText(StringRef("\t.previous"));
  OutStreamer.EmitRawText(".L." + Twine(CurrentFnSym->getName()) + ":");
}


bool PPCLinuxAsmPrinter::doFinalization(Module &M) {
  const TargetData *TD = TM.getTargetData();

  bool isPPC64 = TD->getPointerSizeInBits() == 64;

  if (isPPC64 && !TOC.empty()) {
    // FIXME 64-bit SVR4: Use MCSection here?
    OutStreamer.EmitRawText(StringRef("\t.section\t\".toc\",\"aw\""));

    // FIXME: This is nondeterminstic!
    for (DenseMap<MCSymbol*, MCSymbol*>::iterator I = TOC.begin(),
         E = TOC.end(); I != E; ++I) {
      OutStreamer.EmitLabel(I->second);
      OutStreamer.EmitRawText("\t.tc " + Twine(I->first->getName()) +
                              "[TC]," + I->first->getName());
    }
  }

  return AsmPrinter::doFinalization(M);
}

void PPCDarwinAsmPrinter::EmitStartOfAsmFile(Module &M) {
  static const char *const CPUDirectives[] = {
    "",
    "ppc",
    "ppc601",
    "ppc602",
    "ppc603",
    "ppc7400",
    "ppc750",
    "ppc970",
    "ppc64"
  };

  unsigned Directive = Subtarget.getDarwinDirective();
  if (Subtarget.isGigaProcessor() && Directive < PPC::DIR_970)
    Directive = PPC::DIR_970;
  if (Subtarget.hasAltivec() && Directive < PPC::DIR_7400)
    Directive = PPC::DIR_7400;
  if (Subtarget.isPPC64() && Directive < PPC::DIR_970)
    Directive = PPC::DIR_64;
  assert(Directive <= PPC::DIR_64 && "Directive out of range.");
  OutStreamer.EmitRawText("\t.machine " + Twine(CPUDirectives[Directive]));

  // Prime text sections so they are adjacent.  This reduces the likelihood a
  // large data or debug section causes a branch to exceed 16M limit.
  const TargetLoweringObjectFileMachO &TLOFMacho = 
    static_cast<const TargetLoweringObjectFileMachO &>(getObjFileLowering());
  OutStreamer.SwitchSection(TLOFMacho.getTextCoalSection());
  if (TM.getRelocationModel() == Reloc::PIC_) {
    OutStreamer.SwitchSection(
           OutContext.getMachOSection("__TEXT", "__picsymbolstub1",
                                      MCSectionMachO::S_SYMBOL_STUBS |
                                      MCSectionMachO::S_ATTR_PURE_INSTRUCTIONS,
                                      32, SectionKind::getText()));
  } else if (TM.getRelocationModel() == Reloc::DynamicNoPIC) {
    OutStreamer.SwitchSection(
           OutContext.getMachOSection("__TEXT","__symbol_stub1",
                                      MCSectionMachO::S_SYMBOL_STUBS |
                                      MCSectionMachO::S_ATTR_PURE_INSTRUCTIONS,
                                      16, SectionKind::getText()));
  }
  OutStreamer.SwitchSection(getObjFileLowering().getTextSection());
}

static MCSymbol *GetLazyPtr(MCSymbol *Sym, MCContext &Ctx) {
  // Remove $stub suffix, add $lazy_ptr.
  SmallString<128> TmpStr(Sym->getName().begin(), Sym->getName().end()-5);
  TmpStr += "$lazy_ptr";
  return Ctx.GetOrCreateSymbol(TmpStr.str());
}

static MCSymbol *GetAnonSym(MCSymbol *Sym, MCContext &Ctx) {
  // Add $tmp suffix to $stub, yielding $stub$tmp.
  SmallString<128> TmpStr(Sym->getName().begin(), Sym->getName().end());
  TmpStr += "$tmp";
  return Ctx.GetOrCreateSymbol(TmpStr.str());
}

void PPCDarwinAsmPrinter::
EmitFunctionStubs(const MachineModuleInfoMachO::SymbolListTy &Stubs) {
  bool isPPC64 = TM.getTargetData()->getPointerSizeInBits() == 64;
  
  const TargetLoweringObjectFileMachO &TLOFMacho = 
    static_cast<const TargetLoweringObjectFileMachO &>(getObjFileLowering());

  // .lazy_symbol_pointer
  const MCSection *LSPSection = TLOFMacho.getLazySymbolPointerSection();
  
  // Output stubs for dynamically-linked functions
  if (TM.getRelocationModel() == Reloc::PIC_) {
    const MCSection *StubSection = 
    OutContext.getMachOSection("__TEXT", "__picsymbolstub1",
                               MCSectionMachO::S_SYMBOL_STUBS |
                               MCSectionMachO::S_ATTR_PURE_INSTRUCTIONS,
                               32, SectionKind::getText());
    for (unsigned i = 0, e = Stubs.size(); i != e; ++i) {
      OutStreamer.SwitchSection(StubSection);
      EmitAlignment(4);
      
      MCSymbol *Stub = Stubs[i].first;
      MCSymbol *RawSym = Stubs[i].second.getPointer();
      MCSymbol *LazyPtr = GetLazyPtr(Stub, OutContext);
      MCSymbol *AnonSymbol = GetAnonSym(Stub, OutContext);
                                           
      OutStreamer.EmitLabel(Stub);
      OutStreamer.EmitSymbolAttribute(RawSym, MCSA_IndirectSymbol);
      // FIXME: MCize this.
      OutStreamer.EmitRawText(StringRef("\tmflr r0"));
      OutStreamer.EmitRawText("\tbcl 20,31," + Twine(AnonSymbol->getName()));
      OutStreamer.EmitLabel(AnonSymbol);
      OutStreamer.EmitRawText(StringRef("\tmflr r11"));
      OutStreamer.EmitRawText("\taddis r11,r11,ha16("+Twine(LazyPtr->getName())+
                              "-" + AnonSymbol->getName() + ")");
      OutStreamer.EmitRawText(StringRef("\tmtlr r0"));
      
      if (isPPC64)
        OutStreamer.EmitRawText("\tldu r12,lo16(" + Twine(LazyPtr->getName()) +
                                "-" + AnonSymbol->getName() + ")(r11)");
      else
        OutStreamer.EmitRawText("\tlwzu r12,lo16(" + Twine(LazyPtr->getName()) +
                                "-" + AnonSymbol->getName() + ")(r11)");
      OutStreamer.EmitRawText(StringRef("\tmtctr r12"));
      OutStreamer.EmitRawText(StringRef("\tbctr"));
      
      OutStreamer.SwitchSection(LSPSection);
      OutStreamer.EmitLabel(LazyPtr);
      OutStreamer.EmitSymbolAttribute(RawSym, MCSA_IndirectSymbol);
      
      if (isPPC64)
        OutStreamer.EmitRawText(StringRef("\t.quad dyld_stub_binding_helper"));
      else
        OutStreamer.EmitRawText(StringRef("\t.long dyld_stub_binding_helper"));
    }
    OutStreamer.AddBlankLine();
    return;
  }
  
  const MCSection *StubSection =
    OutContext.getMachOSection("__TEXT","__symbol_stub1",
                               MCSectionMachO::S_SYMBOL_STUBS |
                               MCSectionMachO::S_ATTR_PURE_INSTRUCTIONS,
                               16, SectionKind::getText());
  for (unsigned i = 0, e = Stubs.size(); i != e; ++i) {
    MCSymbol *Stub = Stubs[i].first;
    MCSymbol *RawSym = Stubs[i].second.getPointer();
    MCSymbol *LazyPtr = GetLazyPtr(Stub, OutContext);

    OutStreamer.SwitchSection(StubSection);
    EmitAlignment(4);
    OutStreamer.EmitLabel(Stub);
    OutStreamer.EmitSymbolAttribute(RawSym, MCSA_IndirectSymbol);
    OutStreamer.EmitRawText("\tlis r11,ha16(" + Twine(LazyPtr->getName()) +")");
    if (isPPC64)
      OutStreamer.EmitRawText("\tldu r12,lo16(" + Twine(LazyPtr->getName()) +
                              ")(r11)");
    else
      OutStreamer.EmitRawText("\tlwzu r12,lo16(" + Twine(LazyPtr->getName()) +
                              ")(r11)");
    OutStreamer.EmitRawText(StringRef("\tmtctr r12"));
    OutStreamer.EmitRawText(StringRef("\tbctr"));
    OutStreamer.SwitchSection(LSPSection);
    OutStreamer.EmitLabel(LazyPtr);
    OutStreamer.EmitSymbolAttribute(RawSym, MCSA_IndirectSymbol);
    
    if (isPPC64)
      OutStreamer.EmitRawText(StringRef("\t.quad dyld_stub_binding_helper"));
    else
      OutStreamer.EmitRawText(StringRef("\t.long dyld_stub_binding_helper"));
  }
  
  OutStreamer.AddBlankLine();
}


bool PPCDarwinAsmPrinter::doFinalization(Module &M) {
  bool isPPC64 = TM.getTargetData()->getPointerSizeInBits() == 64;

  // Darwin/PPC always uses mach-o.
  const TargetLoweringObjectFileMachO &TLOFMacho = 
    static_cast<const TargetLoweringObjectFileMachO &>(getObjFileLowering());
  MachineModuleInfoMachO &MMIMacho =
    MMI->getObjFileInfo<MachineModuleInfoMachO>();
  
  MachineModuleInfoMachO::SymbolListTy Stubs = MMIMacho.GetFnStubList();
  if (!Stubs.empty())
    EmitFunctionStubs(Stubs);

  if (MAI->doesSupportExceptionHandling() && MMI) {
    // Add the (possibly multiple) personalities to the set of global values.
    // Only referenced functions get into the Personalities list.
    const std::vector<const Function*> &Personalities = MMI->getPersonalities();
    for (std::vector<const Function*>::const_iterator I = Personalities.begin(),
         E = Personalities.end(); I != E; ++I) {
      if (*I) {
        MCSymbol *NLPSym = GetSymbolWithGlobalValueBase(*I, "$non_lazy_ptr");
        MachineModuleInfoImpl::StubValueTy &StubSym =
          MMIMacho.getGVStubEntry(NLPSym);
        StubSym = MachineModuleInfoImpl::StubValueTy(Mang->getSymbol(*I), true);
      }
    }
  }

  // Output stubs for dynamically-linked functions.
  Stubs = MMIMacho.GetGVStubList();
  
  // Output macho stubs for external and common global variables.
  if (!Stubs.empty()) {
    // Switch with ".non_lazy_symbol_pointer" directive.
    OutStreamer.SwitchSection(TLOFMacho.getNonLazySymbolPointerSection());
    EmitAlignment(isPPC64 ? 3 : 2);
    
    for (unsigned i = 0, e = Stubs.size(); i != e; ++i) {
      // L_foo$stub:
      OutStreamer.EmitLabel(Stubs[i].first);
      //   .indirect_symbol _foo
      MachineModuleInfoImpl::StubValueTy &MCSym = Stubs[i].second;
      OutStreamer.EmitSymbolAttribute(MCSym.getPointer(), MCSA_IndirectSymbol);

      if (MCSym.getInt())
        // External to current translation unit.
        OutStreamer.EmitIntValue(0, isPPC64 ? 8 : 4/*size*/, 0/*addrspace*/);
      else
        // Internal to current translation unit.
        //
        // When we place the LSDA into the TEXT section, the type info pointers
        // need to be indirect and pc-rel. We accomplish this by using NLPs.
        // However, sometimes the types are local to the file. So we need to
        // fill in the value for the NLP in those cases.
        OutStreamer.EmitValue(MCSymbolRefExpr::Create(MCSym.getPointer(),
                                                      OutContext),
                              isPPC64 ? 8 : 4/*size*/, 0/*addrspace*/);
    }

    Stubs.clear();
    OutStreamer.AddBlankLine();
  }

  Stubs = MMIMacho.GetHiddenGVStubList();
  if (!Stubs.empty()) {
    OutStreamer.SwitchSection(getObjFileLowering().getDataSection());
    EmitAlignment(isPPC64 ? 3 : 2);
    
    for (unsigned i = 0, e = Stubs.size(); i != e; ++i) {
      // L_foo$stub:
      OutStreamer.EmitLabel(Stubs[i].first);
      //   .long _foo
      OutStreamer.EmitValue(MCSymbolRefExpr::
                            Create(Stubs[i].second.getPointer(),
                                   OutContext),
                            isPPC64 ? 8 : 4/*size*/, 0/*addrspace*/);
    }

    Stubs.clear();
    OutStreamer.AddBlankLine();
  }

  // Funny Darwin hack: This flag tells the linker that no global symbols
  // contain code that falls through to other global symbols (e.g. the obvious
  // implementation of multiple entry points).  If this doesn't occur, the
  // linker can safely perform dead code stripping.  Since LLVM never generates
  // code that does this, it is always safe to set.
  OutStreamer.EmitAssemblerFlag(MCAF_SubsectionsViaSymbols);

  return AsmPrinter::doFinalization(M);
}

/// createPPCAsmPrinterPass - Returns a pass that prints the PPC assembly code
/// for a MachineFunction to the given output stream, in a format that the
/// Darwin assembler can deal with.
///
static AsmPrinter *createPPCAsmPrinterPass(TargetMachine &tm,
                                           MCStreamer &Streamer) {
  const PPCSubtarget *Subtarget = &tm.getSubtarget<PPCSubtarget>();

  if (Subtarget->isDarwin())
    return new PPCDarwinAsmPrinter(tm, Streamer);
  return new PPCLinuxAsmPrinter(tm, Streamer);
}

static MCInstPrinter *createPPCMCInstPrinter(const Target &T,
                                             unsigned SyntaxVariant,
                                             const MCAsmInfo &MAI) {
  return new PPCInstPrinter(MAI, SyntaxVariant);
}


// Force static initialization.
extern "C" void LLVMInitializePowerPCAsmPrinter() { 
  TargetRegistry::RegisterAsmPrinter(ThePPC32Target, createPPCAsmPrinterPass);
  TargetRegistry::RegisterAsmPrinter(ThePPC64Target, createPPCAsmPrinterPass);
  
  TargetRegistry::RegisterMCInstPrinter(ThePPC32Target, createPPCMCInstPrinter);
  TargetRegistry::RegisterMCInstPrinter(ThePPC32Target, createPPCMCInstPrinter);
}
