//===-- ARMFastISel.cpp - ARM FastISel implementation ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the ARM-specific support for the FastISel class. Some
// of the target-specific code is generated by tablegen in the file
// ARMGenFastISel.inc, which is #included here.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMBaseInstrInfo.h"
#include "ARMRegisterInfo.h"
#include "ARMTargetMachine.h"
#include "ARMSubtarget.h"
#include "llvm/CallingConv.h"
#include "llvm/DerivedTypes.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/FastISel.h"
#include "llvm/CodeGen/FunctionLoweringInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
using namespace llvm;

static cl::opt<bool>
EnableARMFastISel("arm-fast-isel",
                  cl::desc("Turn on experimental ARM fast-isel support"),
                  cl::init(false), cl::Hidden);

namespace {

class ARMFastISel : public FastISel {

  /// Subtarget - Keep a pointer to the ARMSubtarget around so that we can
  /// make the right decision when generating code for different targets.
  const ARMSubtarget *Subtarget;
  const TargetMachine &TM;
  const TargetInstrInfo &TII;
  const TargetLowering &TLI;
  const ARMFunctionInfo *AFI;

  public:
    explicit ARMFastISel(FunctionLoweringInfo &funcInfo) 
    : FastISel(funcInfo),
      TM(funcInfo.MF->getTarget()),
      TII(*TM.getInstrInfo()),
      TLI(*TM.getTargetLowering()) {
      Subtarget = &TM.getSubtarget<ARMSubtarget>();
      AFI = funcInfo.MF->getInfo<ARMFunctionInfo>();
    }

    // Code from FastISel.cpp.
    virtual unsigned FastEmitInst_(unsigned MachineInstOpcode,
                                   const TargetRegisterClass *RC);
    virtual unsigned FastEmitInst_r(unsigned MachineInstOpcode,
                                    const TargetRegisterClass *RC,
                                    unsigned Op0, bool Op0IsKill);
    virtual unsigned FastEmitInst_rr(unsigned MachineInstOpcode,
                                     const TargetRegisterClass *RC,
                                     unsigned Op0, bool Op0IsKill,
                                     unsigned Op1, bool Op1IsKill);
    virtual unsigned FastEmitInst_ri(unsigned MachineInstOpcode,
                                     const TargetRegisterClass *RC,
                                     unsigned Op0, bool Op0IsKill,
                                     uint64_t Imm);
    virtual unsigned FastEmitInst_rf(unsigned MachineInstOpcode,
                                     const TargetRegisterClass *RC,
                                     unsigned Op0, bool Op0IsKill,
                                     const ConstantFP *FPImm);
    virtual unsigned FastEmitInst_i(unsigned MachineInstOpcode,
                                    const TargetRegisterClass *RC,
                                    uint64_t Imm);
    virtual unsigned FastEmitInst_rri(unsigned MachineInstOpcode,
                                      const TargetRegisterClass *RC,
                                      unsigned Op0, bool Op0IsKill,
                                      unsigned Op1, bool Op1IsKill,
                                      uint64_t Imm);
    virtual unsigned FastEmitInst_extractsubreg(MVT RetVT,
                                                unsigned Op0, bool Op0IsKill,
                                                uint32_t Idx);
                                                
    // Backend specific FastISel code.
    virtual bool TargetSelectInstruction(const Instruction *I);

  #include "ARMGenFastISel.inc"
  
    // Instruction selection routines.
    virtual bool ARMSelectLoad(const Instruction *I);

    // Utility routines.
  private:
    bool ARMComputeRegOffset(const Instruction *I, unsigned &Reg, int &Offset);
    
    bool DefinesOptionalPredicate(MachineInstr *MI, bool *CPSR);
    const MachineInstrBuilder &AddOptionalDefs(const MachineInstrBuilder &MIB);
};

} // end anonymous namespace

// #include "ARMGenCallingConv.inc"

// DefinesOptionalPredicate - This is different from DefinesPredicate in that
// we don't care about implicit defs here, just places we'll need to add a
// default CCReg argument. Sets CPSR if we're setting CPSR instead of CCR.
bool ARMFastISel::DefinesOptionalPredicate(MachineInstr *MI, bool *CPSR) {
  const TargetInstrDesc &TID = MI->getDesc();
  if (!TID.hasOptionalDef())
    return false;

  // Look to see if our OptionalDef is defining CPSR or CCR.
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
    const MachineOperand &MO = MI->getOperand(i);
    if (!MO.isReg() || !MO.isDef()) continue;
    if (MO.getReg() == ARM::CPSR)
      *CPSR = true;
  }
  return true;
}

