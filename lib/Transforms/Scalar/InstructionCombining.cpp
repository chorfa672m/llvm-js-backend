//===- InstructionCombining.cpp - Combine multiple instructions -----------===//
//
// InstructionCombining - Combine instructions to form fewer, simple
//   instructions.  This pass does not modify the CFG, and has a tendancy to
//   make instructions dead, so a subsequent DIE pass is useful.  This pass is
//   where algebraic simplification happens.
//
// This pass combines things like:
//    %Y = add int 1, %X
//    %Z = add int 1, %Y
// into:
//    %Z = add int 2, %X
//
// This is a simple worklist driven algorithm.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/ConstantHandling.h"
#include "llvm/iMemory.h"
#include "llvm/iOther.h"
#include "llvm/iPHINode.h"
#include "llvm/iOperators.h"
#include "llvm/Pass.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/InstVisitor.h"
#include "Support/StatisticReporter.h"
#include <algorithm>

static Statistic<> NumCombined("instcombine\t- Number of insts combined");

namespace {
  class InstCombiner : public FunctionPass,
                       public InstVisitor<InstCombiner, Instruction*> {
    // Worklist of all of the instructions that need to be simplified.
    std::vector<Instruction*> WorkList;

    void AddUsesToWorkList(Instruction &I) {
      // The instruction was simplified, add all users of the instruction to
      // the work lists because they might get more simplified now...
      //
      for (Value::use_iterator UI = I.use_begin(), UE = I.use_end();
           UI != UE; ++UI)
        WorkList.push_back(cast<Instruction>(*UI));
    }

  public:
    virtual bool runOnFunction(Function &F);

    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.preservesCFG();
    }

    // Visitation implementation - Implement instruction combining for different
    // instruction types.  The semantics are as follows:
    // Return Value:
    //    null        - No change was made
    //     I          - Change was made, I is still valid, I may be dead though
    //   otherwise    - Change was made, replace I with returned instruction
    //   
    Instruction *visitNot(UnaryOperator &I);
    Instruction *visitAdd(BinaryOperator &I);
    Instruction *visitSub(BinaryOperator &I);
    Instruction *visitMul(BinaryOperator &I);
    Instruction *visitDiv(BinaryOperator &I);
    Instruction *visitRem(BinaryOperator &I);
    Instruction *visitAnd(BinaryOperator &I);
    Instruction *visitOr (BinaryOperator &I);
    Instruction *visitXor(BinaryOperator &I);
    Instruction *visitSetCondInst(BinaryOperator &I);
    Instruction *visitShiftInst(Instruction &I);
    Instruction *visitCastInst(CastInst &CI);
    Instruction *visitPHINode(PHINode &PN);
    Instruction *visitGetElementPtrInst(GetElementPtrInst &GEP);

    // visitInstruction - Specify what to return for unhandled instructions...
    Instruction *visitInstruction(Instruction &I) { return 0; }

    // InsertNewInstBefore - insert an instruction New before instruction Old
    // in the program.  Add the new instruction to the worklist.
    //
    void InsertNewInstBefore(Instruction *New, Instruction &Old) {
      BasicBlock *BB = Old.getParent();
      BB->getInstList().insert(&Old, New);  // Insert inst
      WorkList.push_back(New);              // Add to worklist
    }

    // ReplaceInstUsesWith - This method is to be used when an instruction is
    // found to be dead, replacable with another preexisting expression.  Here
    // we add all uses of I to the worklist, replace all uses of I with the new
    // value, then return I, so that the inst combiner will know that I was
    // modified.
    //
    Instruction *ReplaceInstUsesWith(Instruction &I, Value *V) {
      AddUsesToWorkList(I);         // Add all modified instrs to worklist
      I.replaceAllUsesWith(V);
      return &I;
    }
  };

  RegisterOpt<InstCombiner> X("instcombine", "Combine redundant instructions");
}


