// $Id$
//***************************************************************************
// File:
//	SparcInstrSelection.cpp
// 
// Purpose:
//	
// History:
//	7/02/01	 -  Vikram Adve  -  Created
//***************************************************************************


//************************** System Include Files **************************/

#include <assert.h>

//*************************** User Include Files ***************************/

#include "llvm/Type.h"
#include "llvm/DerivedTypes.h"
#include "llvm/SymbolTable.h"
#include "llvm/Value.h"
#include "llvm/Instruction.h"
#include "llvm/InstrTypes.h"
#include "llvm/iTerminators.h"
#include "llvm/iMemory.h"
#include "llvm/iOther.h"
#include "llvm/BasicBlock.h"
#include "llvm/Method.h"
#include "llvm/ConstPoolVals.h"
#include "llvm/LLC/CompileContext.h"
#include "llvm/Codegen/Sparc.h"
#include "llvm/Codegen/MachineInstr.h"
#include "llvm/Codegen/InstrForest.h"
#include "llvm/Codegen/InstrSelection.h"


//******************** Internal Data Declarations ************************/

// to be used later
struct BranchPattern {
  bool	        flipCondition; // should the sense of the test be reversed
  BasicBlock*	targetBB;      // which basic block to branch to
  MachineInstr* extraBranch;   // if neither branch is fall-through, then this
                               // BA must be inserted after the cond'l one
};

//************************* Forward Declarations ***************************/


static MachineOpCode	ChooseBprInstruction	(const InstructionNode* instrNode);

static MachineOpCode	ChooseBccInstruction	(const InstructionNode* instrNode,
						 bool& isFPBranch);

static MachineOpCode	ChooseBpccInstruction	(const InstructionNode* instrNode,
						 const BinaryOperator* setCCInstr);

static MachineOpCode	ChooseBfpccInstruction	(const InstructionNode* instrNode,
						 const BinaryOperator* setCCInstr);

static MachineOpCode	ChooseConvertToFloatInstr(const InstructionNode* instrNode,
						  const Type* opType);

static MachineOpCode	ChooseConvertToIntInstr	(const InstructionNode* instrNode,
						 const Type* opType);

static MachineOpCode	ChooseAddInstruction	(const InstructionNode* instrNode);

static MachineOpCode	ChooseSubInstruction	(const InstructionNode* instrNode);

static MachineOpCode	ChooseFcmpInstruction	(const InstructionNode* instrNode);

static MachineOpCode	ChooseMulInstruction	(const InstructionNode* instrNode,
						 bool checkCasts);

static MachineOpCode	ChooseDivInstruction	(const InstructionNode* instrNode);

static MachineOpCode	ChooseLoadInstruction	(const Type* resultType);

static MachineOpCode	ChooseStoreInstruction	(const Type* valueType);

static void		SetOperandsForMemInstr	(MachineInstr* minstr,
					 const InstructionNode* vmInstrNode,
					 const TargetMachine& targetMachine);

static void		SetMemOperands_Internal	(MachineInstr* minstr,
					 const InstructionNode* vmInstrNode,
					 Value* ptrVal,
					 Value* arrayOffsetVal,
					 const vector<ConstPoolVal*>& idxVec,
					 const TargetMachine& targetMachine);

static unsigned		FixConstantOperands(const InstructionNode* vmInstrNode,
					 MachineInstr** mvec,
					 unsigned numInstr,
					 TargetMachine& targetMachine);

static unsigned		InsertLoadConstInstructions(unsigned loadConstFlags,
				         const InstructionNode* vmInstrNode,
					 MachineInstr** mvec,
					 unsigned numInstr);

static MachineInstr*	MakeOneLoadConstInstr(Instruction* vmInstr,
					      Value* val);


//******************* Externally Visible Functions *************************/


//------------------------------------------------------------------------ 
// External Function: ThisIsAChainRule
//
// Purpose:
//   Check if a given BURG rule is a chain rule.
//------------------------------------------------------------------------ 

extern bool
ThisIsAChainRule(int eruleno)
{
  switch(eruleno)
    {
    case 111:	// stmt:  reg
    case 112:	// stmt:  boolconst
    case 113:	// stmt:  bool
    case 121:
    case 122:
    case 123:
    case 124:
    case 125:
    case 126:
    case 127:
    case 128:
    case 129:
    case 130:
    case 131:
    case 132:
    case 153: return true; break;
      
    default: return false; break;
    }
}

//------------------------------------------------------------------------ 
// External Function: GetInstructionsByRule
//
// Purpose:
//   Choose machine instructions for the SPARC according to the
//   patterns chosen by the BURG-generated parser.
//------------------------------------------------------------------------ 

