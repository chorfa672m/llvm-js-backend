//===-- MSP430ISelLowering.cpp - MSP430 DAG Lowering Implementation  ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the MSP430TargetLowering class.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "msp430-lower"

#include "MSP430ISelLowering.h"
#include "MSP430.h"
#include "MSP430TargetMachine.h"
#include "MSP430Subtarget.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Intrinsics.h"
#include "llvm/CallingConv.h"
#include "llvm/GlobalVariable.h"
#include "llvm/GlobalAlias.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/ADT/VectorExtras.h"
using namespace llvm;

MSP430TargetLowering::MSP430TargetLowering(MSP430TargetMachine &tm) :
  TargetLowering(tm, new TargetLoweringObjectFileELF()),
  Subtarget(*tm.getSubtargetImpl()), TM(tm) {

  // Set up the register classes.
  addRegisterClass(EVT::i8,  MSP430::GR8RegisterClass);
  addRegisterClass(EVT::i16, MSP430::GR16RegisterClass);

  // Compute derived properties from the register classes
  computeRegisterProperties();

  // Provide all sorts of operation actions

  // Division is expensive
  setIntDivIsCheap(false);

  // Even if we have only 1 bit shift here, we can perform
  // shifts of the whole bitwidth 1 bit per step.
  setShiftAmountType(EVT::i8);

  setStackPointerRegisterToSaveRestore(MSP430::SPW);
  setBooleanContents(ZeroOrOneBooleanContent);
  setSchedulingPreference(SchedulingForLatency);

  setLoadExtAction(ISD::EXTLOAD,  EVT::i1, Promote);
  setLoadExtAction(ISD::SEXTLOAD, EVT::i1, Promote);
  setLoadExtAction(ISD::ZEXTLOAD, EVT::i1, Promote);
  setLoadExtAction(ISD::SEXTLOAD, EVT::i8, Expand);
  setLoadExtAction(ISD::SEXTLOAD, EVT::i16, Expand);

  // We don't have any truncstores
  setTruncStoreAction(EVT::i16, EVT::i8, Expand);

  setOperationAction(ISD::SRA,              EVT::i8,    Custom);
  setOperationAction(ISD::SHL,              EVT::i8,    Custom);
  setOperationAction(ISD::SRL,              EVT::i8,    Custom);
  setOperationAction(ISD::SRA,              EVT::i16,   Custom);
  setOperationAction(ISD::SHL,              EVT::i16,   Custom);
  setOperationAction(ISD::SRL,              EVT::i16,   Custom);
  setOperationAction(ISD::ROTL,             EVT::i8,    Expand);
  setOperationAction(ISD::ROTR,             EVT::i8,    Expand);
  setOperationAction(ISD::ROTL,             EVT::i16,   Expand);
  setOperationAction(ISD::ROTR,             EVT::i16,   Expand);
  setOperationAction(ISD::GlobalAddress,    EVT::i16,   Custom);
  setOperationAction(ISD::ExternalSymbol,   EVT::i16,   Custom);
  setOperationAction(ISD::BR_JT,            EVT::Other, Expand);
  setOperationAction(ISD::BRIND,            EVT::Other, Expand);
  setOperationAction(ISD::BR_CC,            EVT::i8,    Custom);
  setOperationAction(ISD::BR_CC,            EVT::i16,   Custom);
  setOperationAction(ISD::BRCOND,           EVT::Other, Expand);
  setOperationAction(ISD::SETCC,            EVT::i8,    Expand);
  setOperationAction(ISD::SETCC,            EVT::i16,   Expand);
  setOperationAction(ISD::SELECT,           EVT::i8,    Expand);
  setOperationAction(ISD::SELECT,           EVT::i16,   Expand);
  setOperationAction(ISD::SELECT_CC,        EVT::i8,    Custom);
  setOperationAction(ISD::SELECT_CC,        EVT::i16,   Custom);
  setOperationAction(ISD::SIGN_EXTEND,      EVT::i16,   Custom);

  setOperationAction(ISD::CTTZ,             EVT::i8,    Expand);
  setOperationAction(ISD::CTTZ,             EVT::i16,   Expand);
  setOperationAction(ISD::CTLZ,             EVT::i8,    Expand);
  setOperationAction(ISD::CTLZ,             EVT::i16,   Expand);
  setOperationAction(ISD::CTPOP,            EVT::i8,    Expand);
  setOperationAction(ISD::CTPOP,            EVT::i16,   Expand);

  setOperationAction(ISD::SHL_PARTS,        EVT::i8,    Expand);
  setOperationAction(ISD::SHL_PARTS,        EVT::i16,   Expand);
  setOperationAction(ISD::SRL_PARTS,        EVT::i8,    Expand);
  setOperationAction(ISD::SRL_PARTS,        EVT::i16,   Expand);
  setOperationAction(ISD::SRA_PARTS,        EVT::i8,    Expand);
  setOperationAction(ISD::SRA_PARTS,        EVT::i16,   Expand);

  setOperationAction(ISD::SIGN_EXTEND_INREG, EVT::i1,   Expand);

  // FIXME: Implement efficiently multiplication by a constant
  setOperationAction(ISD::MUL,              EVT::i16,   Expand);
  setOperationAction(ISD::MULHS,            EVT::i16,   Expand);
  setOperationAction(ISD::MULHU,            EVT::i16,   Expand);
  setOperationAction(ISD::SMUL_LOHI,        EVT::i16,   Expand);
  setOperationAction(ISD::UMUL_LOHI,        EVT::i16,   Expand);

  setOperationAction(ISD::UDIV,             EVT::i16,   Expand);
  setOperationAction(ISD::UDIVREM,          EVT::i16,   Expand);
  setOperationAction(ISD::UREM,             EVT::i16,   Expand);
  setOperationAction(ISD::SDIV,             EVT::i16,   Expand);
  setOperationAction(ISD::SDIVREM,          EVT::i16,   Expand);
  setOperationAction(ISD::SREM,             EVT::i16,   Expand);
}

