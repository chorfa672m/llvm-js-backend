//===-- AlphaISelLowering.cpp - Alpha DAG Lowering Implementation ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Andrew Lenharth and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the AlphaISelLowering class.
//
//===----------------------------------------------------------------------===//

#include "AlphaISelLowering.h"
#include "AlphaTargetMachine.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SSARegMap.h"
#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/Module.h"
#include "llvm/Support/CommandLine.h"
#include <iostream>

using namespace llvm;

namespace llvm {
  cl::opt<bool> EnableAlphaLSMark("enable-alpha-lsmark",
    cl::desc("Emit symbols to correlate Mem ops to LLVM Values"),
    cl::Hidden);
}

/// AddLiveIn - This helper function adds the specified physical register to the
/// MachineFunction as a live in value.  It also creates a corresponding virtual
/// register for it.
static unsigned AddLiveIn(MachineFunction &MF, unsigned PReg,
                          TargetRegisterClass *RC) {
  assert(RC->contains(PReg) && "Not the correct regclass!");
  unsigned VReg = MF.getSSARegMap()->createVirtualRegister(RC);
  MF.addLiveIn(PReg, VReg);
  return VReg;
}

AlphaTargetLowering::AlphaTargetLowering(TargetMachine &TM) : TargetLowering(TM) {
  // Set up the TargetLowering object.
  //I am having problems with shr n ubyte 1
  setShiftAmountType(MVT::i64);
  setSetCCResultType(MVT::i64);
  setSetCCResultContents(ZeroOrOneSetCCResult);
  
  addRegisterClass(MVT::i64, Alpha::GPRCRegisterClass);
  addRegisterClass(MVT::f64, Alpha::F8RCRegisterClass);
  addRegisterClass(MVT::f32, Alpha::F4RCRegisterClass);
  
  setOperationAction(ISD::BRCONDTWOWAY, MVT::Other, Expand);
  setOperationAction(ISD::BRTWOWAY_CC,  MVT::Other, Expand);
  
  setOperationAction(ISD::EXTLOAD, MVT::i1,  Promote);
  setOperationAction(ISD::EXTLOAD, MVT::f32, Expand);
  
  setOperationAction(ISD::ZEXTLOAD, MVT::i1,  Promote);
  setOperationAction(ISD::ZEXTLOAD, MVT::i32, Expand);
  
  setOperationAction(ISD::SEXTLOAD, MVT::i1,  Promote);
  setOperationAction(ISD::SEXTLOAD, MVT::i8,  Expand);
  setOperationAction(ISD::SEXTLOAD, MVT::i16, Expand);
  
  setOperationAction(ISD::TRUNCSTORE, MVT::i1, Promote);

  if (EnableAlphaLSMark) {
    setOperationAction(ISD::LOAD, MVT::i64, Custom);
    setOperationAction(ISD::LOAD, MVT::f64, Custom);
    setOperationAction(ISD::LOAD, MVT::f32, Custom);

    setOperationAction(ISD::ZEXTLOAD, MVT::i8,  Custom);
    setOperationAction(ISD::ZEXTLOAD, MVT::i16, Custom);
    setOperationAction(ISD::SEXTLOAD, MVT::i32, Custom);

    setOperationAction(ISD::EXTLOAD, MVT::i8,  Custom);
    setOperationAction(ISD::EXTLOAD, MVT::i16, Custom);
    setOperationAction(ISD::EXTLOAD, MVT::i32, Custom);
  }

  setOperationAction(ISD::FREM, MVT::f32, Expand);
  setOperationAction(ISD::FREM, MVT::f64, Expand);
  
  setOperationAction(ISD::UINT_TO_FP, MVT::i64, Expand);
  setOperationAction(ISD::SINT_TO_FP, MVT::i64, Custom);
  setOperationAction(ISD::FP_TO_UINT, MVT::i64, Expand);
  setOperationAction(ISD::FP_TO_SINT, MVT::i64, Custom);

  if (!TM.getSubtarget<AlphaSubtarget>().hasCT()) {
    setOperationAction(ISD::CTPOP    , MVT::i64  , Expand);
    setOperationAction(ISD::CTTZ     , MVT::i64  , Expand);
    setOperationAction(ISD::CTLZ     , MVT::i64  , Expand);
  }
  setOperationAction(ISD::BSWAP    , MVT::i64, Expand);
  setOperationAction(ISD::ROTL     , MVT::i64, Expand);
  setOperationAction(ISD::ROTR     , MVT::i64, Expand);
  
  setOperationAction(ISD::SREM     , MVT::i64, Custom);
  setOperationAction(ISD::UREM     , MVT::i64, Custom);
  setOperationAction(ISD::SDIV     , MVT::i64, Custom);
  setOperationAction(ISD::UDIV     , MVT::i64, Custom);
  
  setOperationAction(ISD::MEMMOVE  , MVT::Other, Expand);
  setOperationAction(ISD::MEMSET   , MVT::Other, Expand);
  setOperationAction(ISD::MEMCPY   , MVT::Other, Expand);
  
  // We don't support sin/cos/sqrt
  setOperationAction(ISD::FSIN , MVT::f64, Expand);
  setOperationAction(ISD::FCOS , MVT::f64, Expand);
  setOperationAction(ISD::FSIN , MVT::f32, Expand);
  setOperationAction(ISD::FCOS , MVT::f32, Expand);

  setOperationAction(ISD::FSQRT, MVT::f64, Expand);
  setOperationAction(ISD::FSQRT, MVT::f32, Expand);

  setOperationAction(ISD::SETCC, MVT::f32, Promote);

  // We don't have line number support yet.
  setOperationAction(ISD::LOCATION, MVT::Other, Expand);
  setOperationAction(ISD::DEBUG_LOC, MVT::Other, Expand);
  setOperationAction(ISD::DEBUG_LABEL, MVT::Other, Expand);

  // Not implemented yet.
  setOperationAction(ISD::STACKSAVE, MVT::Other, Expand); 
  setOperationAction(ISD::STACKRESTORE, MVT::Other, Expand);
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i64, Expand);

  // We want to legalize GlobalAddress and ConstantPool and
  // ExternalSymbols nodes into the appropriate instructions to
  // materialize the address.
  setOperationAction(ISD::GlobalAddress,  MVT::i64, Custom);
  setOperationAction(ISD::ConstantPool,   MVT::i64, Custom);
  setOperationAction(ISD::ExternalSymbol, MVT::i64, Custom);

  setOperationAction(ISD::VASTART, MVT::Other, Custom);
  setOperationAction(ISD::VAEND,   MVT::Other, Expand);
  setOperationAction(ISD::VACOPY,  MVT::Other, Custom);
  setOperationAction(ISD::VAARG,   MVT::Other, Custom);
  setOperationAction(ISD::VAARG,   MVT::i32,   Custom);

  setStackPointerRegisterToSaveRestore(Alpha::R30);

  addLegalFPImmediate(+0.0); //F31
  addLegalFPImmediate(-0.0); //-F31

  computeRegisterProperties();

  useITOF = TM.getSubtarget<AlphaSubtarget>().hasF2I();
}