unsigned
GetInstructionsByRule(InstructionNode* subtreeRoot,
		      int ruleForNode,
		      short* nts,
		      CompileContext& ccontext,
		      MachineInstr** mvec)
{
  int numInstr = 1;			// initialize for common case
  bool checkCast = false;		// initialize here to use fall-through
  Value *leftVal, *rightVal;
  const Type* opType;
  int nextRule;
  BranchPattern brPattern;
  
  mvec[0] = mvec[1] = mvec[2] = mvec[3] = NULL;	// just for safety
  
  switch(ruleForNode) {
  case 1:	// stmt:   Ret
  case 2:	// stmt:   RetValue(reg)
		// NOTE: Prepass of register allocation is responsible
		//	 for moving return value to appropriate register.
		// Mark the return-address register as a hidden virtual reg.
    {		
    Instruction* returnReg = new TmpInstruction(Instruction::UserOp1,
					subtreeRoot->getInstruction(), NULL);
    subtreeRoot->getInstruction()->getMachineInstrVec().addTempValue(returnReg);
    
    mvec[0] = new MachineInstr(RETURN);
    mvec[0]->SetMachineOperand(0, MachineOperand::MO_Register, returnReg);
    mvec[0]->SetMachineOperand(1, MachineOperand::MO_SignExtendedImmed,
			          (int64_t) 0);

    mvec[numInstr++] = new MachineInstr(NOP); // delay slot
    break;
    }  
    
  case 3:	// stmt:   Store(reg,reg)
  case 4:	// stmt:   Store(reg,ptrreg)
    mvec[0] = new MachineInstr(ChooseStoreInstruction(subtreeRoot->leftChild()->getValue()->getType()));
    SetOperandsForMemInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 5:	// stmt:   BrUncond
    mvec[0] = new MachineInstr(BA);
    mvec[0]->SetMachineOperand(0, MachineOperand::MO_PCRelativeDisp,
	      ((BranchInst*) subtreeRoot->getInstruction())->getSuccessor(0));
    
    mvec[numInstr++] = new MachineInstr(NOP); // delay slot
    break;
    
  case 6:	// stmt:   BrCond(boolconst)
    // boolconst => boolean was computed with `%b = setCC type reg1 constant'
    // If the constant is ZERO, we can use the branch-on-integer-register
    // instructions and avoid the SUBcc instruction entirely.
    // Otherwise this is just the same as case 5, so just fall through.
    {
    InstrTreeNode* constNode = subtreeRoot->leftChild()->rightChild();
    assert(constNode && constNode->getNodeType() ==InstrTreeNode::NTConstNode);
    ConstPoolVal* constVal = (ConstPoolVal*) constNode->getValue();
    
    if (constVal->getType()->isIntegral()
	&& ((constVal->getType()->isSigned()
	     && ((ConstPoolSInt*) constVal)->getValue()==0)
	    || (constVal->getType()->isUnsigned()
		&& ((ConstPoolUInt*) constVal)->getValue()== 0)))
      {
	// Whew!  Ok, that was zero after all...
	// Use the left child of the setCC instruction as the first argument!
	mvec[0] = new MachineInstr(ChooseBprInstruction(subtreeRoot));
	mvec[0]->SetMachineOperand(0, MachineOperand::MO_Register,
		      subtreeRoot->leftChild()->leftChild()->getValue());
	mvec[0]->SetMachineOperand(1, MachineOperand::MO_PCRelativeDisp,
	      ((BranchInst*) subtreeRoot->getInstruction())->getSuccessor(0));

	mvec[numInstr++] = new MachineInstr(NOP); // delay slot
	
	mvec[numInstr++] = new MachineInstr(BA); // false branch
	mvec[numInstr-1]->SetMachineOperand(0, MachineOperand::MO_PCRelativeDisp, ((BranchInst*) subtreeRoot->getInstruction())->getSuccessor(1));
	break;
      }
    // ELSE FALL THROUGH
    }
    
  case 7:	// stmt:   BrCond(bool)
    // bool => boolean was computed with `%b = setcc type reg1 reg2'
    // Need to check whether the type was a FP, signed int or unsigned int,
    // nad check the branching condition in order to choose the branch to use.
    // Also, for FP branches, an extra operand specifies which FCCn reg to use.
    // 
    {
    bool isFPBranch;
    mvec[0] = new MachineInstr(ChooseBccInstruction(subtreeRoot, isFPBranch));
    
    int opNum = 0;
    if (isFPBranch)
	mvec[0]->SetMachineOperand(opNum++, MachineOperand::MO_CCRegister,
				  subtreeRoot->leftChild()->getValue());
    
    mvec[0]->SetMachineOperand(opNum, MachineOperand::MO_PCRelativeDisp,
	      ((BranchInst*) subtreeRoot->getInstruction())->getSuccessor(0));
    
    mvec[numInstr++] = new MachineInstr(NOP); // delay slot
    
    mvec[numInstr++] = new MachineInstr(BA); // false branch
    mvec[numInstr-1]->SetMachineOperand(0, MachineOperand::MO_PCRelativeDisp, ((BranchInst*) subtreeRoot->getInstruction())->getSuccessor(1));
    break;
    }
  
  case 8:	// stmt:   BrCond(boolreg)
    // bool => boolean is stored in an existing register.
    // Just use the branch-on-integer-register instruction!
    // 
    mvec[0] = new MachineInstr(BRNZ);
    mvec[0]->SetMachineOperand(0, MachineOperand::MO_Register,
			         subtreeRoot->leftChild()->getValue());
    mvec[0]->SetMachineOperand(1, MachineOperand::MO_PCRelativeDisp,
	      ((BranchInst*) subtreeRoot->getInstruction())->getSuccessor(0));
    mvec[numInstr++] = new MachineInstr(NOP); // delay slot
    break;
    
  case 9:	// stmt:   Switch(reg)
    assert(0 && "*** SWITCH instruction is not implemented yet.");
    numInstr = 0;
    break;
    
  case 10:	// reg:   VRegList(reg, reg)
    assert(0 && "VRegList should never be the topmost non-chain rule");
    break;
    
  case 21:	// reg:   Not(reg):	Implemented as reg = reg XOR-NOT 0
    mvec[0] = new MachineInstr(XNOR);
    mvec[0]->SetMachineOperand(0, MachineOperand::MO_Register,
			          subtreeRoot->leftChild()->getValue());
    mvec[0]->SetMachineOperand(1, /*regNum %g0*/ (unsigned int) 0);
    mvec[0]->SetMachineOperand(2, MachineOperand::MO_Register,
				 subtreeRoot->getValue());
    break;
    
  case 22:	// reg:   ToBoolTy(reg):
    opType = subtreeRoot->leftChild()->getValue()->getType();
    assert(opType->isIntegral() || opType == Type::BoolTy);
    numInstr = 0;
    break;
    
  case 23:	// reg:   ToUByteTy(reg)
  case 25:	// reg:   ToUShortTy(reg)
  case 27:	// reg:   ToUIntTy(reg)
  case 29:	// reg:   ToULongTy(reg)
    opType = subtreeRoot->leftChild()->getValue()->getType();
    assert(opType->isIntegral() || opType == Type::BoolTy);
    numInstr = 0;
    break;
    
  case 24:	// reg:   ToSByteTy(reg)
  case 26:	// reg:   ToShortTy(reg)
  case 28:	// reg:   ToIntTy(reg)
  case 30:	// reg:   ToLongTy(reg)
    opType = subtreeRoot->leftChild()->getValue()->getType();
    if (opType->isIntegral() || opType == Type::BoolTy)
      numInstr = 0;
    else
      {
	mvec[0] =new MachineInstr(ChooseConvertToIntInstr(subtreeRoot,opType));
	Set2OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
      }
    break;
    
  case 31:	// reg:   ToFloatTy(reg):
  case 32:	// reg:   ToDoubleTy(reg):
    
    // If this instruction has a parent (a user) in the tree 
    // and the user is translated as an FsMULd instruction,
    // then the cast is unnecessary.  So check that first.
    // In the future, we'll want to do the same for the FdMULq instruction,
    // so do the check here instead of only for ToFloatTy(reg).
    // 
    if (subtreeRoot->parent() != NULL &&
	((InstructionNode*) subtreeRoot->parent())->getInstruction()->getMachineInstrVec()[0]->getOpCode() == FSMULD)
      {
	numInstr = 0;
      }
    else
      {
	opType = subtreeRoot->leftChild()->getValue()->getType();
	mvec[0] = new MachineInstr(ChooseConvertToFloatInstr(subtreeRoot, opType));
	Set2OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
      }
    break;
    
  case 19:	// reg:   ToArrayTy(reg):
  case 20:	// reg:   ToPointerTy(reg):
    numInstr = 0;
    break;
    
  case 33:	// reg:   Add(reg, reg)
    mvec[0] = new MachineInstr(ChooseAddInstruction(subtreeRoot));
    Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 34:	// reg:   Sub(reg, reg)
    mvec[0] = new MachineInstr(ChooseSubInstruction(subtreeRoot));
    Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 135:	// reg:   Mul(todouble, todouble)
    checkCast = true;
    // FALL THROUGH 
    
  case 35:	// reg:   Mul(reg, reg)
    mvec[0] = new MachineInstr(ChooseMulInstruction(subtreeRoot, checkCast));
    Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 36:	// reg:   Div(reg, reg)
    mvec[0] = new MachineInstr(ChooseDivInstruction(subtreeRoot));
    Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 37:	// reg:   Rem(reg, reg)
    assert(0 && "REM instruction unimplemented for the SPARC.");
    break;
    
  case 38:	// reg:   And(reg, reg)
    mvec[0] = new MachineInstr(AND);
    Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 138:	// reg:   And(reg, not)
    mvec[0] = new MachineInstr(ANDN);
    Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 39:	// reg:   Or(reg, reg)
    mvec[0] = new MachineInstr(ORN);
    Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 139:	// reg:   Or(reg, not)
    mvec[0] = new MachineInstr(ORN);
    Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 40:	// reg:   Xor(reg, reg)
    mvec[0] = new MachineInstr(XOR);
    Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 140:	// reg:   Xor(reg, not)
    mvec[0] = new MachineInstr(XNOR);
    Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 41:	// boolconst:   SetCC(reg, Constant)
    // Check if this is an integer comparison, and
    // there is a parent, and the parent decided to use
    // a branch-on-integer-register instead of branch-on-condition-code.
    // If so, the SUBcc instruction is not required.
    // (However, we must still check for constants to be loaded from
    // the constant pool so that such a load can be associated with
    // this instruction.)
    // 
    // Otherwise this is just the same as case 7, so just fall through.
    // 
    if (subtreeRoot->leftChild()->getValue()->getType()->isIntegral() &&
	subtreeRoot->parent() != NULL)
      {
	InstructionNode* parentNode = (InstructionNode*) subtreeRoot->parent();
	assert(parentNode->getNodeType() == InstrTreeNode::NTInstructionNode);
	const vector<MachineInstr*>&
	  minstrVec = parentNode->getInstruction()->getMachineInstrVec();
	MachineOpCode parentOpCode;
	if (minstrVec.size() == 1 &&
	    (parentOpCode = minstrVec[0]->getOpCode()) >= BRZ &&
	    parentOpCode <= BRGEZ)
	  {
	    numInstr = 0;
	    break;
	  }
      }
    // ELSE FALL THROUGH
    
  case 42:	// bool:   SetCC(reg, reg):
    if (subtreeRoot->leftChild()->getValue()->getType()->isIntegral())
      {
	// integer condition: destination should be %g0
	mvec[0] = new MachineInstr(SUBcc);
	Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget(),
			      /*canDiscardResult*/ true);
      }
    else
      {
	// FP condition: dest should be a FCCn register chosen by reg-alloc
	mvec[0] = new MachineInstr(ChooseFcmpInstruction(subtreeRoot));
	
	leftVal = subtreeRoot->leftChild()->getValue();
	rightVal = subtreeRoot->rightChild()->getValue();
	mvec[0]->SetMachineOperand(0, MachineOperand::MO_CCRegister,
				     subtreeRoot->getValue());
	mvec[0]->SetMachineOperand(1, MachineOperand::MO_Register, leftVal);
	mvec[0]->SetMachineOperand(2, MachineOperand::MO_Register, rightVal);
      }
    break;
    
  case 43:	// boolreg: VReg
    numInstr = 0;
    break;

  case 51:	// reg:   Load(reg)
  case 52:	// reg:   Load(ptrreg)
  case 53:	// reg:   LoadIdx(reg,reg)
  case 54:	// reg:   LoadIdx(ptrreg,reg)
    mvec[0] = new MachineInstr(ChooseLoadInstruction(subtreeRoot->getValue()->getType()));
    SetOperandsForMemInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 55:	// reg:   GetElemPtr(reg)
  case 56:	// reg:   GetElemPtrIdx(reg,reg)
    if (subtreeRoot->parent() != NULL)
      {
	// Check if the parent was an array access.
	// If so, we still need to generate this instruction.
	MemAccessInst* memInst =(MemAccessInst*) subtreeRoot->getInstruction();
	const PointerType* ptrType =
	  (const PointerType*) memInst->getPtrOperand()->getType();
	if (! ptrType->getValueType()->isArrayType())
	  {// we don't need a separate instr
	    numInstr = 0;
	    break;
	  }
      }
    // else in all other cases we need to a separate ADD instruction
    mvec[0] = new MachineInstr(ADD);
    SetOperandsForMemInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 57:	// reg:   Alloca: Implement as 2 instructions:
    		//	sub %sp, tmp -> %sp
    {		//	add %sp, 0   -> result
    Instruction* instr = subtreeRoot->getInstruction();
    const PointerType* instrType = (const PointerType*) instr->getType();
    assert(instrType->isPointerType());
    int tsize = (int) ccontext.getTarget().findOptimalStorageSize(
						    instrType->getValueType());
    if (tsize == 0)
      {
	numInstr = 0;
	break;
      }
    //else go on to create the instructions needed...
    
    // Create a temporary Value to hold the constant type-size
    ConstPoolSInt* valueForTSize = new ConstPoolSInt(Type::IntTy, tsize);
    ConstantPool &cpool = instr->getParent()->getParent()->getConstantPool();
    if (cpool.find(valueForTSize) == 0)
      cpool.insert(valueForTSize);
    
    // Instruction 1: sub %sp, tsize -> %sp
    // tsize is always constant, but it may have to be put into a
    // register if it doesn't fit in the immediate field.
    // 
    mvec[0] = new MachineInstr(SUB);
    mvec[0]->SetMachineOperand(0, /*regNum %sp = o6 = r[14]*/(unsigned int)14);
    mvec[0]->SetMachineOperand(1, MachineOperand::MO_Register, valueForTSize);
    mvec[0]->SetMachineOperand(2, /*regNum %sp = o6 = r[14]*/(unsigned int)14);
    
    // Instruction 2: add %sp, 0 -> result
    numInstr++;
    mvec[1] = new MachineInstr(ADD);
    mvec[1]->SetMachineOperand(0, /*regNum %sp = o6 = r[14]*/(unsigned int)14);
    mvec[1]->SetMachineOperand(1, /*regNum %g0*/ (unsigned int) 0);
    mvec[1]->SetMachineOperand(2, MachineOperand::MO_Register, instr);
    break;
    }
  
  case 58:	// reg:   Alloca(reg): Implement as 3 instructions:
    		//	mul num, typeSz -> tmp
    		//	sub %sp, tmp    -> %sp
    {		//	add %sp, 0      -> result
    Instruction* instr = subtreeRoot->getInstruction();
    const PointerType* instrType = (const PointerType*) instr->getType();
    assert(instrType->isPointerType() &&
	   instrType->getValueType()->isArrayType());
    const Type* eltType =
      ((ArrayType*) instrType->getValueType())->getElementType();
    int tsize = (int) ccontext.getTarget().findOptimalStorageSize(eltType);
    
    if (tsize == 0)
      {
	numInstr = 0;
	break;
      }
    //else go on to create the instructions needed...

    // Create a temporary Value to hold the constant type-size
    ConstPoolSInt* valueForTSize = new ConstPoolSInt(Type::IntTy, tsize);
    ConstantPool &cpool = instr->getParent()->getParent()->getConstantPool();
    if (cpool.find(valueForTSize) == 0)
      cpool.insert(valueForTSize);
    
    // Create a temporary value to hold `tmp'
    Instruction* tmpInstr = new TmpInstruction(Instruction::UserOp1,
					  subtreeRoot->leftChild()->getValue(),
					  NULL /*could insert tsize here*/);
    subtreeRoot->getInstruction()->getMachineInstrVec().addTempValue(tmpInstr);

    // Instruction 1: mul numElements, typeSize -> tmp
    mvec[0] = new MachineInstr(MULX);
    mvec[0]->SetMachineOperand(0, MachineOperand::MO_Register,
				subtreeRoot->leftChild()->getValue());
    mvec[0]->SetMachineOperand(1, MachineOperand::MO_Register, valueForTSize);
    mvec[0]->SetMachineOperand(2, MachineOperand::MO_Register, tmpInstr);

    // Instruction 2: sub %sp, tmp -> %sp
    numInstr++;
    mvec[1] = new MachineInstr(SUB);
    mvec[1]->SetMachineOperand(0, /*regNum %sp = o6 = r[14]*/(unsigned int)14);
    mvec[1]->SetMachineOperand(1, MachineOperand::MO_Register, tmpInstr);
    mvec[1]->SetMachineOperand(2, /*regNum %sp = o6 = r[14]*/(unsigned int)14);
    
    // Instruction 3: add %sp, 0 -> result
    numInstr++;
    mvec[2] = new MachineInstr(ADD);
    mvec[2]->SetMachineOperand(0, /*regNum %sp = o6 = r[14]*/(unsigned int)14);
    mvec[2]->SetMachineOperand(1, /*regNum %g0*/ (unsigned int) 0);
    mvec[2]->SetMachineOperand(2, MachineOperand::MO_Register, instr);
    break;
    }
    
  case 61:	// reg:   Call
		// Generate a call-indirect (i.e., JMPL) for now to expose
		// the potential need for registers.  If an absolute address
		// is available, replace this with a CALL instruction.
		// Mark both the indirection register and the return-address
    {		// register as hidden virtual registers.
      
    Instruction* targetReg = new TmpInstruction(Instruction::UserOp1,
	((CallInst*) subtreeRoot->getInstruction())->getCalledMethod(), NULL);
    Instruction* returnReg = new TmpInstruction(Instruction::UserOp1,
						subtreeRoot->getValue(), NULL);
    subtreeRoot->getInstruction()->getMachineInstrVec().addTempValue(targetReg);
    subtreeRoot->getInstruction()->getMachineInstrVec().addTempValue(returnReg);
    
    mvec[0] = new MachineInstr(JMPL);
    mvec[0]->SetMachineOperand(0, MachineOperand::MO_Register, targetReg);
    mvec[0]->SetMachineOperand(1, MachineOperand::MO_SignExtendedImmed,
			          (int64_t) 0);
    mvec[0]->SetMachineOperand(2, MachineOperand::MO_Register, returnReg);
    
    mvec[numInstr++] = new MachineInstr(NOP); // delay slot
    break;
    }
    
  case 62:	// reg:   Shl(reg, reg)
    opType = subtreeRoot->leftChild()->getValue()->getType();
    assert(opType->isIntegral() || opType == Type::BoolTy); 
    mvec[0] = new MachineInstr((opType == Type::LongTy)? SLLX : SLL);
    Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 63:	// reg:   Shr(reg, reg)
    opType = subtreeRoot->leftChild()->getValue()->getType();
    assert(opType->isIntegral() || opType == Type::BoolTy); 
    mvec[0] = new MachineInstr((opType->isSigned()
				? ((opType == Type::LongTy)? SRAX : SRA)
				: ((opType == Type::LongTy)? SRLX : SRL)));
    Set3OperandsFromInstr(mvec[0], subtreeRoot, ccontext.getTarget());
    break;
    
  case 71:	// reg:     VReg
  case 72:	// reg:     Constant
    numInstr = 0;
    break;

  case 111:	// stmt:  reg
  case 112:	// stmt:  boolconst
  case 113:	// stmt:  bool
  case 121:
  case 122:
  case 123:
  case 124:
  case 125:
  case 126:
  case 127:
  case 128:
  case 129:
  case 130:
  case 131:
  case 132:
  case 153:
    // 
    // These are all chain rules, which have a single nonterminal on the RHS.
    // Get the rule that matches the RHS non-terminal and use that instead.
    // 
    assert(ThisIsAChainRule(ruleForNode));
    assert(nts[0] && ! nts[1]
	   && "A chain rule should have only one RHS non-terminal!");
    nextRule = burm_rule(subtreeRoot->getBasicNode()->state, nts[0]);
    nts = burm_nts[nextRule];
    numInstr = GetInstructionsByRule(subtreeRoot, nextRule, nts,ccontext,mvec);
    break;
    
  default:
    numInstr = 0;
    break;
  }
  
  numInstr =
    FixConstantOperands(subtreeRoot, mvec, numInstr, ccontext.getTarget());
  
  return numInstr;
}