SDValue MSP430TargetLowering::LowerOperation(SDValue Op, SelectionDAG &DAG) {
  switch (Op.getOpcode()) {
  case ISD::SHL: // FALLTHROUGH
  case ISD::SRL:
  case ISD::SRA:              return LowerShifts(Op, DAG);
  case ISD::GlobalAddress:    return LowerGlobalAddress(Op, DAG);
  case ISD::ExternalSymbol:   return LowerExternalSymbol(Op, DAG);
  case ISD::BR_CC:            return LowerBR_CC(Op, DAG);
  case ISD::SELECT_CC:        return LowerSELECT_CC(Op, DAG);
  case ISD::SIGN_EXTEND:      return LowerSIGN_EXTEND(Op, DAG);
  default:
    llvm_unreachable("unimplemented operand");
    return SDValue();
  }
}

/// getFunctionAlignment - Return the Log2 alignment of this function.
unsigned MSP430TargetLowering::getFunctionAlignment(const Function *F) const {
  return F->hasFnAttr(Attribute::OptimizeForSize) ? 1 : 4;
}

//===----------------------------------------------------------------------===//
//                      Calling Convention Implementation
//===----------------------------------------------------------------------===//

#include "MSP430GenCallingConv.inc"

SDValue
MSP430TargetLowering::LowerFormalArguments(SDValue Chain,
                                           unsigned CallConv,
                                           bool isVarArg,
                                           const SmallVectorImpl<ISD::InputArg>
                                             &Ins,
                                           DebugLoc dl,
                                           SelectionDAG &DAG,
                                           SmallVectorImpl<SDValue> &InVals) {

  switch (CallConv) {
  default:
    llvm_unreachable("Unsupported calling convention");
  case CallingConv::C:
  case CallingConv::Fast:
    return LowerCCCArguments(Chain, CallConv, isVarArg, Ins, dl, DAG, InVals);
  }
}

SDValue
MSP430TargetLowering::LowerCall(SDValue Chain, SDValue Callee,
                                unsigned CallConv, bool isVarArg,
                                bool isTailCall,
                                const SmallVectorImpl<ISD::OutputArg> &Outs,
                                const SmallVectorImpl<ISD::InputArg> &Ins,
                                DebugLoc dl, SelectionDAG &DAG,
                                SmallVectorImpl<SDValue> &InVals) {

  switch (CallConv) {
  default:
    llvm_unreachable("Unsupported calling convention");
  case CallingConv::Fast:
  case CallingConv::C:
    return LowerCCCCallTo(Chain, Callee, CallConv, isVarArg, isTailCall,
                          Outs, Ins, dl, DAG, InVals);
  }
}