const char *AlphaTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  default: return 0;
  case AlphaISD::ITOFT_: return "Alpha::ITOFT_";
  case AlphaISD::FTOIT_: return "Alpha::FTOIT_";
  case AlphaISD::CVTQT_: return "Alpha::CVTQT_";
  case AlphaISD::CVTQS_: return "Alpha::CVTQS_";
  case AlphaISD::CVTTQ_: return "Alpha::CVTTQ_";
  case AlphaISD::GPRelHi: return "Alpha::GPRelHi";
  case AlphaISD::GPRelLo: return "Alpha::GPRelLo";
  case AlphaISD::RelLit: return "Alpha::RelLit";
  case AlphaISD::GlobalBaseReg: return "Alpha::GlobalBaseReg";
  case AlphaISD::CALL:   return "Alpha::CALL";
  case AlphaISD::DivCall: return "Alpha::DivCall";
  case AlphaISD::LDQ_: return "Alpha::LDQ_";
  case AlphaISD::LDT_: return "Alpha::LDT_";
  case AlphaISD::LDS_: return "Alpha::LDS_";
  case AlphaISD::LDL_: return "Alpha::LDL_";
  case AlphaISD::LDWU_: return "Alpha::LDWU_";
  case AlphaISD::LDBU_:  return "Alpha::LDBU_";
  case AlphaISD::STQ_: return "Alpha::STQ_";
  case AlphaISD::STT_: return "Alpha::STT_";
  case AlphaISD::STS_: return "Alpha::STS_";
  case AlphaISD::STL_: return "Alpha::STL_";
  case AlphaISD::STW_: return "Alpha::STW_";
  case AlphaISD::STB_:  return "Alpha::STB_";
  }
}