//---------------------------------------------------------------------------
// Private helper routines for SPARC instruction selection.
//---------------------------------------------------------------------------


static MachineOpCode 
ChooseBprInstruction(const InstructionNode* instrNode)
{
  MachineOpCode opCode;
  
  Instruction* setCCInstr =
    ((InstructionNode*) instrNode->leftChild())->getInstruction();
  
  switch(setCCInstr->getOpcode())
    {
    case Instruction::SetEQ: opCode = BRZ;   break;
    case Instruction::SetNE: opCode = BRNZ;  break;
    case Instruction::SetLE: opCode = BRLEZ; break;
    case Instruction::SetGE: opCode = BRGEZ; break;
    case Instruction::SetLT: opCode = BRLZ;  break;
    case Instruction::SetGT: opCode = BRGZ;  break;
    default:
      assert(0 && "Unrecognized VM instruction!");
      opCode = INVALID_OPCODE;
      break; 
    }
  
  return opCode;
}


static MachineOpCode 
ChooseBccInstruction(const InstructionNode* instrNode,
		     bool& isFPBranch)
{
  InstructionNode* setCCNode = (InstructionNode*) instrNode->leftChild();
  BinaryOperator* setCCInstr = (BinaryOperator*) setCCNode->getInstruction();
  const Type* setCCType = setCCInstr->getOperand(0)->getType();
  
  isFPBranch = (setCCType == Type::FloatTy || setCCType == Type::DoubleTy); 
  
  if (isFPBranch) 
    return ChooseBfpccInstruction(instrNode, setCCInstr);
  else
    return ChooseBpccInstruction(instrNode, setCCInstr);
}


