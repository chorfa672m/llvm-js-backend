//===-- PPC32ISelPattern.cpp - A pattern matching inst selector for PPC32 -===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Nate Begeman and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file defines a pattern matching instruction selector for 32 bit PowerPC.
//
//===----------------------------------------------------------------------===//

#include "PowerPC.h"
#include "PowerPCInstrBuilder.h"
#include "PowerPCInstrInfo.h"
#include "PPC32RegisterInfo.h"
#include "llvm/Constants.h"                   // FIXME: REMOVE
#include "llvm/Function.h"
#include "llvm/CodeGen/MachineConstantPool.h" // FIXME: REMOVE
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/SSARegMap.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/ADT/Statistic.h"
#include <set>
#include <algorithm>
using namespace llvm;

//===----------------------------------------------------------------------===//
//  PPC32TargetLowering - PPC32 Implementation of the TargetLowering interface
namespace {
  class PPC32TargetLowering : public TargetLowering {
    int VarArgsFrameIndex;            // FrameIndex for start of varargs area.
    int ReturnAddrIndex;              // FrameIndex for return slot.
  public:
    PPC32TargetLowering(TargetMachine &TM) : TargetLowering(TM) {
      // Set up the TargetLowering object.

      // Set up the register classes.
      addRegisterClass(MVT::i32, PPC32::GPRCRegisterClass);
      addRegisterClass(MVT::f32, PPC32::GPRCRegisterClass);
      addRegisterClass(MVT::f64, PPC32::FPRCRegisterClass);
      
      computeRegisterProperties();
    }

    /// LowerArguments - This hook must be implemented to indicate how we should
    /// lower the arguments for the specified function, into the specified DAG.
    virtual std::vector<SDOperand>
    LowerArguments(Function &F, SelectionDAG &DAG);
    
    /// LowerCallTo - This hook lowers an abstract call to a function into an
    /// actual call.
    virtual std::pair<SDOperand, SDOperand>
    LowerCallTo(SDOperand Chain, const Type *RetTy, SDOperand Callee,
                ArgListTy &Args, SelectionDAG &DAG);
    
    virtual std::pair<SDOperand, SDOperand>
    LowerVAStart(SDOperand Chain, SelectionDAG &DAG);
    
    virtual std::pair<SDOperand,SDOperand>
    LowerVAArgNext(bool isVANext, SDOperand Chain, SDOperand VAList,
                   const Type *ArgTy, SelectionDAG &DAG);

    virtual std::pair<SDOperand, SDOperand>
    LowerFrameReturnAddress(bool isFrameAddr, SDOperand Chain, unsigned Depth,
                            SelectionDAG &DAG);
  };
}