//http://www.cs.arizona.edu/computer.help/policy/DIGITAL_unix/AA-PY8AC-TET1_html/callCH3.html#BLOCK21

//For now, just use variable size stack frame format

//In a standard call, the first six items are passed in registers $16
//- $21 and/or registers $f16 - $f21. (See Section 4.1.2 for details
//of argument-to-register correspondence.) The remaining items are
//collected in a memory argument list that is a naturally aligned
//array of quadwords. In a standard call, this list, if present, must
//be passed at 0(SP).
//7 ... n         0(SP) ... (n-7)*8(SP)

// //#define FP    $15
// //#define RA    $26
// //#define PV    $27
// //#define GP    $29
// //#define SP    $30

std::vector<SDOperand>
AlphaTargetLowering::LowerArguments(Function &F, SelectionDAG &DAG)
{
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  MachineBasicBlock& BB = MF.front();
  std::vector<SDOperand> ArgValues;

  unsigned args_int[] = {
    Alpha::R16, Alpha::R17, Alpha::R18, Alpha::R19, Alpha::R20, Alpha::R21};
  unsigned args_float[] = {
    Alpha::F16, Alpha::F17, Alpha::F18, Alpha::F19, Alpha::F20, Alpha::F21};

  int count = 0;

  GP = AddLiveIn(MF, Alpha::R29, getRegClassFor(MVT::i64));
  RA = AddLiveIn(MF, Alpha::R26, getRegClassFor(MVT::i64));

  for (Function::arg_iterator I = F.arg_begin(), E = F.arg_end(); I != E; ++I)
  {
    SDOperand argt;
    if (count  < 6) {
      unsigned Vreg;
      MVT::ValueType VT = getValueType(I->getType());
      switch (VT) {
      default:
        std::cerr << "Unknown Type " << VT << "\n";
        abort();
      case MVT::f64:
      case MVT::f32:
        args_float[count] = AddLiveIn(MF, args_float[count], getRegClassFor(VT));
        argt = DAG.getCopyFromReg(DAG.getRoot(), args_float[count], VT);
        DAG.setRoot(argt.getValue(1));
        break;
      case MVT::i1:
      case MVT::i8:
      case MVT::i16:
      case MVT::i32:
      case MVT::i64:
        args_int[count] = AddLiveIn(MF, args_int[count], getRegClassFor(MVT::i64));
        argt = DAG.getCopyFromReg(DAG.getRoot(), args_int[count], MVT::i64);
        DAG.setRoot(argt.getValue(1));
        if (VT != MVT::i64) {
          unsigned AssertOp = 
            I->getType()->isSigned() ? ISD::AssertSext : ISD::AssertZext;
          argt = DAG.getNode(AssertOp, MVT::i64, argt, 
                             DAG.getValueType(VT));
          argt = DAG.getNode(ISD::TRUNCATE, VT, argt);
        }
        break;
      }
    } else { //more args
      // Create the frame index object for this incoming parameter...
      int FI = MFI->CreateFixedObject(8, 8 * (count - 6));

      // Create the SelectionDAG nodes corresponding to a load
      //from this parameter
      SDOperand FIN = DAG.getFrameIndex(FI, MVT::i64);
      argt = DAG.getLoad(getValueType(I->getType()),
                         DAG.getEntryNode(), FIN, DAG.getSrcValue(NULL));
    }
    ++count;
    ArgValues.push_back(argt);
  }

  // If the functions takes variable number of arguments, copy all regs to stack
  if (F.isVarArg()) {
    VarArgsOffset = count * 8;
    std::vector<SDOperand> LS;
    for (int i = 0; i < 6; ++i) {
      if (MRegisterInfo::isPhysicalRegister(args_int[i]))
        args_int[i] = AddLiveIn(MF, args_int[i], getRegClassFor(MVT::i64));
      SDOperand argt = DAG.getCopyFromReg(DAG.getRoot(), args_int[i], MVT::i64);
      int FI = MFI->CreateFixedObject(8, -8 * (6 - i));
      if (i == 0) VarArgsBase = FI;
      SDOperand SDFI = DAG.getFrameIndex(FI, MVT::i64);
      LS.push_back(DAG.getNode(ISD::STORE, MVT::Other, DAG.getRoot(), argt,
                               SDFI, DAG.getSrcValue(NULL)));

      if (MRegisterInfo::isPhysicalRegister(args_float[i]))
        args_float[i] = AddLiveIn(MF, args_float[i], getRegClassFor(MVT::f64));
      argt = DAG.getCopyFromReg(DAG.getRoot(), args_float[i], MVT::f64);
      FI = MFI->CreateFixedObject(8, - 8 * (12 - i));
      SDFI = DAG.getFrameIndex(FI, MVT::i64);
      LS.push_back(DAG.getNode(ISD::STORE, MVT::Other, DAG.getRoot(), argt,
                               SDFI, DAG.getSrcValue(NULL)));
    }

    //Set up a token factor with all the stack traffic
    DAG.setRoot(DAG.getNode(ISD::TokenFactor, MVT::Other, LS));
  }

  // Finally, inform the code generator which regs we return values in.
  switch (getValueType(F.getReturnType())) {
  default: assert(0 && "Unknown type!");
  case MVT::isVoid: break;
  case MVT::i1:
  case MVT::i8:
  case MVT::i16:
  case MVT::i32:
  case MVT::i64:
    MF.addLiveOut(Alpha::R0);
    break;
  case MVT::f32:
  case MVT::f64:
    MF.addLiveOut(Alpha::F0);
    break;
  }

  //return the arguments
  return ArgValues;
}