static MachineOpCode 
ChooseBpccInstruction(const InstructionNode* instrNode,
		      const BinaryOperator* setCCInstr)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  bool isSigned = setCCInstr->getOperand(0)->getType()->isSigned();
  
  if (isSigned)
    {
      switch(setCCInstr->getOpcode())
	{
	case Instruction::SetEQ: opCode = BE;  break;
	case Instruction::SetNE: opCode = BNE; break;
	case Instruction::SetLE: opCode = BLE; break;
	case Instruction::SetGE: opCode = BGE; break;
	case Instruction::SetLT: opCode = BL;  break;
	case Instruction::SetGT: opCode = BG;  break;
	default:
	  assert(0 && "Unrecognized VM instruction!");
	  break; 
	}
    }
  else
    {
      switch(setCCInstr->getOpcode())
	{
	case Instruction::SetEQ: opCode = BE;   break;
	case Instruction::SetNE: opCode = BNE;  break;
	case Instruction::SetLE: opCode = BLEU; break;
	case Instruction::SetGE: opCode = BCC;  break;
	case Instruction::SetLT: opCode = BCS;  break;
	case Instruction::SetGT: opCode = BGU;  break;
	default:
	  assert(0 && "Unrecognized VM instruction!");
	  break; 
	}
    }
  
  return opCode;
}