std::vector<SDOperand>
PPC32TargetLowering::LowerArguments(Function &F, SelectionDAG &DAG) {
  //
  // add beautiful description of PPC stack frame format, or at least some docs
  //
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  MachineBasicBlock& BB = MF.front();
  std::vector<SDOperand> ArgValues;
  
  // Due to the rather complicated nature of the PowerPC ABI, rather than a 
  // fixed size array of physical args, for the sake of simplicity let the STL
  // handle tracking them for us.
  std::vector<unsigned> argVR, argPR, argOp;
  unsigned ArgOffset = 24;
  unsigned GPR_remaining = 8;
  unsigned FPR_remaining = 13;
  unsigned GPR_idx = 0, FPR_idx = 0;
  static const unsigned GPR[] = { 
    PPC::R3, PPC::R4, PPC::R5, PPC::R6,
    PPC::R7, PPC::R8, PPC::R9, PPC::R10,
  };
  static const unsigned FPR[] = {
    PPC::F1, PPC::F2, PPC::F3, PPC::F4, PPC::F5, PPC::F6, PPC::F7,
    PPC::F8, PPC::F9, PPC::F10, PPC::F11, PPC::F12, PPC::F13
  };

  // Add DAG nodes to load the arguments...  On entry to a function on PPC,
  // the arguments start at offset 24, although they are likely to be passed
  // in registers.
  for (Function::arg_iterator I = F.arg_begin(), E = F.arg_end(); I != E; ++I) {
    SDOperand newroot, argt;
    unsigned ObjSize;
    bool needsLoad = false;
    MVT::ValueType ObjectVT = getValueType(I->getType());
    
    switch (ObjectVT) {
    default: assert(0 && "Unhandled argument type!");
    case MVT::i1:
    case MVT::i8:
    case MVT::i16:
    case MVT::i32: 
      ObjSize = 4;
      if (GPR_remaining > 0) {
        BuildMI(&BB, PPC::IMPLICIT_DEF, 0, GPR[GPR_idx]);
        unsigned virtReg = 
          MF.getSSARegMap()->createVirtualRegister(getRegClassFor(MVT::i32));
        argt = newroot = DAG.getCopyFromReg(virtReg, MVT::i32, DAG.getRoot());
        if (ObjectVT != MVT::i32)
          argt = DAG.getNode(ISD::TRUNCATE, ObjectVT, newroot);
        argVR.push_back(virtReg);
        argPR.push_back(GPR[GPR_idx]);
        argOp.push_back(PPC::OR);
      } else {
        needsLoad = true;
      }
      break;
      case MVT::i64: ObjSize = 8; 
      if (GPR_remaining > 1) {
        BuildMI(&BB, PPC::IMPLICIT_DEF, 0, GPR[GPR_idx]);
        BuildMI(&BB, PPC::IMPLICIT_DEF, 0, GPR[GPR_idx+1]);
        MF.getSSARegMap()->createVirtualRegister(getRegClassFor(MVT::i32));
        unsigned virtReg = 
          MF.getSSARegMap()->createVirtualRegister(getRegClassFor(MVT::i32))-1;
        // FIXME: is this correct?
        argt = newroot = DAG.getCopyFromReg(virtReg, MVT::i32, DAG.getRoot());
        argt = DAG.getCopyFromReg(virtReg+1, MVT::i32, newroot);
        // Push the arguments for emitting into BB later
        argVR.push_back(virtReg);       argVR.push_back(virtReg+1);
        argPR.push_back(GPR[GPR_idx]);  argPR.push_back(GPR[GPR_idx+1]);
        argOp.push_back(PPC::OR);       argOp.push_back(PPC::OR);
      } else {
        needsLoad = true; 
      }
      break;
      case MVT::f32: ObjSize = 4;
      case MVT::f64: ObjSize = 8;
      if (FPR_remaining > 0) {
        BuildMI(&BB, PPC::IMPLICIT_DEF, 0, FPR[FPR_idx]);
        unsigned virtReg = 
          MF.getSSARegMap()->createVirtualRegister(getRegClassFor(ObjectVT));
        argt = newroot = DAG.getCopyFromReg(virtReg, ObjectVT, DAG.getRoot());
        argVR.push_back(virtReg);
        argPR.push_back(FPR[FPR_idx]);
        argOp.push_back(PPC::FMR);
        --FPR_remaining;
        ++FPR_idx;
      } else {
        needsLoad = true;
      }
      break;
    }
    
    // We need to load the argument to a virtual register if we determined above
    // that we ran out of physical registers of the appropriate type 
    if (needsLoad) {
      int FI = MFI->CreateFixedObject(ObjSize, ArgOffset);
      SDOperand FIN = DAG.getFrameIndex(FI, MVT::i32);
      argt = newroot = DAG.getLoad(ObjectVT, DAG.getEntryNode(), FIN);
    }
    
    // Every 4 bytes of argument space consumes one of the GPRs available for
    // argument passing.
    if (GPR_remaining > 0) {
      unsigned delta = (GPR_remaining > 1 && ObjSize == 8) ? 2 : 1;
      GPR_remaining -= delta;
      GPR_idx += delta;
    }
    ArgOffset += ObjSize;
    
    DAG.setRoot(newroot.getValue(1));
    ArgValues.push_back(argt);
  }

  for (int i = 0, count = argVR.size(); i < count; ++i) {
    if (argOp[i] == PPC::FMR)
      BuildMI(&BB, argOp[i], 1, argVR[i]).addReg(argPR[i]);
    else
      BuildMI(&BB, argOp[i], 2, argVR[i]).addReg(argPR[i]).addReg(argPR[i]);
  }

  // If the function takes variable number of arguments, make a frame index for
  // the start of the first vararg value... for expansion of llvm.va_start.
  if (F.isVarArg())
    VarArgsFrameIndex = MFI->CreateFixedObject(4, ArgOffset);

  return ArgValues;
}