Instruction *InstCombiner::visitNot(UnaryOperator &I) {
  // not (not X) = X
  if (Instruction *Op = dyn_cast<Instruction>(I.getOperand(0)))
    if (Op->getOpcode() == Instruction::Not)
      return ReplaceInstUsesWith(I, Op->getOperand(0));
  return 0;
}


// Make sure that this instruction has a constant on the right hand side if it
// has any constant arguments.  If not, fix it an return true.
//
static bool SimplifyBinOp(BinaryOperator &I) {
  if (isa<Constant>(I.getOperand(0)) && !isa<Constant>(I.getOperand(1)))
    return !I.swapOperands();
  return false;
}

// dyn_castNegInst - Given a 'sub' instruction, return the RHS of the
// instruction if the LHS is a constant zero (which is the 'negate' form).
//
static inline Value *dyn_castNegInst(Value *V) {
  Instruction *I = dyn_cast<Instruction>(V);
  if (!I || I->getOpcode() != Instruction::Sub) return 0;

  if (I->getOperand(0) == Constant::getNullValue(I->getType()))
    return I->getOperand(1);
  return 0;
}

Instruction *InstCombiner::visitAdd(BinaryOperator &I) {
  bool Changed = SimplifyBinOp(I);
  Value *LHS = I.getOperand(0), *RHS = I.getOperand(1);

  // Eliminate 'add int %X, 0'
  if (RHS == Constant::getNullValue(I.getType()))
    return ReplaceInstUsesWith(I, LHS);

  // -A + B  -->  B - A
  if (Value *V = dyn_castNegInst(LHS))
    return BinaryOperator::create(Instruction::Sub, RHS, V);

  // A + -B  -->  A - B
  if (Value *V = dyn_castNegInst(RHS))
    return BinaryOperator::create(Instruction::Sub, LHS, V);

  // Simplify add instructions with a constant RHS...
  if (Constant *Op2 = dyn_cast<Constant>(RHS)) {
    if (BinaryOperator *ILHS = dyn_cast<BinaryOperator>(LHS)) {
      if (ILHS->getOpcode() == Instruction::Add &&
          isa<Constant>(ILHS->getOperand(1))) {
        // Fold:
        //    %Y = add int %X, 1
        //    %Z = add int %Y, 1
        // into:
        //    %Z = add int %X, 2
        //
        if (Constant *Val = *Op2 + *cast<Constant>(ILHS->getOperand(1))) {
          I.setOperand(0, ILHS->getOperand(0));
          I.setOperand(1, Val);
          return &I;
        }
      }
    }
  }

  return Changed ? &I : 0;
}

Instruction *InstCombiner::visitSub(BinaryOperator &I) {
  Value *Op0 = I.getOperand(0), *Op1 = I.getOperand(1);

  if (Op0 == Op1)         // sub X, X  -> 0
    return ReplaceInstUsesWith(I, Constant::getNullValue(I.getType()));

  // If this is a subtract instruction with a constant RHS, convert it to an add
  // instruction of a negative constant
  //
  if (Constant *Op2 = dyn_cast<Constant>(Op1))
    if (Constant *RHS = *Constant::getNullValue(I.getType()) - *Op2) // 0 - RHS
      return BinaryOperator::create(Instruction::Add, Op0, RHS, I.getName());

  // If this is a 'B = x-(-A)', change to B = x+A...
  if (Value *V = dyn_castNegInst(Op1))
    return BinaryOperator::create(Instruction::Add, Op0, V);

  // Replace (x - (y - z)) with (x + (z - y)) if the (y - z) subexpression is
  // not used by anyone else...
  //
  if (BinaryOperator *Op1I = dyn_cast<BinaryOperator>(Op1))
    if (Op1I->use_size() == 1 && Op1I->getOpcode() == Instruction::Sub) {
      // Swap the two operands of the subexpr...
      Value *IIOp0 = Op1I->getOperand(0), *IIOp1 = Op1I->getOperand(1);
      Op1I->setOperand(0, IIOp1);
      Op1I->setOperand(1, IIOp0);

      // Create the new top level add instruction...
      return BinaryOperator::create(Instruction::Add, Op0, Op1);
    }
  return 0;
}