/// LowerCCCArguments - transform physical registers into virtual registers and
/// generate load operations for arguments places on the stack.
// FIXME: struct return stuff
// FIXME: varargs
SDValue
MSP430TargetLowering::LowerCCCArguments(SDValue Chain,
                                        unsigned CallConv,
                                        bool isVarArg,
                                        const SmallVectorImpl<ISD::InputArg>
                                          &Ins,
                                        DebugLoc dl,
                                        SelectionDAG &DAG,
                                        SmallVectorImpl<SDValue> &InVals) {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();

  // Assign locations to all of the incoming arguments.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, getTargetMachine(),
                 ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_MSP430);

  assert(!isVarArg && "Varargs not supported yet");

  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];
    if (VA.isRegLoc()) {
      // Arguments passed in registers
      EVT RegVT = VA.getLocVT();
      switch (RegVT.getSimpleVT()) {
      default: 
        {
#ifndef NDEBUG
          cerr << "LowerFormalArguments Unhandled argument type: "
               << RegVT.getSimpleVT() << "\n";
#endif
          llvm_unreachable(0);
        }
      case EVT::i16:
        unsigned VReg =
          RegInfo.createVirtualRegister(MSP430::GR16RegisterClass);
        RegInfo.addLiveIn(VA.getLocReg(), VReg);
        SDValue ArgValue = DAG.getCopyFromReg(Chain, dl, VReg, RegVT);

        // If this is an 8-bit value, it is really passed promoted to 16
        // bits. Insert an assert[sz]ext to capture this, then truncate to the
        // right size.
        if (VA.getLocInfo() == CCValAssign::SExt)
          ArgValue = DAG.getNode(ISD::AssertSext, dl, RegVT, ArgValue,
                                 DAG.getValueType(VA.getValVT()));
        else if (VA.getLocInfo() == CCValAssign::ZExt)
          ArgValue = DAG.getNode(ISD::AssertZext, dl, RegVT, ArgValue,
                                 DAG.getValueType(VA.getValVT()));

        if (VA.getLocInfo() != CCValAssign::Full)
          ArgValue = DAG.getNode(ISD::TRUNCATE, dl, VA.getValVT(), ArgValue);

        InVals.push_back(ArgValue);
      }
    } else {
      // Sanity check
      assert(VA.isMemLoc());
      // Load the argument to a virtual register
      unsigned ObjSize = VA.getLocVT().getSizeInBits()/8;
      if (ObjSize > 2) {
        cerr << "LowerFormalArguments Unhandled argument type: "
             << VA.getLocVT().getSimpleVT()
             << "\n";
      }
      // Create the frame index object for this incoming parameter...
      int FI = MFI->CreateFixedObject(ObjSize, VA.getLocMemOffset());

      // Create the SelectionDAG nodes corresponding to a load
      //from this parameter
      SDValue FIN = DAG.getFrameIndex(FI, EVT::i16);
      InVals.push_back(DAG.getLoad(VA.getLocVT(), dl, Chain, FIN,
                                   PseudoSourceValue::getFixedStack(FI), 0));
    }
  }

  return Chain;
}