std::pair<SDOperand, SDOperand>
PPC32TargetLowering::LowerCallTo(SDOperand Chain,
				 const Type *RetTy, SDOperand Callee,
				 ArgListTy &Args, SelectionDAG &DAG) {
  // FIXME
  int NumBytes = 56;

  Chain = DAG.getNode(ISD::ADJCALLSTACKDOWN, MVT::Other, Chain,
		      DAG.getConstant(NumBytes, getPointerTy()));
  std::vector<SDOperand> args_to_use;
  for (unsigned i = 0, e = Args.size(); i != e; ++i)
  {
    switch (getValueType(Args[i].second)) {
    default: assert(0 && "Unexpected ValueType for argument!");
    case MVT::i1:
    case MVT::i8:
    case MVT::i16:
    case MVT::i32:
    case MVT::i64:
    case MVT::f64:
    case MVT::f32:
      break;
    }
    args_to_use.push_back(Args[i].first);
  }
  
  std::vector<MVT::ValueType> RetVals;
  MVT::ValueType RetTyVT = getValueType(RetTy);
  if (RetTyVT != MVT::isVoid)
    RetVals.push_back(RetTyVT);
  RetVals.push_back(MVT::Other);

  SDOperand TheCall = SDOperand(DAG.getCall(RetVals, 
                                            Chain, Callee, args_to_use), 0);
  Chain = TheCall.getValue(RetTyVT != MVT::isVoid);
  Chain = DAG.getNode(ISD::ADJCALLSTACKUP, MVT::Other, Chain,
                      DAG.getConstant(NumBytes, getPointerTy()));
  return std::make_pair(TheCall, Chain);
}

std::pair<SDOperand, SDOperand>
PPC32TargetLowering::LowerVAStart(SDOperand Chain, SelectionDAG &DAG) {
  //vastart just returns the address of the VarArgsFrameIndex slot.
  return std::make_pair(DAG.getFrameIndex(VarArgsFrameIndex, MVT::i32), Chain);
}

std::pair<SDOperand,SDOperand> PPC32TargetLowering::
LowerVAArgNext(bool isVANext, SDOperand Chain, SDOperand VAList,
               const Type *ArgTy, SelectionDAG &DAG) {
  abort();
}
               

std::pair<SDOperand, SDOperand> PPC32TargetLowering::
LowerFrameReturnAddress(bool isFrameAddress, SDOperand Chain, unsigned Depth,
                        SelectionDAG &DAG) {
  abort();
}

namespace {

//===--------------------------------------------------------------------===//
/// ISel - PPC32 specific code to select PPC32 machine instructions for
/// SelectionDAG operations.
//===--------------------------------------------------------------------===//
class ISel : public SelectionDAGISel {
  
  /// Comment Here.
  PPC32TargetLowering PPC32Lowering;
  
  /// ExprMap - As shared expressions are codegen'd, we keep track of which
  /// vreg the value is produced in, so we only emit one copy of each compiled
  /// tree.
  std::map<SDOperand, unsigned> ExprMap;
  
public:
  ISel(TargetMachine &TM) : SelectionDAGISel(PPC32Lowering), PPC32Lowering(TM) 
  {}
  
  /// InstructionSelectBasicBlock - This callback is invoked by
  /// SelectionDAGISel when it has created a SelectionDAG for us to codegen.
  virtual void InstructionSelectBasicBlock(SelectionDAG &DAG) {
    DEBUG(BB->dump());
    // Codegen the basic block.
    Select(DAG.getRoot());
    
    // Clear state used for selection.
    ExprMap.clear();
  }
  
  unsigned SelectExpr(SDOperand N);
  unsigned SelectExprFP(SDOperand N, unsigned Result);
  void Select(SDOperand N);
  