Instruction *InstCombiner::visitMul(BinaryOperator &I) {
  bool Changed = SimplifyBinOp(I);
  Value *Op1 = I.getOperand(0);

  // Simplify mul instructions with a constant RHS...
  if (Constant *Op2 = dyn_cast<Constant>(I.getOperand(1))) {
    if (I.getType()->isIntegral() && cast<ConstantInt>(Op2)->equalsInt(1))
      return ReplaceInstUsesWith(I, Op1);  // Eliminate 'mul int %X, 1'

    if (I.getType()->isIntegral() && cast<ConstantInt>(Op2)->equalsInt(2))
      // Convert 'mul int %X, 2' to 'add int %X, %X'
      return BinaryOperator::create(Instruction::Add, Op1, Op1, I.getName());

    if (Op2->isNullValue())
      return ReplaceInstUsesWith(I, Op2);  // Eliminate 'mul int %X, 0'
  }

  return Changed ? &I : 0;
}


Instruction *InstCombiner::visitDiv(BinaryOperator &I) {
  // div X, 1 == X
  if (ConstantInt *RHS = dyn_cast<ConstantInt>(I.getOperand(1)))
    if (RHS->equalsInt(1))
      return ReplaceInstUsesWith(I, I.getOperand(0));
  return 0;
}


Instruction *InstCombiner::visitRem(BinaryOperator &I) {
  // rem X, 1 == 0
  if (ConstantInt *RHS = dyn_cast<ConstantInt>(I.getOperand(1)))
    if (RHS->equalsInt(1))
      return ReplaceInstUsesWith(I, Constant::getNullValue(I.getType()));

  return 0;
}

// isMaxValueMinusOne - return true if this is Max-1
static bool isMaxValueMinusOne(const ConstantInt *C) {
  if (const ConstantUInt *CU = dyn_cast<ConstantUInt>(C)) {
    // Calculate -1 casted to the right type...
    unsigned TypeBits = C->getType()->getPrimitiveSize()*8;
    uint64_t Val = ~0ULL;                // All ones
    Val >>= 64-TypeBits;                 // Shift out unwanted 1 bits...
    return CU->getValue() == Val-1;
  }

  const ConstantSInt *CS = cast<ConstantSInt>(C);
  
  // Calculate 0111111111..11111
  unsigned TypeBits = C->getType()->getPrimitiveSize()*8;
  int64_t Val = INT64_MAX;             // All ones
  Val >>= 64-TypeBits;                 // Shift out unwanted 1 bits...
  return CS->getValue() == Val-1;
}

// isMinValuePlusOne - return true if this is Min+1
static bool isMinValuePlusOne(const ConstantInt *C) {
  if (const ConstantUInt *CU = dyn_cast<ConstantUInt>(C))
    return CU->getValue() == 1;

  const ConstantSInt *CS = cast<ConstantSInt>(C);
  
  // Calculate 1111111111000000000000 
  unsigned TypeBits = C->getType()->getPrimitiveSize()*8;
  int64_t Val = -1;                    // All ones
  Val <<= TypeBits-1;                  // Shift over to the right spot
  return CS->getValue() == Val+1;
}


Instruction *InstCombiner::visitAnd(BinaryOperator &I) {
  bool Changed = SimplifyBinOp(I);
  Value *Op0 = I.getOperand(0), *Op1 = I.getOperand(1);

  // and X, X = X   and X, 0 == 0
  if (Op0 == Op1 || Op1 == Constant::getNullValue(I.getType()))
    return ReplaceInstUsesWith(I, Op1);

  // and X, -1 == X
  if (ConstantIntegral *RHS = dyn_cast<ConstantIntegral>(Op1))
    if (RHS->isAllOnesValue())
      return ReplaceInstUsesWith(I, Op0);

  return Changed ? &I : 0;
}