static MachineOpCode 
ChooseBfpccInstruction(const InstructionNode* instrNode,
		       const BinaryOperator* setCCInstr)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  switch(setCCInstr->getOpcode())
    {
    case Instruction::SetEQ: opCode = FBE;  break;
    case Instruction::SetNE: opCode = FBNE; break;
    case Instruction::SetLE: opCode = FBLE; break;
    case Instruction::SetGE: opCode = FBGE; break;
    case Instruction::SetLT: opCode = FBL;  break;
    case Instruction::SetGT: opCode = FBG;  break;
    default:
      assert(0 && "Unrecognized VM instruction!");
      break; 
    }
  
  return opCode;
}

static MachineOpCode 
ChooseConvertToFloatInstr(const InstructionNode* instrNode,
			  const Type* opType)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  switch(instrNode->getOpLabel())
    {
    case ToFloatTy: 
      if (opType == Type::SByteTy || opType == Type::ShortTy || opType == Type::IntTy)
	opCode = FITOS;
      else if (opType == Type::LongTy)
	opCode = FXTOS;
      else if (opType == Type::DoubleTy)
	opCode = FDTOS;
      else
	  assert(0 && "Cannot convert this type to FLOAT on SPARC");
      break;
      
    case ToDoubleTy: 
      if (opType == Type::SByteTy || opType == Type::ShortTy || opType == Type::IntTy)
	opCode = FITOD;
      else if (opType == Type::LongTy)
	opCode = FXTOD;
      else if (opType == Type::FloatTy)
	opCode = FSTOD;
      else
	assert(0 && "Cannot convert this type to DOUBLE on SPARC");
      break;
      
    default:
      break;
    }
  
  return opCode;
}

static MachineOpCode 
ChooseConvertToIntInstr(const InstructionNode* instrNode,
			const Type* opType)
{
  MachineOpCode opCode = INVALID_OPCODE;;
  
  int instrType = (int) instrNode->getOpLabel();
  
  if (instrType == ToSByteTy || instrType == ToShortTy || instrType == ToIntTy)
    {
      switch (opType->getPrimitiveID())
	{
        case Type::FloatTyID:   opCode = FSTOI; break;
        case Type::DoubleTyID:  opCode = FDTOI; break;
	default:
	  assert(0 && "Non-numeric non-bool type cannot be converted to Int");
	  break;
	}
    }
  else if (instrType == ToLongTy)
    {
      switch (opType->getPrimitiveID())
	{
        case Type::FloatTyID:   opCode = FSTOX; break;
        case Type::DoubleTyID:  opCode = FDTOX; break;
	default:
	  assert(0 && "Non-numeric non-bool type cannot be converted to Long");
	  break;
	}
    }
  else
      assert(0 && "Should not get here, Mo!");
  
  return opCode;
}


static MachineOpCode 
ChooseAddInstruction(const InstructionNode* instrNode)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  const Type* resultType = instrNode->getInstruction()->getType();
  
  if (resultType->isIntegral() ||
      resultType->isPointerType() ||
      resultType->isMethodType() ||
      resultType->isLabelType())
    {
      opCode = ADD;
    }
  else
    {
      Value* operand = ((InstrTreeNode*) instrNode->leftChild())->getValue();
      switch(operand->getType()->getPrimitiveID())
	{
	case Type::FloatTyID:  opCode = FADDS; break;
	case Type::DoubleTyID: opCode = FADDD; break;
	default: assert(0 && "Invalid type for ADD instruction"); break; 
	}
    }
  
  return opCode;
}