  void SelectAddr(SDOperand N, unsigned& Reg, int& offset);
  void SelectBranchCC(SDOperand N);
};

/// canUseAsImmediateForOpcode - This method returns a value indicating whether
/// the ConstantSDNode N can be used as an immediate to Opcode.  The return
/// values are either 0, 1 or 2.  0 indicates that either N is not a
/// ConstantSDNode, or is not suitable for use by that opcode.  A return value 
/// of 1 indicates that the constant may be used in normal immediate form.  A
/// return value of 2 indicates that the constant may be used in shifted
/// immediate form.  If the return value is nonzero, the constant value is
/// placed in Imm.
///
static unsigned canUseAsImmediateForOpcode(SDOperand N, unsigned Opcode,
                                           unsigned& Imm) {
  if (N.getOpcode() != ISD::Constant) return 0;

  int v = (int)cast<ConstantSDNode>(N)->getSignExtended();
  
  switch(Opcode) {
  default: return 0;
  case ISD::ADD:
    if (v <= 32767 && v >= -32768) { Imm = v & 0xFFFF; return 1; }
    if ((v & 0x0000FFFF) == 0) { Imm = v >> 16; return 2; }
    break;
  case ISD::AND:
  case ISD::XOR:
  case ISD::OR:
    if (v >= 0 && v <= 65535) { Imm = v & 0xFFFF; return 1; }
    if ((v & 0x0000FFFF) == 0) { Imm = v >> 16; return 2; }
    break;
  }
  return 0;
}
}

//Check to see if the load is a constant offset from a base register
void ISel::SelectAddr(SDOperand N, unsigned& Reg, int& offset)
{
  Reg = SelectExpr(N);
  offset = 0;
  return;
}

void ISel::SelectBranchCC(SDOperand N)
{
  assert(N.getOpcode() == ISD::BRCOND && "Not a BranchCC???");
  MachineBasicBlock *Dest = 
    cast<BasicBlockSDNode>(N.getOperand(2))->getBasicBlock();
  unsigned Opc;
  
  Select(N.getOperand(0));  //chain
  SDOperand CC = N.getOperand(1);
  
  //Giveup and do the stupid thing
  unsigned Tmp1 = SelectExpr(CC);
  BuildMI(BB, PPC::BNE, 2).addReg(Tmp1).addMBB(Dest);
  return;
}

unsigned ISel::SelectExprFP(SDOperand N, unsigned Result)
{
  unsigned Tmp1, Tmp2, Tmp3;
  unsigned Opc = 0;
  SDNode *Node = N.Val;
  MVT::ValueType DestType = N.getValueType();
  unsigned opcode = N.getOpcode();

  switch (opcode) {
  default:
    Node->dump();
    assert(0 && "Node not handled!\n");

  case ISD::SELECT:
    abort();
    
  case ISD::FP_ROUND:
    assert (DestType == MVT::f32 && 
            N.getOperand(0).getValueType() == MVT::f64 && 
            "only f64 to f32 conversion supported here");
    Tmp1 = SelectExpr(N.getOperand(0));
    BuildMI(BB, PPC::FRSP, 1, Result).addReg(Tmp1);
    return Result;

  case ISD::FP_EXTEND:
    assert (DestType == MVT::f64 && 
            N.getOperand(0).getValueType() == MVT::f32 && 
            "only f32 to f64 conversion supported here");
    Tmp1 = SelectExpr(N.getOperand(0));
    BuildMI(BB, PPC::FMR, 1, Result).addReg(Tmp1);
    return Result;

  case ISD::CopyFromReg:
    // FIXME: Handle copy from physregs!
    // Just use the specified register as our input.
    return dyn_cast<RegSDNode>(Node)->getReg();
    
  case ISD::LOAD:
  case ISD::EXTLOAD:
    abort();
    
  case ISD::ConstantFP:
    abort();
    
  case ISD::MUL:
  case ISD::ADD:
  case ISD::SUB:
  case ISD::SDIV:
    switch( opcode ) {
    case ISD::MUL:  Opc = DestType == MVT::f64 ? PPC::FMUL : PPC::FMULS; break;
    case ISD::ADD:  Opc = DestType == MVT::f64 ? PPC::FADD : PPC::FADDS; break;
    case ISD::SUB:  Opc = DestType == MVT::f64 ? PPC::FSUB : PPC::FSUBS; break;
    case ISD::SDIV: Opc = DestType == MVT::f64 ? PPC::FDIV : PPC::FDIVS; break;
    };

    Tmp1 = SelectExpr(N.getOperand(0));
    Tmp2 = SelectExpr(N.getOperand(1));
    BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addReg(Tmp2);
    return Result;

  case ISD::UINT_TO_FP:
  case ISD::SINT_TO_FP:
    abort();
  }
  assert(0 && "should not get here");
  return 0;
}