// If the machine is predicable go ahead and add the predicate operands, if
// it needs default CC operands add those.
const MachineInstrBuilder &
ARMFastISel::AddOptionalDefs(const MachineInstrBuilder &MIB) {
  MachineInstr *MI = &*MIB;

  // Do we use a predicate?
  if (TII.isPredicable(MI))
    AddDefaultPred(MIB);
  
  // Do we optionally set a predicate?  Preds is size > 0 iff the predicate
  // defines CPSR. All other OptionalDefines in ARM are the CCR register.
  bool CPSR = false;
  if (DefinesOptionalPredicate(MI, &CPSR)) {
    if (CPSR)
      AddDefaultT1CC(MIB);
    else
      AddDefaultCC(MIB);
  }
  return MIB;
}

unsigned ARMFastISel::FastEmitInst_(unsigned MachineInstOpcode,
                                    const TargetRegisterClass* RC) {
  unsigned ResultReg = createResultReg(RC);
  const TargetInstrDesc &II = TII.get(MachineInstOpcode);

  AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II, ResultReg));
  return ResultReg;
}

unsigned ARMFastISel::FastEmitInst_r(unsigned MachineInstOpcode,
                                     const TargetRegisterClass *RC,
                                     unsigned Op0, bool Op0IsKill) {
  unsigned ResultReg = createResultReg(RC);
  const TargetInstrDesc &II = TII.get(MachineInstOpcode);

  if (II.getNumDefs() >= 1)
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II, ResultReg)
                   .addReg(Op0, Op0IsKill * RegState::Kill));
  else {
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II)
                   .addReg(Op0, Op0IsKill * RegState::Kill));
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL,
                   TII.get(TargetOpcode::COPY), ResultReg)
                   .addReg(II.ImplicitDefs[0]));
  }
  return ResultReg;
}

unsigned ARMFastISel::FastEmitInst_rr(unsigned MachineInstOpcode,
                                      const TargetRegisterClass *RC,
                                      unsigned Op0, bool Op0IsKill,
                                      unsigned Op1, bool Op1IsKill) {
  unsigned ResultReg = createResultReg(RC);
  const TargetInstrDesc &II = TII.get(MachineInstOpcode);

  if (II.getNumDefs() >= 1)
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II, ResultReg)
                   .addReg(Op0, Op0IsKill * RegState::Kill)
                   .addReg(Op1, Op1IsKill * RegState::Kill));
  else {
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II)
                   .addReg(Op0, Op0IsKill * RegState::Kill)
                   .addReg(Op1, Op1IsKill * RegState::Kill));
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL,
                           TII.get(TargetOpcode::COPY), ResultReg)
                   .addReg(II.ImplicitDefs[0]));
  }
  return ResultReg;
}

unsigned ARMFastISel::FastEmitInst_ri(unsigned MachineInstOpcode,
                                      const TargetRegisterClass *RC,
                                      unsigned Op0, bool Op0IsKill,
                                      uint64_t Imm) {
  unsigned ResultReg = createResultReg(RC);
  const TargetInstrDesc &II = TII.get(MachineInstOpcode);

  if (II.getNumDefs() >= 1)
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II, ResultReg)
                   .addReg(Op0, Op0IsKill * RegState::Kill)
                   .addImm(Imm));
  else {
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II)
                   .addReg(Op0, Op0IsKill * RegState::Kill)
                   .addImm(Imm));
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL,
                           TII.get(TargetOpcode::COPY), ResultReg)
                   .addReg(II.ImplicitDefs[0]));
  }
  return ResultReg;
}

unsigned ARMFastISel::FastEmitInst_rf(unsigned MachineInstOpcode,
                                      const TargetRegisterClass *RC,
                                      unsigned Op0, bool Op0IsKill,
                                      const ConstantFP *FPImm) {
  unsigned ResultReg = createResultReg(RC);
  const TargetInstrDesc &II = TII.get(MachineInstOpcode);

  if (II.getNumDefs() >= 1)
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II, ResultReg)
                   .addReg(Op0, Op0IsKill * RegState::Kill)
                   .addFPImm(FPImm));
  else {
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II)
                   .addReg(Op0, Op0IsKill * RegState::Kill)
                   .addFPImm(FPImm));
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL,
                           TII.get(TargetOpcode::COPY), ResultReg)
                   .addReg(II.ImplicitDefs[0]));
  }
  return ResultReg;
}