SDValue
MSP430TargetLowering::LowerReturn(SDValue Chain,
                                  unsigned CallConv, bool isVarArg,
                                  const SmallVectorImpl<ISD::OutputArg> &Outs,
                                  DebugLoc dl, SelectionDAG &DAG) {

  // CCValAssign - represent the assignment of the return value to a location
  SmallVector<CCValAssign, 16> RVLocs;

  // CCState - Info about the registers and stack slot.
  CCState CCInfo(CallConv, isVarArg, getTargetMachine(),
                 RVLocs, *DAG.getContext());

  // Analize return values.
  CCInfo.AnalyzeReturn(Outs, RetCC_MSP430);

  // If this is the first return lowered for this function, add the regs to the
  // liveout set for the function.
  if (DAG.getMachineFunction().getRegInfo().liveout_empty()) {
    for (unsigned i = 0; i != RVLocs.size(); ++i)
      if (RVLocs[i].isRegLoc())
        DAG.getMachineFunction().getRegInfo().addLiveOut(RVLocs[i].getLocReg());
  }

  SDValue Flag;

  // Copy the result values into the output registers.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    CCValAssign &VA = RVLocs[i];
    assert(VA.isRegLoc() && "Can only return in registers!");

    Chain = DAG.getCopyToReg(Chain, dl, VA.getLocReg(),
                             Outs[i].Val, Flag);

    // Guarantee that all emitted copies are stuck together,
    // avoiding something bad.
    Flag = Chain.getValue(1);
  }

  if (Flag.getNode())
    return DAG.getNode(MSP430ISD::RET_FLAG, dl, EVT::Other, Chain, Flag);

  // Return Void
  return DAG.getNode(MSP430ISD::RET_FLAG, dl, EVT::Other, Chain);
}

/// LowerCCCCallTo - functions arguments are copied from virtual regs to
/// (physical regs)/(stack frame), CALLSEQ_START and CALLSEQ_END are emitted.
/// TODO: sret.
SDValue
MSP430TargetLowering::LowerCCCCallTo(SDValue Chain, SDValue Callee,
                                     unsigned CallConv, bool isVarArg,
                                     bool isTailCall,
                                     const SmallVectorImpl<ISD::OutputArg>
                                       &Outs,
                                     const SmallVectorImpl<ISD::InputArg> &Ins,
                                     DebugLoc dl, SelectionDAG &DAG,
                                     SmallVectorImpl<SDValue> &InVals) {
  // Analyze operands of the call, assigning locations to each operand.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, getTargetMachine(),
                 ArgLocs, *DAG.getContext());

  CCInfo.AnalyzeCallOperands(Outs, CC_MSP430);

  // Get a count of how many bytes are to be pushed on the stack.
  unsigned NumBytes = CCInfo.getNextStackOffset();

  Chain = DAG.getCALLSEQ_START(Chain ,DAG.getConstant(NumBytes,
                                                      getPointerTy(), true));

  SmallVector<std::pair<unsigned, SDValue>, 4> RegsToPass;
  SmallVector<SDValue, 12> MemOpChains;
  SDValue StackPtr;

  // Walk the register/memloc assignments, inserting copies/loads.
  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];

    SDValue Arg = Outs[i].Val;

    // Promote the value if needed.
    switch (VA.getLocInfo()) {
      default: llvm_unreachable("Unknown loc info!");
      case CCValAssign::Full: break;
      case CCValAssign::SExt:
        Arg = DAG.getNode(ISD::SIGN_EXTEND, dl, VA.getLocVT(), Arg);
        break;
      case CCValAssign::ZExt:
        Arg = DAG.getNode(ISD::ZERO_EXTEND, dl, VA.getLocVT(), Arg);
        break;
      case CCValAssign::AExt:
        Arg = DAG.getNode(ISD::ANY_EXTEND, dl, VA.getLocVT(), Arg);
        break;
    }

    // Arguments that can be passed on register must be kept at RegsToPass
    // vector
    if (VA.isRegLoc()) {
      RegsToPass.push_back(std::make_pair(VA.getLocReg(), Arg));
    } else {
      assert(VA.isMemLoc());

      if (StackPtr.getNode() == 0)
        StackPtr = DAG.getCopyFromReg(Chain, dl, MSP430::SPW, getPointerTy());

      SDValue PtrOff = DAG.getNode(ISD::ADD, dl, getPointerTy(),
                                   StackPtr,
                                   DAG.getIntPtrConstant(VA.getLocMemOffset()));


      MemOpChains.push_back(DAG.getStore(Chain, dl, Arg, PtrOff,
                                         PseudoSourceValue::getStack(),
                                         VA.getLocMemOffset()));
    }
  }

  // Transform all store nodes into one single node because all store nodes are
  // independent of each other.
  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, dl, EVT::Other,
                        &MemOpChains[0], MemOpChains.size());

  // Build a sequence of copy-to-reg nodes chained together with token chain and
  // flag operands which copy the outgoing args into registers.  The InFlag in
  // necessary since all emited instructions must be stuck together.
  SDValue InFlag;
  for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i) {
    Chain = DAG.getCopyToReg(Chain, dl, RegsToPass[i].first,
                             RegsToPass[i].second, InFlag);
    InFlag = Chain.getValue(1);
  }

  // If the callee is a GlobalAddress node (quite common, every direct call is)
  // turn it into a TargetGlobalAddress node so that legalize doesn't hack it.
  // Likewise ExternalSymbol -> TargetExternalSymbol.
  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), EVT::i16);
  else if (ExternalSymbolSDNode *E = dyn_cast<ExternalSymbolSDNode>(Callee))
    Callee = DAG.getTargetExternalSymbol(E->getSymbol(), EVT::i16);

  // Returns a chain & a flag for retval copy to use.
  SDVTList NodeTys = DAG.getVTList(EVT::Other, EVT::Flag);
  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);

  // Add argument registers to the end of the list so that they are
  // known live into the call.
  for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i)
    Ops.push_back(DAG.getRegister(RegsToPass[i].first,
                                  RegsToPass[i].second.getValueType()));

  if (InFlag.getNode())
    Ops.push_back(InFlag);

  Chain = DAG.getNode(MSP430ISD::CALL, dl, NodeTys, &Ops[0], Ops.size());
  InFlag = Chain.getValue(1);

  // Create the CALLSEQ_END node.
  Chain = DAG.getCALLSEQ_END(Chain,
                             DAG.getConstant(NumBytes, getPointerTy(), true),
                             DAG.getConstant(0, getPointerTy(), true),
                             InFlag);
  InFlag = Chain.getValue(1);

  // Handle result values, copying them out of physregs into vregs that we
  // return.
  return LowerCallResult(Chain, InFlag, CallConv, isVarArg, Ins, dl,
                         DAG, InVals);
}