Instruction *InstCombiner::visitOr(BinaryOperator &I) {
  bool Changed = SimplifyBinOp(I);
  Value *Op0 = I.getOperand(0), *Op1 = I.getOperand(1);

  // or X, X = X   or X, 0 == X
  if (Op0 == Op1 || Op1 == Constant::getNullValue(I.getType()))
    return ReplaceInstUsesWith(I, Op0);

  // or X, -1 == -1
  if (ConstantIntegral *RHS = dyn_cast<ConstantIntegral>(Op1))
    if (RHS->isAllOnesValue())
      return ReplaceInstUsesWith(I, Op1);

  return Changed ? &I : 0;
}



Instruction *InstCombiner::visitXor(BinaryOperator &I) {
  bool Changed = SimplifyBinOp(I);
  Value *Op0 = I.getOperand(0), *Op1 = I.getOperand(1);

  // xor X, X = 0
  if (Op0 == Op1)
    return ReplaceInstUsesWith(I, Constant::getNullValue(I.getType()));

  if (ConstantIntegral *Op1C = dyn_cast<ConstantIntegral>(Op1)) {
    // xor X, 0 == X
    if (Op1C->isNullValue())
      return ReplaceInstUsesWith(I, Op0);

    // xor X, -1 = not X
    if (Op1C->isAllOnesValue())
      return UnaryOperator::create(Instruction::Not, Op0, I.getName());
  }

  return Changed ? &I : 0;
}

// AddOne, SubOne - Add or subtract a constant one from an integer constant...
static Constant *AddOne(ConstantInt *C) {
  Constant *Result = *C + *ConstantInt::get(C->getType(), 1);
  assert(Result && "Constant folding integer addition failed!");
  return Result;
}
static Constant *SubOne(ConstantInt *C) {
  Constant *Result = *C - *ConstantInt::get(C->getType(), 1);
  assert(Result && "Constant folding integer addition failed!");
  return Result;
}

// isTrueWhenEqual - Return true if the specified setcondinst instruction is
// true when both operands are equal...
//
static bool isTrueWhenEqual(Instruction &I) {
  return I.getOpcode() == Instruction::SetEQ ||
         I.getOpcode() == Instruction::SetGE ||
         I.getOpcode() == Instruction::SetLE;
}