std::pair<SDOperand, SDOperand>
AlphaTargetLowering::LowerCallTo(SDOperand Chain,
                                 const Type *RetTy, bool isVarArg,
                                 unsigned CallingConv, bool isTailCall,
                                 SDOperand Callee, ArgListTy &Args,
                                 SelectionDAG &DAG) {
  int NumBytes = 0;
  if (Args.size() > 6)
    NumBytes = (Args.size() - 6) * 8;

  Chain = DAG.getNode(ISD::CALLSEQ_START, MVT::Other, Chain,
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
      // Promote the integer to 64 bits.  If the input type is signed use a
      // sign extend, otherwise use a zero extend.
      if (Args[i].second->isSigned())
        Args[i].first = DAG.getNode(ISD::SIGN_EXTEND, MVT::i64, Args[i].first);
      else
        Args[i].first = DAG.getNode(ISD::ZERO_EXTEND, MVT::i64, Args[i].first);
      break;
    case MVT::i64:
    case MVT::f64:
    case MVT::f32:
      break;
    }
    args_to_use.push_back(Args[i].first);
  }

  std::vector<MVT::ValueType> RetVals;
  MVT::ValueType RetTyVT = getValueType(RetTy);
  MVT::ValueType ActualRetTyVT = RetTyVT;
  if (RetTyVT >= MVT::i1 && RetTyVT <= MVT::i32)
    ActualRetTyVT = MVT::i64;

  if (RetTyVT != MVT::isVoid)
    RetVals.push_back(ActualRetTyVT);
  RetVals.push_back(MVT::Other);

  std::vector<SDOperand> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);
  Ops.insert(Ops.end(), args_to_use.begin(), args_to_use.end());
  SDOperand TheCall = DAG.getNode(AlphaISD::CALL, RetVals, Ops);
  Chain = TheCall.getValue(RetTyVT != MVT::isVoid);
  Chain = DAG.getNode(ISD::CALLSEQ_END, MVT::Other, Chain,
                      DAG.getConstant(NumBytes, getPointerTy()));
  SDOperand RetVal = TheCall;

  if (RetTyVT != ActualRetTyVT) {
    RetVal = DAG.getNode(RetTy->isSigned() ? ISD::AssertSext : ISD::AssertZext,
                         MVT::i64, RetVal, DAG.getValueType(RetTyVT));
    RetVal = DAG.getNode(ISD::TRUNCATE, RetTyVT, RetVal);
  }

  return std::make_pair(RetVal, Chain);
}