unsigned ISel::SelectExpr(SDOperand N) {
  unsigned Result;
  unsigned Tmp1, Tmp2, Tmp3;
  unsigned Opc = 0;
  unsigned opcode = N.getOpcode();

  SDNode *Node = N.Val;
  MVT::ValueType DestType = N.getValueType();

  unsigned &Reg = ExprMap[N];
  if (Reg) return Reg;

  if (DestType == MVT::f64 || DestType == MVT::f32)
    return SelectExprFP(N, Result);

  if (N.getOpcode() != ISD::CALL)
    Reg = Result = (N.getValueType() != MVT::Other) ?
      MakeReg(N.getValueType()) : 1;
  else
    abort(); // FIXME: Implement Call

  switch (opcode) {
  default:
    Node->dump();
    assert(0 && "Node not handled!\n");
 
  case ISD::DYNAMIC_STACKALLOC:
    // Generate both result values.  FIXME: Need a better commment here?
    if (Result != 1)
      ExprMap[N.getValue(1)] = 1;
    else
      Result = ExprMap[N.getValue(0)] = MakeReg(N.getValue(0).getValueType());

    // FIXME: We are currently ignoring the requested alignment for handling
    // greater than the stack alignment.  This will need to be revisited at some
    // point.  Align = N.getOperand(2);
    if (!isa<ConstantSDNode>(N.getOperand(2)) ||
        cast<ConstantSDNode>(N.getOperand(2))->getValue() != 0) {
      std::cerr << "Cannot allocate stack object with greater alignment than"
                << " the stack alignment yet!";
      abort();
    }
    Select(N.getOperand(0));
    Tmp1 = SelectExpr(N.getOperand(1));
    // Subtract size from stack pointer, thereby allocating some space.
    BuildMI(BB, PPC::SUBF, 2, PPC::R1).addReg(Tmp1).addReg(PPC::R1);
    // Put a pointer to the space into the result register by copying the SP
    BuildMI(BB, PPC::OR, 2, Result).addReg(PPC::R1).addReg(PPC::R1);
    return Result;

  case ISD::ConstantPool:
    abort();

  case ISD::FrameIndex:
    abort();
  
  case ISD::LOAD:
  case ISD::EXTLOAD:
  case ISD::ZEXTLOAD:
  {
    // Make sure we generate both values.
    if (Result != 1)
      ExprMap[N.getValue(1)] = 1;   // Generate the token
    else
      Result = ExprMap[N.getValue(0)] = MakeReg(N.getValue(0).getValueType());

    SDOperand Chain   = N.getOperand(0);
    SDOperand Address = N.getOperand(1);
    Select(Chain);

    switch (Node->getValueType(0)) {
    default: assert(0 && "Cannot load this type!");
    case MVT::i1:
    case MVT::i8:  Opc = PPC::LBZ; break;
    case MVT::i16: Opc = PPC::LHZ; break;
    case MVT::i32: Opc = PPC::LWZ; break;
    }
    
    if (Address.getOpcode() == ISD::GlobalAddress) {  // FIXME
      BuildMI(BB, Opc, 2, Result)
        .addGlobalAddress(cast<GlobalAddressSDNode>(Address)->getGlobal())
        .addReg(PPC::R1);
    }
    else if (ConstantPoolSDNode *CP = dyn_cast<ConstantPoolSDNode>(Address)) {
      BuildMI(BB, Opc, 2, Result).addConstantPoolIndex(CP->getIndex())
        .addReg(PPC::R1);
    }
    else if(Address.getOpcode() == ISD::FrameIndex) {
      BuildMI(BB, Opc, 2, Result)
      .addFrameIndex(cast<FrameIndexSDNode>(Address)->getIndex())
      .addReg(PPC::R1);
    } else {
      int offset;
      SelectAddr(Address, Tmp1, offset);
      BuildMI(BB, Opc, 2, Result).addSImm(offset).addReg(Tmp1);
    }
    return Result;
  }
    
  case ISD::SEXTLOAD:
  case ISD::GlobalAddress:
  case ISD::CALL:
    abort();

  case ISD::SIGN_EXTEND:
  case ISD::SIGN_EXTEND_INREG:
    Tmp1 = SelectExpr(N.getOperand(0));
    BuildMI(BB, PPC::EXTSH, 1, Result).addReg(Tmp1);
    return Result;
    
  case ISD::ZERO_EXTEND_INREG:
    Tmp1 = SelectExpr(N.getOperand(0));
    switch(cast<MVTSDNode>(Node)->getExtraValueType()) {
    default:
      Node->dump();
      assert(0 && "Zero Extend InReg not there yet");
      break;
    case MVT::i16:  Tmp2 = 16; break;
    case MVT::i8:   Tmp2 = 24; break;
    case MVT::i1:   Tmp2 = 31; break;
    }
    BuildMI(BB, PPC::RLWINM, 5, Result).addReg(Tmp1).addImm(0).addImm(0)
      .addImm(Tmp2).addImm(31);
    return Result;
    
  case ISD::SETCC:
    abort();
    
  case ISD::CopyFromReg:
    if (Result == 1)
      Result = ExprMap[N.getValue(0)] = MakeReg(N.getValue(0).getValueType());
    Tmp1 = dyn_cast<RegSDNode>(Node)->getReg();
    BuildMI(BB, PPC::OR, 2, Result).addReg(Tmp1).addReg(Tmp1);
    return Result;

  case ISD::SHL:
    Tmp1 = SelectExpr(N.getOperand(0));
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      Tmp2 = CN->getValue() & 0x1F;
      BuildMI(BB, PPC::RLWINM, 5, Result).addReg(Tmp1).addImm(Tmp2).addImm(0)
        .addImm(31-Tmp2);
    } else {
      Tmp2 = SelectExpr(N.getOperand(1));
      BuildMI(BB, PPC::SLW, 2, Result).addReg(Tmp1).addReg(Tmp2);
    }
    return Result;
    
  case ISD::SRL:
    Tmp1 = SelectExpr(N.getOperand(0));
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      Tmp2 = CN->getValue() & 0x1F;
      BuildMI(BB, PPC::RLWINM, 5, Result).addReg(Tmp1).addImm(32-Tmp2)
        .addImm(Tmp2).addImm(31);
    } else {
      Tmp2 = SelectExpr(N.getOperand(1));
      BuildMI(BB, PPC::SRW, 2, Result).addReg(Tmp1).addReg(Tmp2);
    }
    return Result;
    
  case ISD::SRA:
    Tmp1 = SelectExpr(N.getOperand(0));
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      Tmp2 = CN->getValue() & 0x1F;
      BuildMI(BB, PPC::SRAWI, 2, Result).addReg(Tmp1).addImm(Tmp2);
    } else {
      Tmp2 = SelectExpr(N.getOperand(1));
      BuildMI(BB, PPC::SRAW, 2, Result).addReg(Tmp1).addReg(Tmp2);
    }
    return Result;
  
  case ISD::ADD:
    assert (DestType == MVT::i32 && "Only do arithmetic on i32s!");
    Tmp1 = SelectExpr(N.getOperand(0));
    switch(canUseAsImmediateForOpcode(N.getOperand(1), opcode, Tmp2)) {
      default: assert(0 && "unhandled result code");
      case 0: // No immediate
        Tmp2 = SelectExpr(N.getOperand(1));
        BuildMI(BB, PPC::ADD, 2, Result).addReg(Tmp1).addReg(Tmp2);
        break;
      case 1: // Low immediate
        BuildMI(BB, PPC::ADDI, 2, Result).addReg(Tmp1).addSImm(Tmp2);
        break;
      case 2: // Shifted immediate
        BuildMI(BB, PPC::ADDIS, 2, Result).addReg(Tmp1).addSImm(Tmp2);
        break;
    }
    return Result;
    
  case ISD::SUB:
    assert (DestType == MVT::i32 && "Only do arithmetic on i32s!");
    Tmp1 = SelectExpr(N.getOperand(0));
    Tmp2 = SelectExpr(N.getOperand(1));
    BuildMI(BB, PPC::SUBF, 2, Result).addReg(Tmp2).addReg(Tmp1);
    return Result;
 
  case ISD::AND:
  case ISD::OR:
  case ISD::XOR:
    assert (DestType == MVT::i32 && "Only do arithmetic on i32s!");
    Tmp1 = SelectExpr(N.getOperand(0));
    switch(canUseAsImmediateForOpcode(N.getOperand(1), opcode, Tmp2)) {
      default: assert(0 && "unhandled result code");
      case 0: // No immediate
        Tmp2 = SelectExpr(N.getOperand(1));
        switch (opcode) {
        case ISD::AND: Opc = PPC::AND; break;
        case ISD::OR:  Opc = PPC::OR;  break;
        case ISD::XOR: Opc = PPC::XOR; break;
        }
        BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addReg(Tmp2);
        break;
      case 1: // Low immediate
        switch (opcode) {
        case ISD::AND: Opc = PPC::ANDIo; break;
        case ISD::OR:  Opc = PPC::ORI;   break;
        case ISD::XOR: Opc = PPC::XORI;  break;
        }
        BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addImm(Tmp2);
        break;
      case 2: // Shifted immediate
        switch (opcode) {
        case ISD::AND: Opc = PPC::ANDISo;  break;
        case ISD::OR:  Opc = PPC::ORIS;    break;
        case ISD::XOR: Opc = PPC::XORIS;   break;
        }
        BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addImm(Tmp2);
        break;
    }
    return Result;
    
  case ISD::MUL:
  case ISD::UREM:
  case ISD::SREM:
  case ISD::SDIV:
  case ISD::UDIV:
    abort();

  case ISD::FP_TO_UINT:
  case ISD::FP_TO_SINT:
    abort();
 
  case ISD::SELECT:
    abort();

  case ISD::Constant:
    switch (N.getValueType()) {
    default: assert(0 && "Cannot use constants of this type!");
    case MVT::i1:
      BuildMI(BB, PPC::LI, 1, Result)
        .addSImm(!cast<ConstantSDNode>(N)->isNullValue());
      break;
    case MVT::i32:
      {
        int v = (int)cast<ConstantSDNode>(N)->getSignExtended();
        if (v < 32768 && v >= -32768) {
          BuildMI(BB, PPC::LI, 1, Result).addSImm(v);
        } else {
          Tmp1 = MakeReg(MVT::i32);
          BuildMI(BB, PPC::LIS, 1, Tmp1).addSImm(v >> 16);
          BuildMI(BB, PPC::ORI, 2, Result).addReg(Tmp1).addImm(v & 0xFFFF);
        }
      }
    }
    return Result;
  }

  return 0;
}