Instruction *InstCombiner::visitSetCondInst(BinaryOperator &I) {
  bool Changed = SimplifyBinOp(I);
  Value *Op0 = I.getOperand(0), *Op1 = I.getOperand(1);
  const Type *Ty = Op0->getType();

  // setcc X, X
  if (Op0 == Op1)
    return ReplaceInstUsesWith(I, ConstantBool::get(isTrueWhenEqual(I)));

  // setcc <global*>, 0 - Global value addresses are never null!
  if (isa<GlobalValue>(Op0) && isa<ConstantPointerNull>(Op1))
    return ReplaceInstUsesWith(I, ConstantBool::get(!isTrueWhenEqual(I)));

  // setcc's with boolean values can always be turned into bitwise operations
  if (Ty == Type::BoolTy) {
    // If this is <, >, or !=, we can change this into a simple xor instruction
    if (!isTrueWhenEqual(I))
      return BinaryOperator::create(Instruction::Xor, Op0, Op1, I.getName());

    // Otherwise we need to make a temporary intermediate instruction and insert
    // it into the instruction stream.  This is what we are after:
    //
    //  seteq bool %A, %B -> ~(A^B)
    //  setle bool %A, %B -> ~A | B
    //  setge bool %A, %B -> A | ~B
    //
    if (I.getOpcode() == Instruction::SetEQ) {  // seteq case
      Instruction *Xor = BinaryOperator::create(Instruction::Xor, Op0, Op1,
                                                I.getName()+"tmp");
      InsertNewInstBefore(Xor, I);
      return UnaryOperator::create(Instruction::Not, Xor, I.getName());
    }

    // Handle the setXe cases...
    assert(I.getOpcode() == Instruction::SetGE ||
           I.getOpcode() == Instruction::SetLE);

    if (I.getOpcode() == Instruction::SetGE)
      std::swap(Op0, Op1);                   // Change setge -> setle

    // Now we just have the SetLE case.
    Instruction *Not =
      UnaryOperator::create(Instruction::Not, Op0, I.getName()+"tmp");
    InsertNewInstBefore(Not, I);
    return BinaryOperator::create(Instruction::Or, Not, Op1, I.getName());
  }

  // Check to see if we are doing one of many comparisons against constant
  // integers at the end of their ranges...
  //
  if (ConstantInt *CI = dyn_cast<ConstantInt>(Op1)) {
    // Check to see if we are comparing against the minimum or maximum value...
    if (CI->isMinValue()) {
      if (I.getOpcode() == Instruction::SetLT)       // A < MIN -> FALSE
        return ReplaceInstUsesWith(I, ConstantBool::False);
      if (I.getOpcode() == Instruction::SetGE)       // A >= MIN -> TRUE
        return ReplaceInstUsesWith(I, ConstantBool::True);
      if (I.getOpcode() == Instruction::SetLE)       // A <= MIN -> A == MIN
        return BinaryOperator::create(Instruction::SetEQ, Op0,Op1, I.getName());
      if (I.getOpcode() == Instruction::SetGT)       // A > MIN -> A != MIN
        return BinaryOperator::create(Instruction::SetNE, Op0,Op1, I.getName());

    } else if (CI->isMaxValue()) {
      if (I.getOpcode() == Instruction::SetGT)       // A > MAX -> FALSE
        return ReplaceInstUsesWith(I, ConstantBool::False);
      if (I.getOpcode() == Instruction::SetLE)       // A <= MAX -> TRUE
        return ReplaceInstUsesWith(I, ConstantBool::True);
      if (I.getOpcode() == Instruction::SetGE)       // A >= MAX -> A == MAX
        return BinaryOperator::create(Instruction::SetEQ, Op0,Op1, I.getName());
      if (I.getOpcode() == Instruction::SetLT)       // A < MAX -> A != MAX
        return BinaryOperator::create(Instruction::SetNE, Op0,Op1, I.getName());

      // Comparing against a value really close to min or max?
    } else if (isMinValuePlusOne(CI)) {
      if (I.getOpcode() == Instruction::SetLT)       // A < MIN+1 -> A == MIN
        return BinaryOperator::create(Instruction::SetEQ, Op0,
                                      SubOne(CI), I.getName());
      if (I.getOpcode() == Instruction::SetGE)       // A >= MIN-1 -> A != MIN
        return BinaryOperator::create(Instruction::SetNE, Op0,
                                      SubOne(CI), I.getName());

    } else if (isMaxValueMinusOne(CI)) {
      if (I.getOpcode() == Instruction::SetGT)       // A > MAX-1 -> A == MAX
        return BinaryOperator::create(Instruction::SetEQ, Op0,
                                      AddOne(CI), I.getName());
      if (I.getOpcode() == Instruction::SetLE)       // A <= MAX-1 -> A != MAX
        return BinaryOperator::create(Instruction::SetNE, Op0,
                                      AddOne(CI), I.getName());
    }
  }

  return Changed ? &I : 0;
}