void AlphaTargetLowering::restoreGP(MachineBasicBlock* BB)
{
  BuildMI(BB, Alpha::BIS, 2, Alpha::R29).addReg(GP).addReg(GP);
}
void AlphaTargetLowering::restoreRA(MachineBasicBlock* BB)
{
  BuildMI(BB, Alpha::BIS, 2, Alpha::R26).addReg(RA).addReg(RA);
}



static void getValueInfo(const Value* v, int& type, int& fun, int& offset)
{
  fun = type = offset = 0;
  if (v == NULL) {
    type = 0;
  } else if (const GlobalValue* GV = dyn_cast<GlobalValue>(v)) {
    type = 1;
    const Module* M = GV->getParent();
    for(Module::const_global_iterator ii = M->global_begin(); &*ii != GV; ++ii)
      ++offset;
  } else if (const Argument* Arg = dyn_cast<Argument>(v)) {
    type = 2;
    const Function* F = Arg->getParent();
    const Module* M = F->getParent();
    for(Module::const_iterator ii = M->begin(); &*ii != F; ++ii)
      ++fun;
    for(Function::const_arg_iterator ii = F->arg_begin(); &*ii != Arg; ++ii)
      ++offset;
  } else if (const Instruction* I = dyn_cast<Instruction>(v)) {
    assert(dyn_cast<PointerType>(I->getType()));
    type = 3;
    const BasicBlock* bb = I->getParent();
    const Function* F = bb->getParent();
    const Module* M = F->getParent();
    for(Module::const_iterator ii = M->begin(); &*ii != F; ++ii)
      ++fun;
    for(Function::const_iterator ii = F->begin(); &*ii != bb; ++ii)
      offset += ii->size();
    for(BasicBlock::const_iterator ii = bb->begin(); &*ii != I; ++ii)
      ++offset;
  } else if (const Constant* C = dyn_cast<Constant>(v)) {
    //Don't know how to look these up yet
    type = 0;
  } else {
    assert(0 && "Error in value marking");
  }
  //type = 4: register spilling
  //type = 5: global address loading or constant loading
}

static int getUID()
{
  static int id = 0;
  return ++id;
}