/// LowerCallResult - Lower the result values of a call into the
/// appropriate copies out of appropriate physical registers.
///
SDValue
MSP430TargetLowering::LowerCallResult(SDValue Chain, SDValue InFlag,
                                      unsigned CallConv, bool isVarArg,
                                      const SmallVectorImpl<ISD::InputArg> &Ins,
                                      DebugLoc dl, SelectionDAG &DAG,
                                      SmallVectorImpl<SDValue> &InVals) {

  // Assign locations to each value returned by this call.
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, isVarArg, getTargetMachine(),
                 RVLocs, *DAG.getContext());

  CCInfo.AnalyzeCallResult(Ins, RetCC_MSP430);

  // Copy all of the result registers out of their specified physreg.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    Chain = DAG.getCopyFromReg(Chain, dl, RVLocs[i].getLocReg(),
                               RVLocs[i].getValVT(), InFlag).getValue(1);
    InFlag = Chain.getValue(2);
    InVals.push_back(Chain.getValue(0));
  }

  return Chain;
}

SDValue MSP430TargetLowering::LowerShifts(SDValue Op,
                                          SelectionDAG &DAG) {
  unsigned Opc = Op.getOpcode();
  SDNode* N = Op.getNode();
  EVT VT = Op.getValueType();
  DebugLoc dl = N->getDebugLoc();

  // We currently only lower shifts of constant argument.
  if (!isa<ConstantSDNode>(N->getOperand(1)))
    return SDValue();

  uint64_t ShiftAmount = cast<ConstantSDNode>(N->getOperand(1))->getZExtValue();

  // Expand the stuff into sequence of shifts.
  // FIXME: for some shift amounts this might be done better!
  // E.g.: foo >> (8 + N) => sxt(swpb(foo)) >> N
  SDValue Victim = N->getOperand(0);

  if (Opc == ISD::SRL && ShiftAmount) {
    // Emit a special goodness here:
    // srl A, 1 => clrc; rrc A
    Victim = DAG.getNode(MSP430ISD::RRC, dl, VT, Victim);
    ShiftAmount -= 1;
  }

  while (ShiftAmount--)
    Victim = DAG.getNode((Opc == ISD::SHL ? MSP430ISD::RLA : MSP430ISD::RRA),
                         dl, VT, Victim);

  return Victim;
}