Instruction *InstCombiner::visitShiftInst(Instruction &I) {
  assert(I.getOperand(1)->getType() == Type::UByteTy);
  Value *Op0 = I.getOperand(0), *Op1 = I.getOperand(1);

  // shl X, 0 == X and shr X, 0 == X
  // shl 0, X == 0 and shr 0, X == 0
  if (Op1 == Constant::getNullValue(Type::UByteTy) ||
      Op0 == Constant::getNullValue(Op0->getType()))
    return ReplaceInstUsesWith(I, Op0);

  // shl uint X, 32 = 0 and shr ubyte Y, 9 = 0, ... just don't eliminate shr of
  // a signed value.
  //
  if (ConstantUInt *CUI = dyn_cast<ConstantUInt>(Op1)) {
    unsigned TypeBits = Op0->getType()->getPrimitiveSize()*8;
    if (CUI->getValue() >= TypeBits &&
        !(Op0->getType()->isSigned() && I.getOpcode() == Instruction::Shr))
      return ReplaceInstUsesWith(I, Constant::getNullValue(Op0->getType()));
  }
  return 0;
}


// isEliminableCastOfCast - Return true if it is valid to eliminate the CI
// instruction.
//
static inline bool isEliminableCastOfCast(const CastInst &CI,
                                          const CastInst *CSrc) {
  assert(CI.getOperand(0) == CSrc);
  const Type *SrcTy = CSrc->getOperand(0)->getType();
  const Type *MidTy = CSrc->getType();
  const Type *DstTy = CI.getType();

  // It is legal to eliminate the instruction if casting A->B->A if the sizes
  // are identical and the bits don't get reinterpreted (for example 
  // int->float->int)
  if (SrcTy == DstTy && SrcTy->isLosslesslyConvertableTo(MidTy))
    return true;

  // Allow free casting and conversion of sizes as long as the sign doesn't
  // change...
  if (SrcTy->isIntegral() && MidTy->isIntegral() && DstTy->isIntegral() &&
      SrcTy->isSigned() == MidTy->isSigned() &&
      MidTy->isSigned() == DstTy->isSigned()) {
    // Only accept cases where we are either monotonically increasing the type
    // size, or monotonically decreasing it.
    //
    unsigned SrcSize = SrcTy->getPrimitiveSize();
    unsigned MidSize = MidTy->getPrimitiveSize();
    unsigned DstSize = DstTy->getPrimitiveSize();
    if (SrcSize < MidSize && MidSize < DstSize)
      return true;

    if (SrcSize > MidSize && MidSize > DstSize)
      return true;
  }

  // Otherwise, we cannot succeed.  Specifically we do not want to allow things
  // like:  short -> ushort -> uint, because this can create wrong results if
  // the input short is negative!
  //
  return false;
}


// CastInst simplification
//
Instruction *InstCombiner::visitCastInst(CastInst &CI) {
  // If the user is casting a value to the same type, eliminate this cast
  // instruction...
  if (CI.getType() == CI.getOperand(0)->getType())
    return ReplaceInstUsesWith(CI, CI.getOperand(0));

  // If casting the result of another cast instruction, try to eliminate this
  // one!
  //
  if (CastInst *CSrc = dyn_cast<CastInst>(CI.getOperand(0))) {
    if (isEliminableCastOfCast(CI, CSrc)) {
      // This instruction now refers directly to the cast's src operand.  This
      // has a good chance of making CSrc dead.
      CI.setOperand(0, CSrc->getOperand(0));
      return &CI;
    }

    // If this is an A->B->A cast, and we are dealing with integral types, try
    // to convert this into a logical 'and' instruction.
    //
    if (CSrc->getOperand(0)->getType() == CI.getType() &&
        CI.getType()->isIntegral() && CSrc->getType()->isIntegral() &&
        CI.getType()->isUnsigned() && CSrc->getType()->isUnsigned() &&
        CSrc->getType()->getPrimitiveSize() < CI.getType()->getPrimitiveSize()){
      assert(CSrc->getType() != Type::ULongTy &&
             "Cannot have type bigger than ulong!");
      unsigned AndValue = (1U << CSrc->getType()->getPrimitiveSize()*8)-1;
      Constant *AndOp = ConstantUInt::get(CI.getType(), AndValue);
      return BinaryOperator::create(Instruction::And, CSrc->getOperand(0),
                                    AndOp);
    }
  }

  return 0;
}