void ISel::Select(SDOperand N) {
  unsigned Tmp1, Tmp2, Opc;
  unsigned opcode = N.getOpcode();

  if (!ExprMap.insert(std::make_pair(N, 1)).second)
    return;  // Already selected.

  SDNode *Node = N.Val;
  
  switch (Node->getOpcode()) {
  default:
    Node->dump(); std::cerr << "\n";
    assert(0 && "Node not handled yet!");
  case ISD::EntryToken: return;  // Noop
  case ISD::TokenFactor:
    for (unsigned i = 0, e = Node->getNumOperands(); i != e; ++i)
      Select(Node->getOperand(i));
    return;
  case ISD::ADJCALLSTACKDOWN:
  case ISD::ADJCALLSTACKUP:
    Select(N.getOperand(0));
    Tmp1 = cast<ConstantSDNode>(N.getOperand(1))->getValue();
    Opc = N.getOpcode() == ISD::ADJCALLSTACKDOWN ? PPC::ADJCALLSTACKDOWN :
      PPC::ADJCALLSTACKUP;
    BuildMI(BB, Opc, 1).addImm(Tmp1);
    return;
  case ISD::BR: {
    MachineBasicBlock *Dest =
      cast<BasicBlockSDNode>(N.getOperand(1))->getBasicBlock();
    Select(N.getOperand(0));
    BuildMI(BB, PPC::B, 1).addMBB(Dest);
    return;
  }
  case ISD::BRCOND: 
    SelectBranchCC(N);
    return;
  case ISD::CopyToReg:
    Select(N.getOperand(0));
    Tmp1 = SelectExpr(N.getOperand(1));
    Tmp2 = cast<RegSDNode>(N)->getReg();
    
    if (Tmp1 != Tmp2) {
      if (N.getOperand(1).getValueType() == MVT::f64 || 
          N.getOperand(1).getValueType() == MVT::f32)
        BuildMI(BB, PPC::FMR, 1, Tmp2).addReg(Tmp1);
      else
        BuildMI(BB, PPC::OR, 2, Tmp2).addReg(Tmp1).addReg(Tmp1);
    }
    return;
  case ISD::ImplicitDef:
    Select(N.getOperand(0));
    BuildMI(BB, PPC::IMPLICIT_DEF, 0, cast<RegSDNode>(N)->getReg());
    return;
  case ISD::RET:
    switch (N.getNumOperands()) {
    default:
      assert(0 && "Unknown return instruction!");
    case 3:
      assert(N.getOperand(1).getValueType() == MVT::i32 &&
             N.getOperand(2).getValueType() == MVT::i32 &&
	           "Unknown two-register value!");
      Select(N.getOperand(0));
      Tmp1 = SelectExpr(N.getOperand(1));
      Tmp2 = SelectExpr(N.getOperand(2));
      BuildMI(BB, PPC::OR, 2, PPC::R3).addReg(Tmp1).addReg(Tmp1);
      BuildMI(BB, PPC::OR, 2, PPC::R4).addReg(Tmp2).addReg(Tmp2);
      break;
    case 2:
      Select(N.getOperand(0));
      Tmp1 = SelectExpr(N.getOperand(1));
      switch (N.getOperand(1).getValueType()) {
        default:
          assert(0 && "Unknown return type!");
        case MVT::f64:
        case MVT::f32:
          BuildMI(BB, PPC::FMR, 1, PPC::F1).addReg(Tmp1);
          break;
        case MVT::i32:
          BuildMI(BB, PPC::OR, 2, PPC::R3).addReg(Tmp1).addReg(Tmp1);
          break;
      }
    }
    BuildMI(BB, PPC::BLR, 0); // Just emit a 'ret' instruction
    return;
  case ISD::TRUNCSTORE: 
  case ISD::STORE: 
    {
      SDOperand Chain   = N.getOperand(0);
      SDOperand Value   = N.getOperand(1);
      SDOperand Address = N.getOperand(2);
      Select(Chain);

      Tmp1 = SelectExpr(Value); //value

      if (opcode == ISD::STORE) {
        switch(Value.getValueType()) {
        default: assert(0 && "unknown Type in store");
        case MVT::i32: Opc = PPC::STW; break;
        case MVT::f64: Opc = PPC::STFD; break;
        case MVT::f32: Opc = PPC::STFS; break;
        }
      } else { //ISD::TRUNCSTORE
        switch(cast<MVTSDNode>(Node)->getExtraValueType()) {
        default: assert(0 && "unknown Type in store");
        case MVT::i1: //FIXME: DAG does not promote this load
        case MVT::i8: Opc  = PPC::STB; break;
        case MVT::i16: Opc = PPC::STH; break;
        }
      }

      if (Address.getOpcode() == ISD::GlobalAddress)
      {
        BuildMI(BB, Opc, 2).addReg(Tmp1)
          .addGlobalAddress(cast<GlobalAddressSDNode>(Address)->getGlobal());
      }
      else if(Address.getOpcode() == ISD::FrameIndex)
      {
        BuildMI(BB, Opc, 2).addReg(Tmp1)
          .addFrameIndex(cast<FrameIndexSDNode>(Address)->getIndex());
      }
      else
      {
        int offset;
        SelectAddr(Address, Tmp2, offset);
        BuildMI(BB, Opc, 3).addReg(Tmp1).addImm(offset).addReg(Tmp2);
      }
      return;
    }
  case ISD::EXTLOAD:
  case ISD::SEXTLOAD:
  case ISD::ZEXTLOAD:
  case ISD::LOAD:
  case ISD::CopyFromReg:
  case ISD::CALL:
  case ISD::DYNAMIC_STACKALLOC:
    ExprMap.erase(N);
    SelectExpr(N);
    return;
  }
  assert(0 && "Should not be reached!");
}


/// createPPC32PatternInstructionSelector - This pass converts an LLVM function
/// into a machine code representation using pattern matching and a machine
/// description file.
///
FunctionPass *llvm::createPPC32ISelPattern(TargetMachine &TM) {
  return new ISel(TM);  
}