SDValue MSP430TargetLowering::LowerGlobalAddress(SDValue Op, SelectionDAG &DAG) {
  const GlobalValue *GV = cast<GlobalAddressSDNode>(Op)->getGlobal();
  int64_t Offset = cast<GlobalAddressSDNode>(Op)->getOffset();

  // Create the TargetGlobalAddress node, folding in the constant offset.
  SDValue Result = DAG.getTargetGlobalAddress(GV, getPointerTy(), Offset);
  return DAG.getNode(MSP430ISD::Wrapper, Op.getDebugLoc(),
                     getPointerTy(), Result);
}

SDValue MSP430TargetLowering::LowerExternalSymbol(SDValue Op,
                                                  SelectionDAG &DAG) {
  DebugLoc dl = Op.getDebugLoc();
  const char *Sym = cast<ExternalSymbolSDNode>(Op)->getSymbol();
  SDValue Result = DAG.getTargetExternalSymbol(Sym, getPointerTy());

  return DAG.getNode(MSP430ISD::Wrapper, dl, getPointerTy(), Result);;
}

static SDValue EmitCMP(SDValue &LHS, SDValue &RHS, unsigned &TargetCC,
                       ISD::CondCode CC,
                       DebugLoc dl, SelectionDAG &DAG) {
  // FIXME: Handle bittests someday
  assert(!LHS.getValueType().isFloatingPoint() && "We don't handle FP yet");

  // FIXME: Handle jump negative someday
  TargetCC = MSP430::COND_INVALID;
  switch (CC) {
  default: llvm_unreachable("Invalid integer condition!");
  case ISD::SETEQ:
    TargetCC = MSP430::COND_E;  // aka COND_Z
    break;
  case ISD::SETNE:
    TargetCC = MSP430::COND_NE; // aka COND_NZ
    break;
  case ISD::SETULE:
    std::swap(LHS, RHS);        // FALLTHROUGH
  case ISD::SETUGE:
    TargetCC = MSP430::COND_HS; // aka COND_C
    break;
  case ISD::SETUGT:
    std::swap(LHS, RHS);        // FALLTHROUGH
  case ISD::SETULT:
    TargetCC = MSP430::COND_LO; // aka COND_NC
    break;
  case ISD::SETLE:
    std::swap(LHS, RHS);        // FALLTHROUGH
  case ISD::SETGE:
    TargetCC = MSP430::COND_GE;
    break;
  case ISD::SETGT:
    std::swap(LHS, RHS);        // FALLTHROUGH
  case ISD::SETLT:
    TargetCC = MSP430::COND_L;
    break;
  }

  return DAG.getNode(MSP430ISD::CMP, dl, EVT::Flag, LHS, RHS);
}


SDValue MSP430TargetLowering::LowerBR_CC(SDValue Op, SelectionDAG &DAG) {
  SDValue Chain = Op.getOperand(0);
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(1))->get();
  SDValue LHS   = Op.getOperand(2);
  SDValue RHS   = Op.getOperand(3);
  SDValue Dest  = Op.getOperand(4);
  DebugLoc dl   = Op.getDebugLoc();

  unsigned TargetCC = MSP430::COND_INVALID;
  SDValue Flag = EmitCMP(LHS, RHS, TargetCC, CC, dl, DAG);

  return DAG.getNode(MSP430ISD::BR_CC, dl, Op.getValueType(),
                     Chain,
                     Dest, DAG.getConstant(TargetCC, EVT::i8),
                     Flag);
}

SDValue MSP430TargetLowering::LowerSELECT_CC(SDValue Op, SelectionDAG &DAG) {
  SDValue LHS    = Op.getOperand(0);
  SDValue RHS    = Op.getOperand(1);
  SDValue TrueV  = Op.getOperand(2);
  SDValue FalseV = Op.getOperand(3);
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(4))->get();
  DebugLoc dl    = Op.getDebugLoc();

  unsigned TargetCC = MSP430::COND_INVALID;
  SDValue Flag = EmitCMP(LHS, RHS, TargetCC, CC, dl, DAG);

  SDVTList VTs = DAG.getVTList(Op.getValueType(), EVT::Flag);
  SmallVector<SDValue, 4> Ops;
  Ops.push_back(TrueV);
  Ops.push_back(FalseV);
  Ops.push_back(DAG.getConstant(TargetCC, EVT::i8));
  Ops.push_back(Flag);

  return DAG.getNode(MSP430ISD::SELECT_CC, dl, VTs, &Ops[0], Ops.size());
}