static MachineOpCode 
ChooseSubInstruction(const InstructionNode* instrNode)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  const Type* resultType = instrNode->getInstruction()->getType();
  
  if (resultType->isIntegral() ||
      resultType->isPointerType())
    {
      opCode = SUB;
    }
  else
    {
      Value* operand = ((InstrTreeNode*) instrNode->leftChild())->getValue();
      switch(operand->getType()->getPrimitiveID())
	{
	case Type::FloatTyID:  opCode = FSUBS; break;
	case Type::DoubleTyID: opCode = FSUBD; break;
	default: assert(0 && "Invalid type for SUB instruction"); break; 
	}
    }
  
  return opCode;
}


static MachineOpCode 
ChooseFcmpInstruction(const InstructionNode* instrNode)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  Value* operand = ((InstrTreeNode*) instrNode->leftChild())->getValue();
  switch(operand->getType()->getPrimitiveID())
    {
    case Type::FloatTyID:  opCode = FCMPS; break;
    case Type::DoubleTyID: opCode = FCMPD; break;
    default: assert(0 && "Invalid type for ADD instruction"); break; 
    }
  
  return opCode;
}


static MachineOpCode 
ChooseMulInstruction(const InstructionNode* instrNode,
		     bool checkCasts)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  if (checkCasts)
    {
      // Assume that leftArg and rightArg are both cast instructions.
      //
      InstrTreeNode* leftArg = instrNode->leftChild();
      InstrTreeNode* rightArg = instrNode->rightChild();
      InstrTreeNode* leftArgArg = leftArg->leftChild();
      InstrTreeNode* rightArgArg = rightArg->leftChild();
      assert(leftArg->getValue()->getType() ==rightArg->getValue()->getType());
      
      // If both arguments are floats cast to double, use FsMULd
      if (leftArg->getValue()->getType() == Type::DoubleTy &&
	  leftArgArg->getValue()->getType() == Type::FloatTy &&
	  rightArgArg->getValue()->getType() == Type::FloatTy)
	{
	  return opCode = FSMULD;
	}
      // else fall through and use the regular multiply instructions
    }
  
  const Type* resultType = instrNode->getInstruction()->getType();
  
  if (resultType->isIntegral())
    {
      opCode = MULX;
    }
  else
    {
      switch(instrNode->leftChild()->getValue()->getType()->getPrimitiveID())
	{
	case Type::FloatTyID:  opCode = FMULS; break;
	case Type::DoubleTyID: opCode = FMULD; break;
	default: assert(0 && "Invalid type for MUL instruction"); break; 
	}
    }
  
  return opCode;
}


static MachineOpCode 
ChooseDivInstruction(const InstructionNode* instrNode)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  const Type* resultType = instrNode->getInstruction()->getType();
  
  if (resultType->isIntegral())
    {
      opCode = resultType->isSigned()? SDIVX : UDIVX;
    }
  else
    {
      Value* operand = ((InstrTreeNode*) instrNode->leftChild())->getValue();
      switch(operand->getType()->getPrimitiveID())
	{
	case Type::FloatTyID:  opCode = FDIVS; break;
	case Type::DoubleTyID: opCode = FDIVD; break;
	default: assert(0 && "Invalid type for DIV instruction"); break; 
	}
    }
  
  return opCode;
}


static MachineOpCode 
ChooseLoadInstruction(const Type* resultType)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  switch (resultType->getPrimitiveID())
    {
    case Type::BoolTyID:	opCode = LDUB; break;
    case Type::UByteTyID:	opCode = LDUB; break;
    case Type::SByteTyID:	opCode = LDSB; break;
    case Type::UShortTyID:	opCode = LDUH; break;
    case Type::ShortTyID:	opCode = LDSH; break;
    case Type::UIntTyID:	opCode = LDUW; break;
    case Type::IntTyID:		opCode = LDSW; break;
    case Type::ULongTyID:
    case Type::LongTyID:	opCode = LDX;  break;
    case Type::FloatTyID:	opCode = LD;   break;
    case Type::DoubleTyID:	opCode = LDD;  break;
    default: assert(0 && "Invalid type for Load instruction"); break; 
    }
  
  return opCode;
}


static MachineOpCode 
ChooseStoreInstruction(const Type* valueType)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  switch (valueType->getPrimitiveID())
    {
    case Type::BoolTyID:
    case Type::UByteTyID:
    case Type::SByteTyID:	opCode = STB; break;
    case Type::UShortTyID:
    case Type::ShortTyID:	opCode = STH; break;
    case Type::UIntTyID:
    case Type::IntTyID:		opCode = STW; break;
    case Type::ULongTyID:
    case Type::LongTyID:	opCode = STX;  break;
    case Type::FloatTyID:	opCode = ST;   break;
    case Type::DoubleTyID:	opCode = STD;  break;
    default: assert(0 && "Invalid type for Store instruction"); break; 
    }
  
  return opCode;
}


//------------------------------------------------------------------------ 
// Function SetOperandsForMemInstr
//
// Choose addressing mode for the given load or store instruction.
// Use [reg+reg] if it is an indexed reference, and the index offset is
//		 not a constant or if it cannot fit in the offset field.
// Use [reg+offset] in all other cases.
// 
// This assumes that all array refs are "lowered" to one of these forms:
//	%x = load (subarray*) ptr, constant	; single constant offset
//	%x = load (subarray*) ptr, offsetVal	; single non-constant offset
// Generally, this should happen via strength reduction + LICM.
// Also, strength reduction should take care of using the same register for
// the loop index variable and an array index, when that is profitable.
//------------------------------------------------------------------------ 