unsigned ARMFastISel::FastEmitInst_rri(unsigned MachineInstOpcode,
                                       const TargetRegisterClass *RC,
                                       unsigned Op0, bool Op0IsKill,
                                       unsigned Op1, bool Op1IsKill,
                                       uint64_t Imm) {
  unsigned ResultReg = createResultReg(RC);
  const TargetInstrDesc &II = TII.get(MachineInstOpcode);

  if (II.getNumDefs() >= 1)
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II, ResultReg)
                   .addReg(Op0, Op0IsKill * RegState::Kill)
                   .addReg(Op1, Op1IsKill * RegState::Kill)
                   .addImm(Imm));
  else {
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II)
                   .addReg(Op0, Op0IsKill * RegState::Kill)
                   .addReg(Op1, Op1IsKill * RegState::Kill)
                   .addImm(Imm));
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL,
                           TII.get(TargetOpcode::COPY), ResultReg)
                   .addReg(II.ImplicitDefs[0]));
  }
  return ResultReg;
}

unsigned ARMFastISel::FastEmitInst_i(unsigned MachineInstOpcode,
                                     const TargetRegisterClass *RC,
                                     uint64_t Imm) {
  unsigned ResultReg = createResultReg(RC);
  const TargetInstrDesc &II = TII.get(MachineInstOpcode);
  
  if (II.getNumDefs() >= 1)
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II, ResultReg)
                   .addImm(Imm));
  else {
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL, II)
                   .addImm(Imm));
    AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL,
                           TII.get(TargetOpcode::COPY), ResultReg)
                   .addReg(II.ImplicitDefs[0]));
  }
  return ResultReg;
}

unsigned ARMFastISel::FastEmitInst_extractsubreg(MVT RetVT,
                                                 unsigned Op0, bool Op0IsKill,
                                                 uint32_t Idx) {
  unsigned ResultReg = createResultReg(TLI.getRegClassFor(RetVT));
  assert(TargetRegisterInfo::isVirtualRegister(Op0) &&
         "Cannot yet extract from physregs");
  AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt,
                         DL, TII.get(TargetOpcode::COPY), ResultReg)
                 .addReg(Op0, getKillRegState(Op0IsKill), Idx));
  return ResultReg;
}

bool ARMFastISel::ARMComputeRegOffset(const Instruction *I, unsigned &Reg,
                                      int &Offset) {
  // Some boilerplate from the X86 FastISel.
  const User *U = NULL;
  Value *Op1 = I->getOperand(0);
  unsigned Opcode = Instruction::UserOp1;
  if (const Instruction *I = dyn_cast<Instruction>(Op1)) {
    // Don't walk into other basic blocks; it's possible we haven't
    // visited them yet, so the instructions may not yet be assigned
    // virtual registers.
    if (FuncInfo.MBBMap[I->getParent()] != FuncInfo.MBB)
      return false;

    Opcode = I->getOpcode();
    U = I;
  } else if (const ConstantExpr *C = dyn_cast<ConstantExpr>(Op1)) {
    Opcode = C->getOpcode();
    U = C;
  }

  if (const PointerType *Ty = dyn_cast<PointerType>(Op1->getType()))
    if (Ty->getAddressSpace() > 255)
      // Fast instruction selection doesn't support the special
      // address spaces.
      return false;
  
  switch (Opcode) {
    default: 
    //errs() << "Failing Opcode is: " << *Op1 << "\n";
    break;
    case Instruction::Alloca: {
      // Do static allocas.
      const AllocaInst *A = cast<AllocaInst>(Op1);
      DenseMap<const AllocaInst*, int>::iterator SI =
        FuncInfo.StaticAllocaMap.find(A);
      if (SI != FuncInfo.StaticAllocaMap.end())
        Offset =
          TM.getRegisterInfo()->getFrameIndexReference(*FuncInfo.MF,
                                                       SI->second, Reg);
      else
        return false;
      return true;
    }
  }
  return false;
}

bool ARMFastISel::ARMSelectLoad(const Instruction *I) {
  
  unsigned Reg;
  int Offset;
  
  // See if we can handle this as Reg + Offset
  if (!ARMComputeRegOffset(I, Reg, Offset))
    return false;
    
  unsigned ResultReg = createResultReg(ARM::GPRRegisterClass);
  AddOptionalDefs(BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DL,
                          TII.get(ARM::LDR), ResultReg)
                  .addImm(0).addReg(Reg).addImm(Offset));
        
  return true;
}

bool ARMFastISel::TargetSelectInstruction(const Instruction *I) {
  // No Thumb-1 for now.
  if (AFI->isThumbFunction() && !AFI->isThumb2Function()) return false;
  
  switch (I->getOpcode()) {
    case Instruction::Load:
      return ARMSelectLoad(I);
    default: break;
  }
  return false;
}

namespace llvm {
  llvm::FastISel *ARM::createFastISel(FunctionLoweringInfo &funcInfo) {
    if (EnableARMFastISel) return new ARMFastISel(funcInfo);
    return 0;
  }
}