SDValue MSP430TargetLowering::LowerSIGN_EXTEND(SDValue Op,
                                               SelectionDAG &DAG) {
  SDValue Val = Op.getOperand(0);
  EVT VT      = Op.getValueType();
  DebugLoc dl = Op.getDebugLoc();

  assert(VT == EVT::i16 && "Only support i16 for now!");

  return DAG.getNode(ISD::SIGN_EXTEND_INREG, dl, VT,
                     DAG.getNode(ISD::ANY_EXTEND, dl, VT, Val),
                     DAG.getValueType(Val.getValueType()));
}

const char *MSP430TargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  default: return NULL;
  case MSP430ISD::RET_FLAG:           return "MSP430ISD::RET_FLAG";
  case MSP430ISD::RRA:                return "MSP430ISD::RRA";
  case MSP430ISD::RLA:                return "MSP430ISD::RLA";
  case MSP430ISD::RRC:                return "MSP430ISD::RRC";
  case MSP430ISD::CALL:               return "MSP430ISD::CALL";
  case MSP430ISD::Wrapper:            return "MSP430ISD::Wrapper";
  case MSP430ISD::BR_CC:              return "MSP430ISD::BR_CC";
  case MSP430ISD::CMP:                return "MSP430ISD::CMP";
  case MSP430ISD::SELECT_CC:          return "MSP430ISD::SELECT_CC";
  }
}

//===----------------------------------------------------------------------===//
//  Other Lowering Code
//===----------------------------------------------------------------------===//

MachineBasicBlock*
MSP430TargetLowering::EmitInstrWithCustomInserter(MachineInstr *MI,
                                                  MachineBasicBlock *BB) const {
  const TargetInstrInfo &TII = *getTargetMachine().getInstrInfo();
  DebugLoc dl = MI->getDebugLoc();
  assert((MI->getOpcode() == MSP430::Select16 ||
          MI->getOpcode() == MSP430::Select8) &&
         "Unexpected instr type to insert");

  // To "insert" a SELECT instruction, we actually have to insert the diamond
  // control-flow pattern.  The incoming instruction knows the destination vreg
  // to set, the condition code register to branch on, the true/false values to
  // select between, and a branch opcode to use.
  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator I = BB;
  ++I;

  //  thisMBB:
  //  ...
  //   TrueVal = ...
  //   cmpTY ccX, r1, r2
  //   jCC copy1MBB
  //   fallthrough --> copy0MBB
  MachineBasicBlock *thisMBB = BB;
  MachineFunction *F = BB->getParent();
  MachineBasicBlock *copy0MBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *copy1MBB = F->CreateMachineBasicBlock(LLVM_BB);
  BuildMI(BB, dl, TII.get(MSP430::JCC))
    .addMBB(copy1MBB)
    .addImm(MI->getOperand(3).getImm());
  F->insert(I, copy0MBB);
  F->insert(I, copy1MBB);
  // Update machine-CFG edges by transferring all successors of the current
  // block to the new block which will contain the Phi node for the select.
  copy1MBB->transferSuccessors(BB);
  // Next, add the true and fallthrough blocks as its successors.
  BB->addSuccessor(copy0MBB);
  BB->addSuccessor(copy1MBB);

  //  copy0MBB:
  //   %FalseValue = ...
  //   # fallthrough to copy1MBB
  BB = copy0MBB;

  // Update machine-CFG edges
  BB->addSuccessor(copy1MBB);

  //  copy1MBB:
  //   %Result = phi [ %FalseValue, copy0MBB ], [ %TrueValue, thisMBB ]
  //  ...
  BB = copy1MBB;
  BuildMI(BB, dl, TII.get(MSP430::PHI),
          MI->getOperand(0).getReg())
    .addReg(MI->getOperand(2).getReg()).addMBB(copy0MBB)
    .addReg(MI->getOperand(1).getReg()).addMBB(thisMBB);

  F->DeleteMachineInstr(MI);   // The pseudo instruction is gone now.
  return BB;
}