static void
SetOperandsForMemInstr(MachineInstr* minstr,
		       const InstructionNode* vmInstrNode,
		       const TargetMachine& targetMachine)
{
  MemAccessInst* memInst = (MemAccessInst*) vmInstrNode->getInstruction();
  
  // Variables to hold the index vector, ptr value, and offset value.
  // The major work here is to extract these for all 3 instruction types
  // and then call the common function SetMemOperands_Internal().
  // 
  const vector<ConstPoolVal*>* idxVec = & memInst->getIndexVec();
  vector<ConstPoolVal*>* newIdxVec = NULL;
  Value* ptrVal;
  Value* arrayOffsetVal = NULL;
  
  // Test if a GetElemPtr instruction is being folded into this mem instrn.
  // If so, it will be in the left child for Load and GetElemPtr,
  // and in the right child for Store instructions.
  // 
  InstrTreeNode* ptrChild = (vmInstrNode->getOpLabel() == Instruction::Store
			     ? vmInstrNode->rightChild()
			     : vmInstrNode->leftChild()); 
  
  if (ptrChild->getOpLabel() == Instruction::GetElementPtr ||
      ptrChild->getOpLabel() == GetElemPtrIdx)
    {
      // There is a GetElemPtr instruction and there may be a chain of
      // more than one.  Use the pointer value of the last one in the chain.
      // Fold the index vectors from the entire chain and from the mem
      // instruction into one single index vector.
      // Finally, we never fold for an array instruction so make that NULL.
      
      newIdxVec = new vector<ConstPoolVal*>;
      ptrVal = FoldGetElemChain((InstructionNode*) ptrChild, *newIdxVec);
      
      newIdxVec->insert(newIdxVec->end(), idxVec->begin(), idxVec->end());
      idxVec = newIdxVec;
      
      assert(! ((PointerType*)ptrVal->getType())->getValueType()->isArrayType()
	     && "GetElemPtr cannot be folded into array refs in selection");
    }
  else
    {
      // There is no GetElemPtr instruction.
      // Use the pointer value and the index vector from the Mem instruction.
      // If it is an array reference, get the array offset value.
      // 
      ptrVal = memInst->getPtrOperand();

      const Type* opType =
	((const PointerType*) ptrVal->getType())->getValueType();
      if (opType->isArrayType())
	{
	  assert((memInst->getNumOperands()
		  == (unsigned) 1 + memInst->getFirstOffsetIdx())
		 && "Array refs must be lowered before Instruction Selection");
	  
	  arrayOffsetVal = memInst->getOperand(memInst->getFirstOffsetIdx());
	}
    }
  
  SetMemOperands_Internal(minstr, vmInstrNode, ptrVal, arrayOffsetVal,
			  *idxVec, targetMachine);
  
  if (newIdxVec != NULL)
    delete newIdxVec;
}


static void
SetMemOperands_Internal(MachineInstr* minstr,
			const InstructionNode* vmInstrNode,
			Value* ptrVal,
			Value* arrayOffsetVal,
			const vector<ConstPoolVal*>& idxVec,
			const TargetMachine& targetMachine)
{
  MemAccessInst* memInst = (MemAccessInst*) vmInstrNode->getInstruction();
  
  // Initialize so we default to storing the offset in a register.
  int64_t smallConstOffset;
  Value* valueForRegOffset = NULL;
  MachineOperand::MachineOperandType offsetOpType =MachineOperand::MO_Register;

  // Check if there is an index vector and if so, if it translates to
  // a small enough constant to fit in the immediate-offset field.
  // 
  if (idxVec.size() > 0)
    {
      bool isConstantOffset = false;
      unsigned offset;
      
      const PointerType* ptrType = (PointerType*) ptrVal->getType();
      
      if (ptrType->getValueType()->isStructType())
	{
	  // the offset is always constant for structs
	  isConstantOffset = true;
	  
	  // Compute the offset value using the index vector
	  offset = MemAccessInst::getIndexedOfsetForTarget(ptrType,
						       idxVec, targetMachine);
	}
      else
	{
	  // It must be an array ref.  Check if the offset is a constant,
	  // and that the indexing has been lowered to a single offset.
	  // 
	  assert(ptrType->getValueType()->isArrayType());
	  assert(arrayOffsetVal != NULL
		 && "Expect to be given Value* for array offsets");
	  
	  if (arrayOffsetVal->getValueType() == Value::ConstantVal)
	    {
	      isConstantOffset = true;	// always constant for structs
	      assert(arrayOffsetVal->getType()->isIntegral());
	      offset = (arrayOffsetVal->getType()->isSigned())
			? ((ConstPoolSInt*) arrayOffsetVal)->getValue()
			: (int64_t) ((ConstPoolUInt*) arrayOffsetVal)->getValue();
	    }
	  else
	    {
	      valueForRegOffset = arrayOffsetVal;
	    }
	}
      
      if (isConstantOffset)
	{
	  // create a virtual register for the constant
	  valueForRegOffset = new ConstPoolSInt(Type::IntTy, offset);
	}
    }
  else
    {
      offsetOpType = MachineOperand::MO_SignExtendedImmed;
      smallConstOffset = 0;
    }
  
  // Operand 0 is value for STORE, ptr for LOAD or GET_ELEMENT_PTR
  // It is the left child in the instruction tree in all cases.
  Value* leftVal = vmInstrNode->leftChild()->getValue();
  minstr->SetMachineOperand(0, MachineOperand::MO_Register, leftVal);
  
  // Operand 1 is ptr for STORE, offset for LOAD or GET_ELEMENT_PTR
  // Operand 3 is offset for STORE, result reg for LOAD or GET_ELEMENT_PTR
  //
  unsigned offsetOpNum = (memInst->getOpcode() == Instruction::Store)? 2 : 1;
  if (offsetOpType == MachineOperand::MO_Register)
    {
      assert(valueForRegOffset != NULL);
      minstr->SetMachineOperand(offsetOpNum, offsetOpType, valueForRegOffset); 
    }
  else
    minstr->SetMachineOperand(offsetOpNum, offsetOpType, smallConstOffset);
  
  if (memInst->getOpcode() == Instruction::Store)
    minstr->SetMachineOperand(1, MachineOperand::MO_Register, ptrVal);
  else
    minstr->SetMachineOperand(2, MachineOperand::MO_Register,
			         vmInstrNode->getValue());
}