/// LowerOperation - Provide custom lowering hooks for some operations.
///
SDOperand AlphaTargetLowering::LowerOperation(SDOperand Op, SelectionDAG &DAG) {
  switch (Op.getOpcode()) {
  default: assert(0 && "Wasn't expecting to be able to lower this!"); 
  case ISD::SINT_TO_FP: {
    assert(MVT::i64 == Op.getOperand(0).getValueType() && 
           "Unhandled SINT_TO_FP type in custom expander!");
    SDOperand LD;
    bool isDouble = MVT::f64 == Op.getValueType();
    if (useITOF) {
      LD = DAG.getNode(AlphaISD::ITOFT_, MVT::f64, Op.getOperand(0));
    } else {
      int FrameIdx =
        DAG.getMachineFunction().getFrameInfo()->CreateStackObject(8, 8);
      SDOperand FI = DAG.getFrameIndex(FrameIdx, MVT::i64);
      SDOperand ST = DAG.getNode(ISD::STORE, MVT::Other, DAG.getEntryNode(),
                                 Op.getOperand(0), FI, DAG.getSrcValue(0));
      LD = DAG.getLoad(MVT::f64, ST, FI, DAG.getSrcValue(0));
      }
    SDOperand FP = DAG.getNode(isDouble?AlphaISD::CVTQT_:AlphaISD::CVTQS_,
                               isDouble?MVT::f64:MVT::f32, LD);
    return FP;
  }
  case ISD::FP_TO_SINT: {
    bool isDouble = MVT::f64 == Op.getOperand(0).getValueType();
    SDOperand src = Op.getOperand(0);

    if (!isDouble) //Promote
      src = DAG.getNode(ISD::FP_EXTEND, MVT::f64, src);
    
    src = DAG.getNode(AlphaISD::CVTTQ_, MVT::f64, src);

    if (useITOF) {
      return DAG.getNode(AlphaISD::FTOIT_, MVT::i64, src);
    } else {
      int FrameIdx =
        DAG.getMachineFunction().getFrameInfo()->CreateStackObject(8, 8);
      SDOperand FI = DAG.getFrameIndex(FrameIdx, MVT::i64);
      SDOperand ST = DAG.getNode(ISD::STORE, MVT::Other, DAG.getEntryNode(),
                                 src, FI, DAG.getSrcValue(0));
      return DAG.getLoad(MVT::i64, ST, FI, DAG.getSrcValue(0));
      }
  }
  case ISD::ConstantPool: {
    Constant *C = cast<ConstantPoolSDNode>(Op)->get();
    SDOperand CPI = DAG.getTargetConstantPool(C, MVT::i64);
    
    SDOperand Hi = DAG.getNode(AlphaISD::GPRelHi,  MVT::i64, CPI,
			       DAG.getNode(AlphaISD::GlobalBaseReg, MVT::i64));
    SDOperand Lo = DAG.getNode(AlphaISD::GPRelLo, MVT::i64, CPI, Hi);
    return Lo;
  }
  case ISD::GlobalAddress: {
    GlobalAddressSDNode *GSDN = cast<GlobalAddressSDNode>(Op);
    GlobalValue *GV = GSDN->getGlobal();
    SDOperand GA = DAG.getTargetGlobalAddress(GV, MVT::i64, GSDN->getOffset());

    if (!GV->hasWeakLinkage() && !GV->isExternal()) {
      SDOperand Hi = DAG.getNode(AlphaISD::GPRelHi,  MVT::i64, GA,
				 DAG.getNode(AlphaISD::GlobalBaseReg, MVT::i64));
      SDOperand Lo = DAG.getNode(AlphaISD::GPRelLo, MVT::i64, GA, Hi);
      return Lo;
    } else
      return DAG.getNode(AlphaISD::RelLit, MVT::i64, GA, DAG.getNode(AlphaISD::GlobalBaseReg, MVT::i64));
  }
  case ISD::ExternalSymbol: {
    return DAG.getNode(AlphaISD::RelLit, MVT::i64, 
		       DAG.getTargetExternalSymbol(cast<ExternalSymbolSDNode>(Op)->getSymbol(), MVT::i64),
		       DAG.getNode(AlphaISD::GlobalBaseReg, MVT::i64));
  }

  case ISD::SDIV:
  case ISD::UDIV:
  case ISD::UREM:
  case ISD::SREM:
    if (MVT::isInteger(Op.getValueType())) {
      const char* opstr = 0;
      switch(Op.getOpcode()) {
      case ISD::UREM: opstr = "__remqu"; break;
      case ISD::SREM: opstr = "__remq";  break;
      case ISD::UDIV: opstr = "__divqu"; break;
      case ISD::SDIV: opstr = "__divq";  break;
      }
      SDOperand Tmp1 = Op.getOperand(0),
        Tmp2 = Op.getOperand(1),
        Addr = DAG.getExternalSymbol(opstr, MVT::i64);
      return DAG.getNode(AlphaISD::DivCall, MVT::i64, Addr, Tmp1, Tmp2);
    }
    break;

  case ISD::LOAD:
  case ISD::SEXTLOAD:
  case ISD::ZEXTLOAD:
  case ISD::EXTLOAD:
    {
      SDOperand Chain   = Op.getOperand(0);
      SDOperand Address = Op.getOperand(1);

      unsigned Opc;
      unsigned opcode = Op.getOpcode();

      if (opcode == ISD::LOAD)
        switch (Op.Val->getValueType(0)) {
        default: Op.Val->dump(); assert(0 && "Bad load!");
        case MVT::i64: Opc = AlphaISD::LDQ_; break;
        case MVT::f64: Opc = AlphaISD::LDT_; break;
        case MVT::f32: Opc = AlphaISD::LDS_; break;
        }
      else
        switch (cast<VTSDNode>(Op.getOperand(3))->getVT()) {
        default: Op.Val->dump(); assert(0 && "Bad sign extend!");
        case MVT::i32: Opc = AlphaISD::LDL_;
          assert(opcode != ISD::ZEXTLOAD && "Not sext"); break;
        case MVT::i16: Opc = AlphaISD::LDWU_;
          assert(opcode != ISD::SEXTLOAD && "Not zext"); break;
        case MVT::i1: //FIXME: Treat i1 as i8 since there are problems otherwise
        case MVT::i8: Opc = AlphaISD::LDBU_;
          assert(opcode != ISD::SEXTLOAD && "Not zext"); break;
        }

      int i, j, k;
      getValueInfo(dyn_cast<SrcValueSDNode>(Op.getOperand(2))->getValue(), i, j, k);

      SDOperand Zero = DAG.getConstant(0, MVT::i64);
      std::vector<MVT::ValueType> VTS;
      VTS.push_back(Op.Val->getValueType(0));
      VTS.push_back(MVT::Other);
      std::vector<SDOperand> ARGS;
      ARGS.push_back(Chain);
      ARGS.push_back(Zero);
      ARGS.push_back(Address);
      ARGS.push_back(DAG.getConstant(i, MVT::i64));
      ARGS.push_back(DAG.getConstant(j, MVT::i64));
      ARGS.push_back(DAG.getConstant(k, MVT::i64));
      ARGS.push_back(DAG.getConstant(getUID(), MVT::i64));
      return DAG.getNode(Opc, VTS, ARGS);
    }

  case ISD::TRUNCSTORE:
  case ISD::STORE:
    {
      SDOperand Chain   = Op.getOperand(0);
      SDOperand Value = Op.getOperand(1);
      SDOperand Address = Op.getOperand(2);

      unsigned Opc;
      unsigned opcode = Op.getOpcode();

      if (opcode == ISD::STORE) {
        switch(Value.getValueType()) {
        default: assert(0 && "unknown Type in store");
        case MVT::i64: Opc = AlphaISD::STQ_; break;
        case MVT::f64: Opc = AlphaISD::STT_; break;
        case MVT::f32: Opc = AlphaISD::STS_; break;
        }
      } else { //ISD::TRUNCSTORE
        switch(cast<VTSDNode>(Op.getOperand(4))->getVT()) {
        default: assert(0 && "unknown Type in store");
        case MVT::i8: Opc = AlphaISD::STB_; break;
        case MVT::i16: Opc = AlphaISD::STW_; break;
        case MVT::i32: Opc = AlphaISD::STL_; break;
        }
      }

      int i, j, k;
      getValueInfo(cast<SrcValueSDNode>(Op.getOperand(3))->getValue(), i, j, k);

      SDOperand Zero = DAG.getConstant(0, MVT::i64);
      std::vector<MVT::ValueType> VTS;
      VTS.push_back(MVT::Other);
      std::vector<SDOperand> ARGS;
      ARGS.push_back(Chain);
      ARGS.push_back(Value);
      ARGS.push_back(Zero);
      ARGS.push_back(Address);
      ARGS.push_back(DAG.getConstant(i, MVT::i64));
      ARGS.push_back(DAG.getConstant(j, MVT::i64));
      ARGS.push_back(DAG.getConstant(k, MVT::i64));
      ARGS.push_back(DAG.getConstant(getUID(), MVT::i64));
      return DAG.getNode(Opc, VTS, ARGS);
    }
  case ISD::VAARG: {
    SDOperand Chain = Op.getOperand(0);
    SDOperand VAListP = Op.getOperand(1);
    SDOperand VAListS = Op.getOperand(2);
    
    SDOperand Base = DAG.getLoad(MVT::i64, Chain, VAListP, VAListS);
    SDOperand Tmp = DAG.getNode(ISD::ADD, MVT::i64, VAListP,
                                DAG.getConstant(8, MVT::i64));
    SDOperand Offset = DAG.getExtLoad(ISD::SEXTLOAD, MVT::i64, Base.getValue(1),
                                      Tmp, DAG.getSrcValue(0), MVT::i32);
    SDOperand DataPtr = DAG.getNode(ISD::ADD, MVT::i64, Base, Offset);
    if (MVT::isFloatingPoint(Op.getValueType()))
    {
      //if fp && Offset < 6*8, then subtract 6*8 from DataPtr
      SDOperand FPDataPtr = DAG.getNode(ISD::SUB, MVT::i64, DataPtr,
                                        DAG.getConstant(8*6, MVT::i64));
      SDOperand CC = DAG.getSetCC(MVT::i64, Offset,
                                  DAG.getConstant(8*6, MVT::i64), ISD::SETLT);
      DataPtr = DAG.getNode(ISD::SELECT, MVT::i64, CC, FPDataPtr, DataPtr);
    }

    SDOperand NewOffset = DAG.getNode(ISD::ADD, MVT::i64, Offset,
                                      DAG.getConstant(8, MVT::i64));
    SDOperand Update = DAG.getNode(ISD::TRUNCSTORE, MVT::Other,
                                   Offset.getValue(1), NewOffset,
                                   Tmp, DAG.getSrcValue(0),
                                   DAG.getValueType(MVT::i32));
    
    SDOperand Result;
    if (Op.getValueType() == MVT::i32)
      Result = DAG.getExtLoad(ISD::SEXTLOAD, MVT::i64, Update, DataPtr,
                              DAG.getSrcValue(0), MVT::i32);
    else
      Result = DAG.getLoad(Op.getValueType(), Update, DataPtr, 
                           DAG.getSrcValue(0));
    return Result;
  }
  case ISD::VACOPY: {
    SDOperand Chain = Op.getOperand(0);
    SDOperand DestP = Op.getOperand(1);
    SDOperand SrcP = Op.getOperand(2);
    SDOperand DestS = Op.getOperand(3);
    SDOperand SrcS = Op.getOperand(4);
    
    SDOperand Val = DAG.getLoad(getPointerTy(), Chain, SrcP, SrcS);
    SDOperand Result = DAG.getNode(ISD::STORE, MVT::Other, Val.getValue(1), Val,
                                   DestP, DestS);
    SDOperand NP = DAG.getNode(ISD::ADD, MVT::i64, SrcP, 
                               DAG.getConstant(8, MVT::i64));
    Val = DAG.getExtLoad(ISD::SEXTLOAD, MVT::i64, Result, NP,
                         DAG.getSrcValue(0), MVT::i32);
    SDOperand NPD = DAG.getNode(ISD::ADD, MVT::i64, DestP,
                                DAG.getConstant(8, MVT::i64));
    return DAG.getNode(ISD::TRUNCSTORE, MVT::Other, Val.getValue(1),
                       Val, NPD, DAG.getSrcValue(0),DAG.getValueType(MVT::i32));
  }
  case ISD::VASTART: {
    SDOperand Chain = Op.getOperand(0);
    SDOperand VAListP = Op.getOperand(1);
    SDOperand VAListS = Op.getOperand(2);
    
    // vastart stores the address of the VarArgsBase and VarArgsOffset
    SDOperand FR  = DAG.getFrameIndex(VarArgsBase, MVT::i64);
    SDOperand S1  = DAG.getNode(ISD::STORE, MVT::Other, Chain, FR, VAListP,
                                VAListS);
    SDOperand SA2 = DAG.getNode(ISD::ADD, MVT::i64, VAListP,
                                DAG.getConstant(8, MVT::i64));
    return DAG.getNode(ISD::TRUNCSTORE, MVT::Other, S1,
                       DAG.getConstant(VarArgsOffset, MVT::i64), SA2,
                       DAG.getSrcValue(0), DAG.getValueType(MVT::i32));
  }
  }

  return SDOperand();
}

SDOperand AlphaTargetLowering::CustomPromoteOperation(SDOperand Op, 
                                                      SelectionDAG &DAG) {
  assert(Op.getValueType() == MVT::i32 && 
         Op.getOpcode() == ISD::VAARG &&
         "Unknown node to custom promote!");
  
  // The code in LowerOperation already handles i32 vaarg
  return LowerOperation(Op, DAG);
}