// PHINode simplification
//
Instruction *InstCombiner::visitPHINode(PHINode &PN) {
  // If the PHI node only has one incoming value, eliminate the PHI node...
  if (PN.getNumIncomingValues() == 1)
    return ReplaceInstUsesWith(PN, PN.getIncomingValue(0));

  return 0;
}


Instruction *InstCombiner::visitGetElementPtrInst(GetElementPtrInst &GEP) {
  // Is it 'getelementptr %P, uint 0'  or 'getelementptr %P'
  // If so, eliminate the noop.
  if ((GEP.getNumOperands() == 2 &&
       GEP.getOperand(1) == Constant::getNullValue(Type::UIntTy)) ||
      GEP.getNumOperands() == 1)
    return ReplaceInstUsesWith(GEP, GEP.getOperand(0));

  // Combine Indices - If the source pointer to this getelementptr instruction
  // is a getelementptr instruction, combine the indices of the two
  // getelementptr instructions into a single instruction.
  //
  if (GetElementPtrInst *Src =
      dyn_cast<GetElementPtrInst>(GEP.getPointerOperand())) {
    std::vector<Value *> Indices;
  
    // Can we combine the two pointer arithmetics offsets?
    if (Src->getNumOperands() == 2 && isa<Constant>(Src->getOperand(1)) &&
        isa<Constant>(GEP.getOperand(1))) {
      // Replace the index list on this GEP with the index on the getelementptr
      Indices.insert(Indices.end(), GEP.idx_begin(), GEP.idx_end());
      Indices[0] = *cast<Constant>(Src->getOperand(1)) +
                   *cast<Constant>(GEP.getOperand(1));
      assert(Indices[0] != 0 && "Constant folding of uint's failed!?");

    } else if (*GEP.idx_begin() == ConstantUInt::get(Type::UIntTy, 0)) { 
      // Otherwise we can do the fold if the first index of the GEP is a zero
      Indices.insert(Indices.end(), Src->idx_begin(), Src->idx_end());
      Indices.insert(Indices.end(), GEP.idx_begin()+1, GEP.idx_end());
    }

    if (!Indices.empty())
      return new GetElementPtrInst(Src->getOperand(0), Indices, GEP.getName());
  }

  return 0;
}


bool InstCombiner::runOnFunction(Function &F) {
  bool Changed = false;

  WorkList.insert(WorkList.end(), inst_begin(F), inst_end(F));

  while (!WorkList.empty()) {
    Instruction *I = WorkList.back();  // Get an instruction from the worklist
    WorkList.pop_back();

    // Now that we have an instruction, try combining it to simplify it...
    if (Instruction *Result = visit(*I)) {
      ++NumCombined;
      // Should we replace the old instruction with a new one?
      if (Result != I) {
        // Instructions can end up on the worklist more than once.  Make sure
        // we do not process an instruction that has been deleted.
        WorkList.erase(std::remove(WorkList.begin(), WorkList.end(), I),
                       WorkList.end());

        ReplaceInstWithInst(I, Result);
      } else {
        BasicBlock::iterator II = I;

        // If the instruction was modified, it's possible that it is now dead.
        // if so, remove it.
        if (dceInstruction(II)) {
          // Instructions may end up in the worklist more than once.  Erase them
          // all.
          WorkList.erase(std::remove(WorkList.begin(), WorkList.end(), I),
                         WorkList.end());
          Result = 0;
        }
      }

      if (Result) {
        WorkList.push_back(Result);
        AddUsesToWorkList(*Result);
      }
      Changed = true;
    }
  }

  return Changed;
}

Pass *createInstructionCombiningPass() {
  return new InstCombiner();
}