// Create one or two load instructions to load constants from the
// constant pool.  The first instructions is stored in instrA;
// the second (if any) in instrB.
//
static unsigned
FixConstantOperands(const InstructionNode* vmInstrNode,
		    MachineInstr** mvec,
		    unsigned numInstr,
		    TargetMachine& targetMachine)
{
  static MachineInstr* loadConstVec[MAX_INSTR_PER_VMINSTR];

  unsigned numNew = 0;
  Instruction* vmInstr = vmInstrNode->getInstruction();
  
  for (unsigned i=0; i < numInstr; i++)
    {
      MachineInstr* minstr = mvec[i];
      const MachineInstrInfo& instrInfo =
	targetMachine.machineInstrInfo[minstr->getOpCode()];
      
      for (unsigned op=0; op < instrInfo.numOperands; op++)
	{
	  const MachineOperand& mop = minstr->getOperand(op);
	  Value* opValue = mop.value;
	  
	  // skip the result position and any other positions already
	  // marked as not a virtual register
	  if (instrInfo.resultPos == (int) op || 
	      opValue == NULL ||
	      ! (mop.machineOperandType == MachineOperand::MO_Register &&
	         mop.vregType == MachineOperand::MO_VirtualReg))
	    {
	      break;
	    }
	  
	  if (opValue->getValueType() == Value::ConstantVal)
	    {
	      MachineOperand::VirtualRegisterType vregType;
	      unsigned int machineRegNum;
	      int64_t immedValue;
	      MachineOperand::MachineOperandType opType =
		ChooseRegOrImmed(opValue, minstr->getOpCode(), targetMachine,
				 /*canUseImmed*/ (op == 1),
				 vregType, machineRegNum, immedValue);
	      
	      if (opType == MachineOperand::MO_Register)
		{
		  if (vregType == MachineOperand::MO_MachineReg)
		    minstr->SetMachineOperand(op, machineRegNum);
		  else
		    { // value is constant and must be loaded into a register
		      loadConstVec[numNew++] =
			MakeOneLoadConstInstr(vmInstr, opValue);
		    }
		}
	      else
		minstr->SetMachineOperand(op, opType, immedValue);
	    }

#if 0
	      // Either put the constant in the immediate field or
	      // create an instruction to "put" it in a register.
	      // Check first if it is 0 and then just use %g0.
	      //

	      bool isValidConstant;
	      int64_t intValue = GetConstantValueAsSignedInt(opValue,
							     isValidConstant);
	      if (intValue == 0 && targetMachine.zeroRegNum >= 0)
		{// put it in register %g0
		  minstr->SetMachineOperand(op, targetMachine.zeroRegNum);
		}
	      else if (op == 1 &&
		       instrInfo.constantFitsInImmedField(intValue))
		{// put it in the immediate field
		  MachineOperand::MachineOperandType opType =
		    (opValue->getType()->isSigned()
		     ? MachineOperand::MO_SignExtendedImmed
		     : MachineOperand::MO_UnextendedImmed);
		  
		  minstr->SetMachineOperand(op, opType, intValue);
		}
	      else
		{// create an instruction to "put" the value in a register.
		  loadConstVec[numNew++] =
		    MakeOneLoadConstInstr(vmInstr, opValue);
		}
#endif
	}
    }
  
  if (numNew > 0)
    {
      // Insert the new instructions *before* the old ones by moving
      // the old ones over `numNew' positions (last-to-first, of course!).
      // 
      for (int i=numInstr-1; i >= ((int) numInstr) - (int) numNew; i--)
	mvec[i+numNew] = mvec[i];
      
      for (unsigned i=0; i < numNew; i++)
	mvec[i] = loadConstVec[i];
    }
  
  return (numInstr + numNew);
}


// Create one or two load instructions to load constants from the
// constant pool.  The first instructions is stored in instrA;
// the second (if any) in instrB.
//
static unsigned
InsertLoadConstInstructions(unsigned loadConstFlags,
			    const InstructionNode* vmInstrNode,
			    MachineInstr** mvec,
			    unsigned numInstr)
{
  MachineInstr *instrA = NULL, *instrB = NULL;
    
  unsigned numNew = 0;
  
  if (loadConstFlags & 0x01)
    {
      instrA = MakeOneLoadConstInstr(vmInstrNode->getInstruction(),
				     vmInstrNode->leftChild()->getValue());
      numNew++;
    }
  
  if (loadConstFlags & 0x02)
    {
      instrB = MakeOneLoadConstInstr(vmInstrNode->getInstruction(),
				     vmInstrNode->rightChild()->getValue());
      numNew++;
    }
  
  // Now insert the new instructions *before* the old ones by
  // moving the old ones over `numNew' positions (last-to-first, of course!).
  // 
  for (int i=numInstr-1; i >= ((int) numInstr) - (int) numNew; i--)
    mvec[i+numNew] = mvec[i];
  
  unsigned whichNew = 0;
  if (instrA != NULL)
    mvec[whichNew++] = instrA; 
  if (instrB != NULL)
    mvec[whichNew++] = instrB; 
  assert(whichNew == numNew);

  return numInstr + numNew;
}


static MachineInstr*
MakeOneLoadConstInstr(Instruction* vmInstr,
		      Value* val)
{
  assert(val->getValueType() == Value::ConstantVal);
  
  MachineInstr* minstr;
  
  // Use a "set" instruction for known constants that can go in an integer reg.
  // Use a "load" instruction for all other constants, in particular,
  // floating point constants.
  // 
  const Type* valType = val->getType();
  if (valType->isIntegral() ||
      valType->isPointerType() ||
      valType == Type::BoolTy)
    {
      bool isValidConstant;
      if (val->getType()->isSigned())
	{
	  minstr = new MachineInstr(SETSW);
	  minstr->SetMachineOperand(0, MachineOperand::MO_SignExtendedImmed,
			    GetSignedIntConstantValue(val, isValidConstant));
	}
      else
	{
	  minstr = new MachineInstr(SETUW);
	  minstr->SetMachineOperand(0, MachineOperand::MO_UnextendedImmed,
			    GetUnsignedIntConstantValue(val, isValidConstant));
	}
      minstr->SetMachineOperand(1, MachineOperand::MO_Register, val);
      assert(isValidConstant && "Unrecognized constant");
    }
  else
    {
      assert(valType == Type::FloatTy || 
	     valType == Type::DoubleTy);
      
      int64_t zeroOffset = 0; // to avoid overloading ambiguity with (Value*) 0
      
      // Make a Load instruction, and make `val' both the ptr value *and*
      // the result value, and set the offset field to 0.  Final code
      // generation will have to generate the base+offset for the constant.
      // 
      minstr = new MachineInstr(ChooseLoadInstruction(val->getType()));
      minstr->SetMachineOperand(0, MachineOperand::MO_Register, val);
      minstr->SetMachineOperand(1, MachineOperand::MO_SignExtendedImmed,
				   zeroOffset);
      minstr->SetMachineOperand(2, MachineOperand::MO_Register, val);
      
    }
  
  Instruction* tmpInstr = new TmpInstruction(Instruction::UserOp1, val, NULL);
  vmInstr->getMachineInstrVec().addTempValue(tmpInstr);
  
  return minstr;
}


// This function is currently unused and incomplete but will be 
// used if we have a linear layout of basic blocks in LLVM code.
// It decides which branch should fall-through, and whether an
// extra unconditional branch is needed (when neither falls through).
// 
void
ChooseBranchPattern(Instruction* vmInstr, BranchPattern& brPattern)
{
  BranchInst* brInstr = (BranchInst*) vmInstr;
  
  brPattern.flipCondition = false;
  brPattern.targetBB	  = brInstr->getSuccessor(0);
  brPattern.extraBranch   = NULL;
  
  assert(brInstr->getNumSuccessors() > 1 &&
	 "Unnecessary analysis for unconditional branch");
  
  assert(0 && "Fold branches in peephole optimization");
}

