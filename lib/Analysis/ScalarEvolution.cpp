//===- ScalarEvolution.cpp - Scalar Evolution Analysis ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the implementation of the scalar evolution analysis
// engine, which is used primarily to analyze expressions involving induction
// variables in loops.
//
// There are several aspects to this library.  First is the representation of
// scalar expressions, which are represented as subclasses of the SCEV class.
// These classes are used to represent certain types of subexpressions that we
// can handle.  These classes are reference counted, managed by the const SCEV *
// class.  We only create one SCEV of a particular shape, so pointer-comparisons
// for equality are legal.
//
// One important aspect of the SCEV objects is that they are never cyclic, even
// if there is a cycle in the dataflow for an expression (ie, a PHI node).  If
// the PHI node is one of the idioms that we can represent (e.g., a polynomial
// recurrence) then we represent it directly as a recurrence node, otherwise we
// represent it as a SCEVUnknown node.
//
// In addition to being able to represent expressions of various types, we also
// have folders that are used to build the *canonical* representation for a
// particular expression.  These folders are capable of using a variety of
// rewrite rules to simplify the expressions.
//
// Once the folders are defined, we can implement the more interesting
// higher-level code, such as the code that recognizes PHI nodes of various
// types, computes the execution count of a loop, etc.
//
// TODO: We should use these routines and value representations to implement
// dependence analysis!
//
//===----------------------------------------------------------------------===//
//
// There are several good references for the techniques used in this analysis.
//
//  Chains of recurrences -- a method to expedite the evaluation
//  of closed-form functions
//  Olaf Bachmann, Paul S. Wang, Eugene V. Zima
//
//  On computational properties of chains of recurrences
//  Eugene V. Zima
//
//  Symbolic Evaluation of Chains of Recurrences for Loop Optimization
//  Robert A. van Engelen
//
//  Efficient Symbolic Analysis for Optimizing Compilers
//  Robert A. van Engelen
//
//  Using the chains of recurrences algebra for data dependence testing and
//  induction variable substitution
//  MS Thesis, Johnie Birch
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "scalar-evolution"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Instructions.h"
#include "llvm/LLVMContext.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Assembly/Writer.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ConstantRange.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <algorithm>
using namespace llvm;

STATISTIC(NumArrayLenItCounts,
          "Number of trip counts computed with array length");
STATISTIC(NumTripCountsComputed,
          "Number of loops with predictable loop counts");
STATISTIC(NumTripCountsNotComputed,
          "Number of loops without predictable loop counts");
STATISTIC(NumBruteForceTripCountsComputed,
          "Number of loops with trip counts computed by force");

static cl::opt<unsigned>
MaxBruteForceIterations("scalar-evolution-max-iterations", cl::ReallyHidden,
                        cl::desc("Maximum number of iterations SCEV will "
                                 "symbolically execute a constant "
                                 "derived loop"),
                        cl::init(100));

static RegisterPass<ScalarEvolution>
R("scalar-evolution", "Scalar Evolution Analysis", false, true);
char ScalarEvolution::ID = 0;

//===----------------------------------------------------------------------===//
//                           SCEV class definitions
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Implementation of the SCEV class.
//

SCEV::~SCEV() {}

void SCEV::dump() const {
  print(errs());
  errs() << '\n';
}

void SCEV::print(std::ostream &o) const {
  raw_os_ostream OS(o);
  print(OS);
}

bool SCEV::isZero() const {
  if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(this))
    return SC->getValue()->isZero();
  return false;
}

bool SCEV::isOne() const {
  if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(this))
    return SC->getValue()->isOne();
  return false;
}

bool SCEV::isAllOnesValue() const {
  if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(this))
    return SC->getValue()->isAllOnesValue();
  return false;
}

SCEVCouldNotCompute::SCEVCouldNotCompute() :
  SCEV(FoldingSetNodeID(), scCouldNotCompute) {}

bool SCEVCouldNotCompute::isLoopInvariant(const Loop *L) const {
  LLVM_UNREACHABLE("Attempt to use a SCEVCouldNotCompute object!");
  return false;
}

const Type *SCEVCouldNotCompute::getType() const {
  LLVM_UNREACHABLE("Attempt to use a SCEVCouldNotCompute object!");
  return 0;
}

bool SCEVCouldNotCompute::hasComputableLoopEvolution(const Loop *L) const {
  LLVM_UNREACHABLE("Attempt to use a SCEVCouldNotCompute object!");
  return false;
}

const SCEV *
SCEVCouldNotCompute::replaceSymbolicValuesWithConcrete(
                                                    const SCEV *Sym,
                                                    const SCEV *Conc,
                                                    ScalarEvolution &SE) const {
  return this;
}

void SCEVCouldNotCompute::print(raw_ostream &OS) const {
  OS << "***COULDNOTCOMPUTE***";
}

bool SCEVCouldNotCompute::classof(const SCEV *S) {
  return S->getSCEVType() == scCouldNotCompute;
}

const SCEV *ScalarEvolution::getConstant(ConstantInt *V) {
  FoldingSetNodeID ID;
  ID.AddInteger(scConstant);
  ID.AddPointer(V);
  void *IP = 0;
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;
  SCEV *S = SCEVAllocator.Allocate<SCEVConstant>();
  new (S) SCEVConstant(ID, V);
  UniqueSCEVs.InsertNode(S, IP);
  return S;
}

const SCEV *ScalarEvolution::getConstant(const APInt& Val) {
  return getConstant(ConstantInt::get(Val));
}

const SCEV *
ScalarEvolution::getConstant(const Type *Ty, uint64_t V, bool isSigned) {
  return getConstant(ConstantInt::get(cast<IntegerType>(Ty), V, isSigned));
}

const Type *SCEVConstant::getType() const { return V->getType(); }

void SCEVConstant::print(raw_ostream &OS) const {
  WriteAsOperand(OS, V, false);
}

SCEVCastExpr::SCEVCastExpr(const FoldingSetNodeID &ID,
                           unsigned SCEVTy, const SCEV *op, const Type *ty)
  : SCEV(ID, SCEVTy), Op(op), Ty(ty) {}

bool SCEVCastExpr::dominates(BasicBlock *BB, DominatorTree *DT) const {
  return Op->dominates(BB, DT);
}

SCEVTruncateExpr::SCEVTruncateExpr(const FoldingSetNodeID &ID,
                                   const SCEV *op, const Type *ty)
  : SCEVCastExpr(ID, scTruncate, op, ty) {
  assert((Op->getType()->isInteger() || isa<PointerType>(Op->getType())) &&
         (Ty->isInteger() || isa<PointerType>(Ty)) &&
         "Cannot truncate non-integer value!");
}

void SCEVTruncateExpr::print(raw_ostream &OS) const {
  OS << "(trunc " << *Op->getType() << " " << *Op << " to " << *Ty << ")";
}

SCEVZeroExtendExpr::SCEVZeroExtendExpr(const FoldingSetNodeID &ID,
                                       const SCEV *op, const Type *ty)
  : SCEVCastExpr(ID, scZeroExtend, op, ty) {
  assert((Op->getType()->isInteger() || isa<PointerType>(Op->getType())) &&
         (Ty->isInteger() || isa<PointerType>(Ty)) &&
         "Cannot zero extend non-integer value!");
}

void SCEVZeroExtendExpr::print(raw_ostream &OS) const {
  OS << "(zext " << *Op->getType() << " " << *Op << " to " << *Ty << ")";
}

SCEVSignExtendExpr::SCEVSignExtendExpr(const FoldingSetNodeID &ID,
                                       const SCEV *op, const Type *ty)
  : SCEVCastExpr(ID, scSignExtend, op, ty) {
  assert((Op->getType()->isInteger() || isa<PointerType>(Op->getType())) &&
         (Ty->isInteger() || isa<PointerType>(Ty)) &&
         "Cannot sign extend non-integer value!");
}

void SCEVSignExtendExpr::print(raw_ostream &OS) const {
  OS << "(sext " << *Op->getType() << " " << *Op << " to " << *Ty << ")";
}

void SCEVCommutativeExpr::print(raw_ostream &OS) const {
  assert(Operands.size() > 1 && "This plus expr shouldn't exist!");
  const char *OpStr = getOperationStr();
  OS << "(" << *Operands[0];
  for (unsigned i = 1, e = Operands.size(); i != e; ++i)
    OS << OpStr << *Operands[i];
  OS << ")";
}

const SCEV *
SCEVCommutativeExpr::replaceSymbolicValuesWithConcrete(
                                                    const SCEV *Sym,
                                                    const SCEV *Conc,
                                                    ScalarEvolution &SE) const {
  for (unsigned i = 0, e = getNumOperands(); i != e; ++i) {
    const SCEV *H =
      getOperand(i)->replaceSymbolicValuesWithConcrete(Sym, Conc, SE);
    if (H != getOperand(i)) {
      SmallVector<const SCEV *, 8> NewOps;
      NewOps.reserve(getNumOperands());
      for (unsigned j = 0; j != i; ++j)
        NewOps.push_back(getOperand(j));
      NewOps.push_back(H);
      for (++i; i != e; ++i)
        NewOps.push_back(getOperand(i)->
                         replaceSymbolicValuesWithConcrete(Sym, Conc, SE));

      if (isa<SCEVAddExpr>(this))
        return SE.getAddExpr(NewOps);
      else if (isa<SCEVMulExpr>(this))
        return SE.getMulExpr(NewOps);
      else if (isa<SCEVSMaxExpr>(this))
        return SE.getSMaxExpr(NewOps);
      else if (isa<SCEVUMaxExpr>(this))
        return SE.getUMaxExpr(NewOps);
      else
        LLVM_UNREACHABLE("Unknown commutative expr!");
    }
  }
  return this;
}

bool SCEVNAryExpr::dominates(BasicBlock *BB, DominatorTree *DT) const {
  for (unsigned i = 0, e = getNumOperands(); i != e; ++i) {
    if (!getOperand(i)->dominates(BB, DT))
      return false;
  }
  return true;
}

bool SCEVUDivExpr::dominates(BasicBlock *BB, DominatorTree *DT) const {
  return LHS->dominates(BB, DT) && RHS->dominates(BB, DT);
}

void SCEVUDivExpr::print(raw_ostream &OS) const {
  OS << "(" << *LHS << " /u " << *RHS << ")";
}

const Type *SCEVUDivExpr::getType() const {
  // In most cases the types of LHS and RHS will be the same, but in some
  // crazy cases one or the other may be a pointer. ScalarEvolution doesn't
  // depend on the type for correctness, but handling types carefully can
  // avoid extra casts in the SCEVExpander. The LHS is more likely to be
  // a pointer type than the RHS, so use the RHS' type here.
  return RHS->getType();
}

const SCEV *
SCEVAddRecExpr::replaceSymbolicValuesWithConcrete(const SCEV *Sym,
                                                  const SCEV *Conc,
                                                  ScalarEvolution &SE) const {
  for (unsigned i = 0, e = getNumOperands(); i != e; ++i) {
    const SCEV *H =
      getOperand(i)->replaceSymbolicValuesWithConcrete(Sym, Conc, SE);
    if (H != getOperand(i)) {
      SmallVector<const SCEV *, 8> NewOps;
      NewOps.reserve(getNumOperands());
      for (unsigned j = 0; j != i; ++j)
        NewOps.push_back(getOperand(j));
      NewOps.push_back(H);
      for (++i; i != e; ++i)
        NewOps.push_back(getOperand(i)->
                         replaceSymbolicValuesWithConcrete(Sym, Conc, SE));

      return SE.getAddRecExpr(NewOps, L);
    }
  }
  return this;
}


bool SCEVAddRecExpr::isLoopInvariant(const Loop *QueryLoop) const {
  // Add recurrences are never invariant in the function-body (null loop).
  if (!QueryLoop)
    return false;

  // This recurrence is variant w.r.t. QueryLoop if QueryLoop contains L.
  if (QueryLoop->contains(L->getHeader()))
    return false;

  // This recurrence is variant w.r.t. QueryLoop if any of its operands
  // are variant.
  for (unsigned i = 0, e = getNumOperands(); i != e; ++i)
    if (!getOperand(i)->isLoopInvariant(QueryLoop))
      return false;

  // Otherwise it's loop-invariant.
  return true;
}

void SCEVAddRecExpr::print(raw_ostream &OS) const {
  OS << "{" << *Operands[0];
  for (unsigned i = 1, e = Operands.size(); i != e; ++i)
    OS << ",+," << *Operands[i];
  OS << "}<" << L->getHeader()->getName() + ">";
}

bool SCEVUnknown::isLoopInvariant(const Loop *L) const {
  // All non-instruction values are loop invariant.  All instructions are loop
  // invariant if they are not contained in the specified loop.
  // Instructions are never considered invariant in the function body
  // (null loop) because they are defined within the "loop".
  if (Instruction *I = dyn_cast<Instruction>(V))
    return L && !L->contains(I->getParent());
  return true;
}

bool SCEVUnknown::dominates(BasicBlock *BB, DominatorTree *DT) const {
  if (Instruction *I = dyn_cast<Instruction>(getValue()))
    return DT->dominates(I->getParent(), BB);
  return true;
}

const Type *SCEVUnknown::getType() const {
  return V->getType();
}

void SCEVUnknown::print(raw_ostream &OS) const {
  WriteAsOperand(OS, V, false);
}

//===----------------------------------------------------------------------===//
//                               SCEV Utilities
//===----------------------------------------------------------------------===//

namespace {
  /// SCEVComplexityCompare - Return true if the complexity of the LHS is less
  /// than the complexity of the RHS.  This comparator is used to canonicalize
  /// expressions.
  class VISIBILITY_HIDDEN SCEVComplexityCompare {
    LoopInfo *LI;
  public:
    explicit SCEVComplexityCompare(LoopInfo *li) : LI(li) {}

    bool operator()(const SCEV *LHS, const SCEV *RHS) const {
      // Primarily, sort the SCEVs by their getSCEVType().
      if (LHS->getSCEVType() != RHS->getSCEVType())
        return LHS->getSCEVType() < RHS->getSCEVType();

      // Aside from the getSCEVType() ordering, the particular ordering
      // isn't very important except that it's beneficial to be consistent,
      // so that (a + b) and (b + a) don't end up as different expressions.

      // Sort SCEVUnknown values with some loose heuristics. TODO: This is
      // not as complete as it could be.
      if (const SCEVUnknown *LU = dyn_cast<SCEVUnknown>(LHS)) {
        const SCEVUnknown *RU = cast<SCEVUnknown>(RHS);

        // Order pointer values after integer values. This helps SCEVExpander
        // form GEPs.
        if (isa<PointerType>(LU->getType()) && !isa<PointerType>(RU->getType()))
          return false;
        if (isa<PointerType>(RU->getType()) && !isa<PointerType>(LU->getType()))
          return true;

        // Compare getValueID values.
        if (LU->getValue()->getValueID() != RU->getValue()->getValueID())
          return LU->getValue()->getValueID() < RU->getValue()->getValueID();

        // Sort arguments by their position.
        if (const Argument *LA = dyn_cast<Argument>(LU->getValue())) {
          const Argument *RA = cast<Argument>(RU->getValue());
          return LA->getArgNo() < RA->getArgNo();
        }

        // For instructions, compare their loop depth, and their opcode.
        // This is pretty loose.
        if (Instruction *LV = dyn_cast<Instruction>(LU->getValue())) {
          Instruction *RV = cast<Instruction>(RU->getValue());

          // Compare loop depths.
          if (LI->getLoopDepth(LV->getParent()) !=
              LI->getLoopDepth(RV->getParent()))
            return LI->getLoopDepth(LV->getParent()) <
                   LI->getLoopDepth(RV->getParent());

          // Compare opcodes.
          if (LV->getOpcode() != RV->getOpcode())
            return LV->getOpcode() < RV->getOpcode();

          // Compare the number of operands.
          if (LV->getNumOperands() != RV->getNumOperands())
            return LV->getNumOperands() < RV->getNumOperands();
        }

        return false;
      }

      // Compare constant values.
      if (const SCEVConstant *LC = dyn_cast<SCEVConstant>(LHS)) {
        const SCEVConstant *RC = cast<SCEVConstant>(RHS);
        if (LC->getValue()->getBitWidth() != RC->getValue()->getBitWidth())
          return LC->getValue()->getBitWidth() < RC->getValue()->getBitWidth();
        return LC->getValue()->getValue().ult(RC->getValue()->getValue());
      }

      // Compare addrec loop depths.
      if (const SCEVAddRecExpr *LA = dyn_cast<SCEVAddRecExpr>(LHS)) {
        const SCEVAddRecExpr *RA = cast<SCEVAddRecExpr>(RHS);
        if (LA->getLoop()->getLoopDepth() != RA->getLoop()->getLoopDepth())
          return LA->getLoop()->getLoopDepth() < RA->getLoop()->getLoopDepth();
      }

      // Lexicographically compare n-ary expressions.
      if (const SCEVNAryExpr *LC = dyn_cast<SCEVNAryExpr>(LHS)) {
        const SCEVNAryExpr *RC = cast<SCEVNAryExpr>(RHS);
        for (unsigned i = 0, e = LC->getNumOperands(); i != e; ++i) {
          if (i >= RC->getNumOperands())
            return false;
          if (operator()(LC->getOperand(i), RC->getOperand(i)))
            return true;
          if (operator()(RC->getOperand(i), LC->getOperand(i)))
            return false;
        }
        return LC->getNumOperands() < RC->getNumOperands();
      }

      // Lexicographically compare udiv expressions.
      if (const SCEVUDivExpr *LC = dyn_cast<SCEVUDivExpr>(LHS)) {
        const SCEVUDivExpr *RC = cast<SCEVUDivExpr>(RHS);
        if (operator()(LC->getLHS(), RC->getLHS()))
          return true;
        if (operator()(RC->getLHS(), LC->getLHS()))
          return false;
        if (operator()(LC->getRHS(), RC->getRHS()))
          return true;
        if (operator()(RC->getRHS(), LC->getRHS()))
          return false;
        return false;
      }

      // Compare cast expressions by operand.
      if (const SCEVCastExpr *LC = dyn_cast<SCEVCastExpr>(LHS)) {
        const SCEVCastExpr *RC = cast<SCEVCastExpr>(RHS);
        return operator()(LC->getOperand(), RC->getOperand());
      }

      LLVM_UNREACHABLE("Unknown SCEV kind!");
      return false;
    }
  };
}

/// GroupByComplexity - Given a list of SCEV objects, order them by their
/// complexity, and group objects of the same complexity together by value.
/// When this routine is finished, we know that any duplicates in the vector are
/// consecutive and that complexity is monotonically increasing.
///
/// Note that we go take special precautions to ensure that we get determinstic
/// results from this routine.  In other words, we don't want the results of
/// this to depend on where the addresses of various SCEV objects happened to
/// land in memory.
///
static void GroupByComplexity(SmallVectorImpl<const SCEV *> &Ops,
                              LoopInfo *LI) {
  if (Ops.size() < 2) return;  // Noop
  if (Ops.size() == 2) {
    // This is the common case, which also happens to be trivially simple.
    // Special case it.
    if (SCEVComplexityCompare(LI)(Ops[1], Ops[0]))
      std::swap(Ops[0], Ops[1]);
    return;
  }

  // Do the rough sort by complexity.
  std::stable_sort(Ops.begin(), Ops.end(), SCEVComplexityCompare(LI));

  // Now that we are sorted by complexity, group elements of the same
  // complexity.  Note that this is, at worst, N^2, but the vector is likely to
  // be extremely short in practice.  Note that we take this approach because we
  // do not want to depend on the addresses of the objects we are grouping.
  for (unsigned i = 0, e = Ops.size(); i != e-2; ++i) {
    const SCEV *S = Ops[i];
    unsigned Complexity = S->getSCEVType();

    // If there are any objects of the same complexity and same value as this
    // one, group them.
    for (unsigned j = i+1; j != e && Ops[j]->getSCEVType() == Complexity; ++j) {
      if (Ops[j] == S) { // Found a duplicate.
        // Move it to immediately after i'th element.
        std::swap(Ops[i+1], Ops[j]);
        ++i;   // no need to rescan it.
        if (i == e-2) return;  // Done!
      }
    }
  }
}



//===----------------------------------------------------------------------===//
//                      Simple SCEV method implementations
//===----------------------------------------------------------------------===//

/// BinomialCoefficient - Compute BC(It, K).  The result has width W.
/// Assume, K > 0.
static const SCEV *BinomialCoefficient(const SCEV *It, unsigned K,
                                      ScalarEvolution &SE,
                                      const Type* ResultTy) {
  // Handle the simplest case efficiently.
  if (K == 1)
    return SE.getTruncateOrZeroExtend(It, ResultTy);

  // We are using the following formula for BC(It, K):
  //
  //   BC(It, K) = (It * (It - 1) * ... * (It - K + 1)) / K!
  //
  // Suppose, W is the bitwidth of the return value.  We must be prepared for
  // overflow.  Hence, we must assure that the result of our computation is
  // equal to the accurate one modulo 2^W.  Unfortunately, division isn't
  // safe in modular arithmetic.
  //
  // However, this code doesn't use exactly that formula; the formula it uses
  // is something like the following, where T is the number of factors of 2 in
  // K! (i.e. trailing zeros in the binary representation of K!), and ^ is
  // exponentiation:
  //
  //   BC(It, K) = (It * (It - 1) * ... * (It - K + 1)) / 2^T / (K! / 2^T)
  //
  // This formula is trivially equivalent to the previous formula.  However,
  // this formula can be implemented much more efficiently.  The trick is that
  // K! / 2^T is odd, and exact division by an odd number *is* safe in modular
  // arithmetic.  To do exact division in modular arithmetic, all we have
  // to do is multiply by the inverse.  Therefore, this step can be done at
  // width W.
  //
  // The next issue is how to safely do the division by 2^T.  The way this
  // is done is by doing the multiplication step at a width of at least W + T
  // bits.  This way, the bottom W+T bits of the product are accurate. Then,
  // when we perform the division by 2^T (which is equivalent to a right shift
  // by T), the bottom W bits are accurate.  Extra bits are okay; they'll get
  // truncated out after the division by 2^T.
  //
  // In comparison to just directly using the first formula, this technique
  // is much more efficient; using the first formula requires W * K bits,
  // but this formula less than W + K bits. Also, the first formula requires
  // a division step, whereas this formula only requires multiplies and shifts.
  //
  // It doesn't matter whether the subtraction step is done in the calculation
  // width or the input iteration count's width; if the subtraction overflows,
  // the result must be zero anyway.  We prefer here to do it in the width of
  // the induction variable because it helps a lot for certain cases; CodeGen
  // isn't smart enough to ignore the overflow, which leads to much less
  // efficient code if the width of the subtraction is wider than the native
  // register width.
  //
  // (It's possible to not widen at all by pulling out factors of 2 before
  // the multiplication; for example, K=2 can be calculated as
  // It/2*(It+(It*INT_MIN/INT_MIN)+-1). However, it requires
  // extra arithmetic, so it's not an obvious win, and it gets
  // much more complicated for K > 3.)

  // Protection from insane SCEVs; this bound is conservative,
  // but it probably doesn't matter.
  if (K > 1000)
    return SE.getCouldNotCompute();

  unsigned W = SE.getTypeSizeInBits(ResultTy);

  // Calculate K! / 2^T and T; we divide out the factors of two before
  // multiplying for calculating K! / 2^T to avoid overflow.
  // Other overflow doesn't matter because we only care about the bottom
  // W bits of the result.
  APInt OddFactorial(W, 1);
  unsigned T = 1;
  for (unsigned i = 3; i <= K; ++i) {
    APInt Mult(W, i);
    unsigned TwoFactors = Mult.countTrailingZeros();
    T += TwoFactors;
    Mult = Mult.lshr(TwoFactors);
    OddFactorial *= Mult;
  }

  // We need at least W + T bits for the multiplication step
  unsigned CalculationBits = W + T;

  // Calcuate 2^T, at width T+W.
  APInt DivFactor = APInt(CalculationBits, 1).shl(T);

  // Calculate the multiplicative inverse of K! / 2^T;
  // this multiplication factor will perform the exact division by
  // K! / 2^T.
  APInt Mod = APInt::getSignedMinValue(W+1);
  APInt MultiplyFactor = OddFactorial.zext(W+1);
  MultiplyFactor = MultiplyFactor.multiplicativeInverse(Mod);
  MultiplyFactor = MultiplyFactor.trunc(W);

  // Calculate the product, at width T+W
  const IntegerType *CalculationTy = IntegerType::get(CalculationBits);
  const SCEV *Dividend = SE.getTruncateOrZeroExtend(It, CalculationTy);
  for (unsigned i = 1; i != K; ++i) {
    const SCEV *S = SE.getMinusSCEV(It, SE.getIntegerSCEV(i, It->getType()));
    Dividend = SE.getMulExpr(Dividend,
                             SE.getTruncateOrZeroExtend(S, CalculationTy));
  }

  // Divide by 2^T
  const SCEV *DivResult = SE.getUDivExpr(Dividend, SE.getConstant(DivFactor));

  // Truncate the result, and divide by K! / 2^T.

  return SE.getMulExpr(SE.getConstant(MultiplyFactor),
                       SE.getTruncateOrZeroExtend(DivResult, ResultTy));
}

/// evaluateAtIteration - Return the value of this chain of recurrences at
/// the specified iteration number.  We can evaluate this recurrence by
/// multiplying each element in the chain by the binomial coefficient
/// corresponding to it.  In other words, we can evaluate {A,+,B,+,C,+,D} as:
///
///   A*BC(It, 0) + B*BC(It, 1) + C*BC(It, 2) + D*BC(It, 3)
///
/// where BC(It, k) stands for binomial coefficient.
///
const SCEV *SCEVAddRecExpr::evaluateAtIteration(const SCEV *It,
                                               ScalarEvolution &SE) const {
  const SCEV *Result = getStart();
  for (unsigned i = 1, e = getNumOperands(); i != e; ++i) {
    // The computation is correct in the face of overflow provided that the
    // multiplication is performed _after_ the evaluation of the binomial
    // coefficient.
    const SCEV *Coeff = BinomialCoefficient(It, i, SE, getType());
    if (isa<SCEVCouldNotCompute>(Coeff))
      return Coeff;

    Result = SE.getAddExpr(Result, SE.getMulExpr(getOperand(i), Coeff));
  }
  return Result;
}

//===----------------------------------------------------------------------===//
//                    SCEV Expression folder implementations
//===----------------------------------------------------------------------===//

const SCEV *ScalarEvolution::getTruncateExpr(const SCEV *Op,
                                            const Type *Ty) {
  assert(getTypeSizeInBits(Op->getType()) > getTypeSizeInBits(Ty) &&
         "This is not a truncating conversion!");
  assert(isSCEVable(Ty) &&
         "This is not a conversion to a SCEVable type!");
  Ty = getEffectiveSCEVType(Ty);

  FoldingSetNodeID ID;
  ID.AddInteger(scTruncate);
  ID.AddPointer(Op);
  ID.AddPointer(Ty);
  void *IP = 0;
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;

  // Fold if the operand is constant.
  if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(Op))
    return getConstant(
      cast<ConstantInt>(ConstantExpr::getTrunc(SC->getValue(), Ty)));

  // trunc(trunc(x)) --> trunc(x)
  if (const SCEVTruncateExpr *ST = dyn_cast<SCEVTruncateExpr>(Op))
    return getTruncateExpr(ST->getOperand(), Ty);

  // trunc(sext(x)) --> sext(x) if widening or trunc(x) if narrowing
  if (const SCEVSignExtendExpr *SS = dyn_cast<SCEVSignExtendExpr>(Op))
    return getTruncateOrSignExtend(SS->getOperand(), Ty);

  // trunc(zext(x)) --> zext(x) if widening or trunc(x) if narrowing
  if (const SCEVZeroExtendExpr *SZ = dyn_cast<SCEVZeroExtendExpr>(Op))
    return getTruncateOrZeroExtend(SZ->getOperand(), Ty);

  // If the input value is a chrec scev, truncate the chrec's operands.
  if (const SCEVAddRecExpr *AddRec = dyn_cast<SCEVAddRecExpr>(Op)) {
    SmallVector<const SCEV *, 4> Operands;
    for (unsigned i = 0, e = AddRec->getNumOperands(); i != e; ++i)
      Operands.push_back(getTruncateExpr(AddRec->getOperand(i), Ty));
    return getAddRecExpr(Operands, AddRec->getLoop());
  }

  // The cast wasn't folded; create an explicit cast node.
  // Recompute the insert position, as it may have been invalidated.
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;
  SCEV *S = SCEVAllocator.Allocate<SCEVTruncateExpr>();
  new (S) SCEVTruncateExpr(ID, Op, Ty);
  UniqueSCEVs.InsertNode(S, IP);
  return S;
}

const SCEV *ScalarEvolution::getZeroExtendExpr(const SCEV *Op,
                                              const Type *Ty) {
  assert(getTypeSizeInBits(Op->getType()) < getTypeSizeInBits(Ty) &&
         "This is not an extending conversion!");
  assert(isSCEVable(Ty) &&
         "This is not a conversion to a SCEVable type!");
  Ty = getEffectiveSCEVType(Ty);

  // Fold if the operand is constant.
  if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(Op)) {
    const Type *IntTy = getEffectiveSCEVType(Ty);
    Constant *C = ConstantExpr::getZExt(SC->getValue(), IntTy);
    if (IntTy != Ty) C = ConstantExpr::getIntToPtr(C, Ty);
    return getConstant(cast<ConstantInt>(C));
  }

  // zext(zext(x)) --> zext(x)
  if (const SCEVZeroExtendExpr *SZ = dyn_cast<SCEVZeroExtendExpr>(Op))
    return getZeroExtendExpr(SZ->getOperand(), Ty);

  // Before doing any expensive analysis, check to see if we've already
  // computed a SCEV for this Op and Ty.
  FoldingSetNodeID ID;
  ID.AddInteger(scZeroExtend);
  ID.AddPointer(Op);
  ID.AddPointer(Ty);
  void *IP = 0;
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;

  // If the input value is a chrec scev, and we can prove that the value
  // did not overflow the old, smaller, value, we can zero extend all of the
  // operands (often constants).  This allows analysis of something like
  // this:  for (unsigned char X = 0; X < 100; ++X) { int Y = X; }
  if (const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(Op))
    if (AR->isAffine()) {
      // Check whether the backedge-taken count is SCEVCouldNotCompute.
      // Note that this serves two purposes: It filters out loops that are
      // simply not analyzable, and it covers the case where this code is
      // being called from within backedge-taken count analysis, such that
      // attempting to ask for the backedge-taken count would likely result
      // in infinite recursion. In the later case, the analysis code will
      // cope with a conservative value, and it will take care to purge
      // that value once it has finished.
      const SCEV *MaxBECount = getMaxBackedgeTakenCount(AR->getLoop());
      if (!isa<SCEVCouldNotCompute>(MaxBECount)) {
        // Manually compute the final value for AR, checking for
        // overflow.
        const SCEV *Start = AR->getStart();
        const SCEV *Step = AR->getStepRecurrence(*this);

        // Check whether the backedge-taken count can be losslessly casted to
        // the addrec's type. The count is always unsigned.
        const SCEV *CastedMaxBECount =
          getTruncateOrZeroExtend(MaxBECount, Start->getType());
        const SCEV *RecastedMaxBECount =
          getTruncateOrZeroExtend(CastedMaxBECount, MaxBECount->getType());
        if (MaxBECount == RecastedMaxBECount) {
          const Type *WideTy =
            IntegerType::get(getTypeSizeInBits(Start->getType()) * 2);
          // Check whether Start+Step*MaxBECount has no unsigned overflow.
          const SCEV *ZMul =
            getMulExpr(CastedMaxBECount,
                       getTruncateOrZeroExtend(Step, Start->getType()));
          const SCEV *Add = getAddExpr(Start, ZMul);
          const SCEV *OperandExtendedAdd =
            getAddExpr(getZeroExtendExpr(Start, WideTy),
                       getMulExpr(getZeroExtendExpr(CastedMaxBECount, WideTy),
                                  getZeroExtendExpr(Step, WideTy)));
          if (getZeroExtendExpr(Add, WideTy) == OperandExtendedAdd)
            // Return the expression with the addrec on the outside.
            return getAddRecExpr(getZeroExtendExpr(Start, Ty),
                                 getZeroExtendExpr(Step, Ty),
                                 AR->getLoop());

          // Similar to above, only this time treat the step value as signed.
          // This covers loops that count down.
          const SCEV *SMul =
            getMulExpr(CastedMaxBECount,
                       getTruncateOrSignExtend(Step, Start->getType()));
          Add = getAddExpr(Start, SMul);
          OperandExtendedAdd =
            getAddExpr(getZeroExtendExpr(Start, WideTy),
                       getMulExpr(getZeroExtendExpr(CastedMaxBECount, WideTy),
                                  getSignExtendExpr(Step, WideTy)));
          if (getZeroExtendExpr(Add, WideTy) == OperandExtendedAdd)
            // Return the expression with the addrec on the outside.
            return getAddRecExpr(getZeroExtendExpr(Start, Ty),
                                 getSignExtendExpr(Step, Ty),
                                 AR->getLoop());
        }
      }
    }

  // The cast wasn't folded; create an explicit cast node.
  // Recompute the insert position, as it may have been invalidated.
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;
  SCEV *S = SCEVAllocator.Allocate<SCEVZeroExtendExpr>();
  new (S) SCEVZeroExtendExpr(ID, Op, Ty);
  UniqueSCEVs.InsertNode(S, IP);
  return S;
}

const SCEV *ScalarEvolution::getSignExtendExpr(const SCEV *Op,
                                              const Type *Ty) {
  assert(getTypeSizeInBits(Op->getType()) < getTypeSizeInBits(Ty) &&
         "This is not an extending conversion!");
  assert(isSCEVable(Ty) &&
         "This is not a conversion to a SCEVable type!");
  Ty = getEffectiveSCEVType(Ty);

  // Fold if the operand is constant.
  if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(Op)) {
    const Type *IntTy = getEffectiveSCEVType(Ty);
    Constant *C = ConstantExpr::getSExt(SC->getValue(), IntTy);
    if (IntTy != Ty) C = ConstantExpr::getIntToPtr(C, Ty);
    return getConstant(cast<ConstantInt>(C));
  }

  // sext(sext(x)) --> sext(x)
  if (const SCEVSignExtendExpr *SS = dyn_cast<SCEVSignExtendExpr>(Op))
    return getSignExtendExpr(SS->getOperand(), Ty);

  // Before doing any expensive analysis, check to see if we've already
  // computed a SCEV for this Op and Ty.
  FoldingSetNodeID ID;
  ID.AddInteger(scSignExtend);
  ID.AddPointer(Op);
  ID.AddPointer(Ty);
  void *IP = 0;
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;

  // If the input value is a chrec scev, and we can prove that the value
  // did not overflow the old, smaller, value, we can sign extend all of the
  // operands (often constants).  This allows analysis of something like
  // this:  for (signed char X = 0; X < 100; ++X) { int Y = X; }
  if (const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(Op))
    if (AR->isAffine()) {
      // Check whether the backedge-taken count is SCEVCouldNotCompute.
      // Note that this serves two purposes: It filters out loops that are
      // simply not analyzable, and it covers the case where this code is
      // being called from within backedge-taken count analysis, such that
      // attempting to ask for the backedge-taken count would likely result
      // in infinite recursion. In the later case, the analysis code will
      // cope with a conservative value, and it will take care to purge
      // that value once it has finished.
      const SCEV *MaxBECount = getMaxBackedgeTakenCount(AR->getLoop());
      if (!isa<SCEVCouldNotCompute>(MaxBECount)) {
        // Manually compute the final value for AR, checking for
        // overflow.
        const SCEV *Start = AR->getStart();
        const SCEV *Step = AR->getStepRecurrence(*this);

        // Check whether the backedge-taken count can be losslessly casted to
        // the addrec's type. The count is always unsigned.
        const SCEV *CastedMaxBECount =
          getTruncateOrZeroExtend(MaxBECount, Start->getType());
        const SCEV *RecastedMaxBECount =
          getTruncateOrZeroExtend(CastedMaxBECount, MaxBECount->getType());
        if (MaxBECount == RecastedMaxBECount) {
          const Type *WideTy =
            IntegerType::get(getTypeSizeInBits(Start->getType()) * 2);
          // Check whether Start+Step*MaxBECount has no signed overflow.
          const SCEV *SMul =
            getMulExpr(CastedMaxBECount,
                       getTruncateOrSignExtend(Step, Start->getType()));
          const SCEV *Add = getAddExpr(Start, SMul);
          const SCEV *OperandExtendedAdd =
            getAddExpr(getSignExtendExpr(Start, WideTy),
                       getMulExpr(getZeroExtendExpr(CastedMaxBECount, WideTy),
                                  getSignExtendExpr(Step, WideTy)));
          if (getSignExtendExpr(Add, WideTy) == OperandExtendedAdd)
            // Return the expression with the addrec on the outside.
            return getAddRecExpr(getSignExtendExpr(Start, Ty),
                                 getSignExtendExpr(Step, Ty),
                                 AR->getLoop());
        }
      }
    }

  // The cast wasn't folded; create an explicit cast node.
  // Recompute the insert position, as it may have been invalidated.
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;
  SCEV *S = SCEVAllocator.Allocate<SCEVSignExtendExpr>();
  new (S) SCEVSignExtendExpr(ID, Op, Ty);
  UniqueSCEVs.InsertNode(S, IP);
  return S;
}

/// getAnyExtendExpr - Return a SCEV for the given operand extended with
/// unspecified bits out to the given type.
///
const SCEV *ScalarEvolution::getAnyExtendExpr(const SCEV *Op,
                                             const Type *Ty) {
  assert(getTypeSizeInBits(Op->getType()) < getTypeSizeInBits(Ty) &&
         "This is not an extending conversion!");
  assert(isSCEVable(Ty) &&
         "This is not a conversion to a SCEVable type!");
  Ty = getEffectiveSCEVType(Ty);

  // Sign-extend negative constants.
  if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(Op))
    if (SC->getValue()->getValue().isNegative())
      return getSignExtendExpr(Op, Ty);

  // Peel off a truncate cast.
  if (const SCEVTruncateExpr *T = dyn_cast<SCEVTruncateExpr>(Op)) {
    const SCEV *NewOp = T->getOperand();
    if (getTypeSizeInBits(NewOp->getType()) < getTypeSizeInBits(Ty))
      return getAnyExtendExpr(NewOp, Ty);
    return getTruncateOrNoop(NewOp, Ty);
  }

  // Next try a zext cast. If the cast is folded, use it.
  const SCEV *ZExt = getZeroExtendExpr(Op, Ty);
  if (!isa<SCEVZeroExtendExpr>(ZExt))
    return ZExt;

  // Next try a sext cast. If the cast is folded, use it.
  const SCEV *SExt = getSignExtendExpr(Op, Ty);
  if (!isa<SCEVSignExtendExpr>(SExt))
    return SExt;

  // If the expression is obviously signed, use the sext cast value.
  if (isa<SCEVSMaxExpr>(Op))
    return SExt;

  // Absent any other information, use the zext cast value.
  return ZExt;
}

/// CollectAddOperandsWithScales - Process the given Ops list, which is
/// a list of operands to be added under the given scale, update the given
/// map. This is a helper function for getAddRecExpr. As an example of
/// what it does, given a sequence of operands that would form an add
/// expression like this:
///
///    m + n + 13 + (A * (o + p + (B * q + m + 29))) + r + (-1 * r)
///
/// where A and B are constants, update the map with these values:
///
///    (m, 1+A*B), (n, 1), (o, A), (p, A), (q, A*B), (r, 0)
///
/// and add 13 + A*B*29 to AccumulatedConstant.
/// This will allow getAddRecExpr to produce this:
///
///    13+A*B*29 + n + (m * (1+A*B)) + ((o + p) * A) + (q * A*B)
///
/// This form often exposes folding opportunities that are hidden in
/// the original operand list.
///
/// Return true iff it appears that any interesting folding opportunities
/// may be exposed. This helps getAddRecExpr short-circuit extra work in
/// the common case where no interesting opportunities are present, and
/// is also used as a check to avoid infinite recursion.
///
static bool
CollectAddOperandsWithScales(DenseMap<const SCEV *, APInt> &M,
                             SmallVector<const SCEV *, 8> &NewOps,
                             APInt &AccumulatedConstant,
                             const SmallVectorImpl<const SCEV *> &Ops,
                             const APInt &Scale,
                             ScalarEvolution &SE) {
  bool Interesting = false;

  // Iterate over the add operands.
  for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
    const SCEVMulExpr *Mul = dyn_cast<SCEVMulExpr>(Ops[i]);
    if (Mul && isa<SCEVConstant>(Mul->getOperand(0))) {
      APInt NewScale =
        Scale * cast<SCEVConstant>(Mul->getOperand(0))->getValue()->getValue();
      if (Mul->getNumOperands() == 2 && isa<SCEVAddExpr>(Mul->getOperand(1))) {
        // A multiplication of a constant with another add; recurse.
        Interesting |=
          CollectAddOperandsWithScales(M, NewOps, AccumulatedConstant,
                                       cast<SCEVAddExpr>(Mul->getOperand(1))
                                         ->getOperands(),
                                       NewScale, SE);
      } else {
        // A multiplication of a constant with some other value. Update
        // the map.
        SmallVector<const SCEV *, 4> MulOps(Mul->op_begin()+1, Mul->op_end());
        const SCEV *Key = SE.getMulExpr(MulOps);
        std::pair<DenseMap<const SCEV *, APInt>::iterator, bool> Pair =
          M.insert(std::make_pair(Key, NewScale));
        if (Pair.second) {
          NewOps.push_back(Pair.first->first);
        } else {
          Pair.first->second += NewScale;
          // The map already had an entry for this value, which may indicate
          // a folding opportunity.
          Interesting = true;
        }
      }
    } else if (const SCEVConstant *C = dyn_cast<SCEVConstant>(Ops[i])) {
      // Pull a buried constant out to the outside.
      if (Scale != 1 || AccumulatedConstant != 0 || C->isZero())
        Interesting = true;
      AccumulatedConstant += Scale * C->getValue()->getValue();
    } else {
      // An ordinary operand. Update the map.
      std::pair<DenseMap<const SCEV *, APInt>::iterator, bool> Pair =
        M.insert(std::make_pair(Ops[i], Scale));
      if (Pair.second) {
        NewOps.push_back(Pair.first->first);
      } else {
        Pair.first->second += Scale;
        // The map already had an entry for this value, which may indicate
        // a folding opportunity.
        Interesting = true;
      }
    }
  }

  return Interesting;
}

namespace {
  struct APIntCompare {
    bool operator()(const APInt &LHS, const APInt &RHS) const {
      return LHS.ult(RHS);
    }
  };
}

/// getAddExpr - Get a canonical add expression, or something simpler if
/// possible.
const SCEV *ScalarEvolution::getAddExpr(SmallVectorImpl<const SCEV *> &Ops) {
  assert(!Ops.empty() && "Cannot get empty add!");
  if (Ops.size() == 1) return Ops[0];
#ifndef NDEBUG
  for (unsigned i = 1, e = Ops.size(); i != e; ++i)
    assert(getEffectiveSCEVType(Ops[i]->getType()) ==
           getEffectiveSCEVType(Ops[0]->getType()) &&
           "SCEVAddExpr operand types don't match!");
#endif

  // Sort by complexity, this groups all similar expression types together.
  GroupByComplexity(Ops, LI);

  // If there are any constants, fold them together.
  unsigned Idx = 0;
  if (const SCEVConstant *LHSC = dyn_cast<SCEVConstant>(Ops[0])) {
    ++Idx;
    assert(Idx < Ops.size());
    while (const SCEVConstant *RHSC = dyn_cast<SCEVConstant>(Ops[Idx])) {
      // We found two constants, fold them together!
      Ops[0] = getConstant(LHSC->getValue()->getValue() +
                           RHSC->getValue()->getValue());
      if (Ops.size() == 2) return Ops[0];
      Ops.erase(Ops.begin()+1);  // Erase the folded element
      LHSC = cast<SCEVConstant>(Ops[0]);
    }

    // If we are left with a constant zero being added, strip it off.
    if (cast<SCEVConstant>(Ops[0])->getValue()->isZero()) {
      Ops.erase(Ops.begin());
      --Idx;
    }
  }

  if (Ops.size() == 1) return Ops[0];

  // Okay, check to see if the same value occurs in the operand list twice.  If
  // so, merge them together into an multiply expression.  Since we sorted the
  // list, these values are required to be adjacent.
  const Type *Ty = Ops[0]->getType();
  for (unsigned i = 0, e = Ops.size()-1; i != e; ++i)
    if (Ops[i] == Ops[i+1]) {      //  X + Y + Y  -->  X + Y*2
      // Found a match, merge the two values into a multiply, and add any
      // remaining values to the result.
      const SCEV *Two = getIntegerSCEV(2, Ty);
      const SCEV *Mul = getMulExpr(Ops[i], Two);
      if (Ops.size() == 2)
        return Mul;
      Ops.erase(Ops.begin()+i, Ops.begin()+i+2);
      Ops.push_back(Mul);
      return getAddExpr(Ops);
    }

  // Check for truncates. If all the operands are truncated from the same
  // type, see if factoring out the truncate would permit the result to be
  // folded. eg., trunc(x) + m*trunc(n) --> trunc(x + trunc(m)*n)
  // if the contents of the resulting outer trunc fold to something simple.
  for (; Idx < Ops.size() && isa<SCEVTruncateExpr>(Ops[Idx]); ++Idx) {
    const SCEVTruncateExpr *Trunc = cast<SCEVTruncateExpr>(Ops[Idx]);
    const Type *DstType = Trunc->getType();
    const Type *SrcType = Trunc->getOperand()->getType();
    SmallVector<const SCEV *, 8> LargeOps;
    bool Ok = true;
    // Check all the operands to see if they can be represented in the
    // source type of the truncate.
    for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
      if (const SCEVTruncateExpr *T = dyn_cast<SCEVTruncateExpr>(Ops[i])) {
        if (T->getOperand()->getType() != SrcType) {
          Ok = false;
          break;
        }
        LargeOps.push_back(T->getOperand());
      } else if (const SCEVConstant *C = dyn_cast<SCEVConstant>(Ops[i])) {
        // This could be either sign or zero extension, but sign extension
        // is much more likely to be foldable here.
        LargeOps.push_back(getSignExtendExpr(C, SrcType));
      } else if (const SCEVMulExpr *M = dyn_cast<SCEVMulExpr>(Ops[i])) {
        SmallVector<const SCEV *, 8> LargeMulOps;
        for (unsigned j = 0, f = M->getNumOperands(); j != f && Ok; ++j) {
          if (const SCEVTruncateExpr *T =
                dyn_cast<SCEVTruncateExpr>(M->getOperand(j))) {
            if (T->getOperand()->getType() != SrcType) {
              Ok = false;
              break;
            }
            LargeMulOps.push_back(T->getOperand());
          } else if (const SCEVConstant *C =
                       dyn_cast<SCEVConstant>(M->getOperand(j))) {
            // This could be either sign or zero extension, but sign extension
            // is much more likely to be foldable here.
            LargeMulOps.push_back(getSignExtendExpr(C, SrcType));
          } else {
            Ok = false;
            break;
          }
        }
        if (Ok)
          LargeOps.push_back(getMulExpr(LargeMulOps));
      } else {
        Ok = false;
        break;
      }
    }
    if (Ok) {
      // Evaluate the expression in the larger type.
      const SCEV *Fold = getAddExpr(LargeOps);
      // If it folds to something simple, use it. Otherwise, don't.
      if (isa<SCEVConstant>(Fold) || isa<SCEVUnknown>(Fold))
        return getTruncateExpr(Fold, DstType);
    }
  }

  // Skip past any other cast SCEVs.
  while (Idx < Ops.size() && Ops[Idx]->getSCEVType() < scAddExpr)
    ++Idx;

  // If there are add operands they would be next.
  if (Idx < Ops.size()) {
    bool DeletedAdd = false;
    while (const SCEVAddExpr *Add = dyn_cast<SCEVAddExpr>(Ops[Idx])) {
      // If we have an add, expand the add operands onto the end of the operands
      // list.
      Ops.insert(Ops.end(), Add->op_begin(), Add->op_end());
      Ops.erase(Ops.begin()+Idx);
      DeletedAdd = true;
    }

    // If we deleted at least one add, we added operands to the end of the list,
    // and they are not necessarily sorted.  Recurse to resort and resimplify
    // any operands we just aquired.
    if (DeletedAdd)
      return getAddExpr(Ops);
  }

  // Skip over the add expression until we get to a multiply.
  while (Idx < Ops.size() && Ops[Idx]->getSCEVType() < scMulExpr)
    ++Idx;

  // Check to see if there are any folding opportunities present with
  // operands multiplied by constant values.
  if (Idx < Ops.size() && isa<SCEVMulExpr>(Ops[Idx])) {
    uint64_t BitWidth = getTypeSizeInBits(Ty);
    DenseMap<const SCEV *, APInt> M;
    SmallVector<const SCEV *, 8> NewOps;
    APInt AccumulatedConstant(BitWidth, 0);
    if (CollectAddOperandsWithScales(M, NewOps, AccumulatedConstant,
                                     Ops, APInt(BitWidth, 1), *this)) {
      // Some interesting folding opportunity is present, so its worthwhile to
      // re-generate the operands list. Group the operands by constant scale,
      // to avoid multiplying by the same constant scale multiple times.
      std::map<APInt, SmallVector<const SCEV *, 4>, APIntCompare> MulOpLists;
      for (SmallVector<const SCEV *, 8>::iterator I = NewOps.begin(),
           E = NewOps.end(); I != E; ++I)
        MulOpLists[M.find(*I)->second].push_back(*I);
      // Re-generate the operands list.
      Ops.clear();
      if (AccumulatedConstant != 0)
        Ops.push_back(getConstant(AccumulatedConstant));
      for (std::map<APInt, SmallVector<const SCEV *, 4>, APIntCompare>::iterator
           I = MulOpLists.begin(), E = MulOpLists.end(); I != E; ++I)
        if (I->first != 0)
          Ops.push_back(getMulExpr(getConstant(I->first),
                                   getAddExpr(I->second)));
      if (Ops.empty())
        return getIntegerSCEV(0, Ty);
      if (Ops.size() == 1)
        return Ops[0];
      return getAddExpr(Ops);
    }
  }

  // If we are adding something to a multiply expression, make sure the
  // something is not already an operand of the multiply.  If so, merge it into
  // the multiply.
  for (; Idx < Ops.size() && isa<SCEVMulExpr>(Ops[Idx]); ++Idx) {
    const SCEVMulExpr *Mul = cast<SCEVMulExpr>(Ops[Idx]);
    for (unsigned MulOp = 0, e = Mul->getNumOperands(); MulOp != e; ++MulOp) {
      const SCEV *MulOpSCEV = Mul->getOperand(MulOp);
      for (unsigned AddOp = 0, e = Ops.size(); AddOp != e; ++AddOp)
        if (MulOpSCEV == Ops[AddOp] && !isa<SCEVConstant>(Ops[AddOp])) {
          // Fold W + X + (X * Y * Z)  -->  W + (X * ((Y*Z)+1))
          const SCEV *InnerMul = Mul->getOperand(MulOp == 0);
          if (Mul->getNumOperands() != 2) {
            // If the multiply has more than two operands, we must get the
            // Y*Z term.
            SmallVector<const SCEV *, 4> MulOps(Mul->op_begin(), Mul->op_end());
            MulOps.erase(MulOps.begin()+MulOp);
            InnerMul = getMulExpr(MulOps);
          }
          const SCEV *One = getIntegerSCEV(1, Ty);
          const SCEV *AddOne = getAddExpr(InnerMul, One);
          const SCEV *OuterMul = getMulExpr(AddOne, Ops[AddOp]);
          if (Ops.size() == 2) return OuterMul;
          if (AddOp < Idx) {
            Ops.erase(Ops.begin()+AddOp);
            Ops.erase(Ops.begin()+Idx-1);
          } else {
            Ops.erase(Ops.begin()+Idx);
            Ops.erase(Ops.begin()+AddOp-1);
          }
          Ops.push_back(OuterMul);
          return getAddExpr(Ops);
        }

      // Check this multiply against other multiplies being added together.
      for (unsigned OtherMulIdx = Idx+1;
           OtherMulIdx < Ops.size() && isa<SCEVMulExpr>(Ops[OtherMulIdx]);
           ++OtherMulIdx) {
        const SCEVMulExpr *OtherMul = cast<SCEVMulExpr>(Ops[OtherMulIdx]);
        // If MulOp occurs in OtherMul, we can fold the two multiplies
        // together.
        for (unsigned OMulOp = 0, e = OtherMul->getNumOperands();
             OMulOp != e; ++OMulOp)
          if (OtherMul->getOperand(OMulOp) == MulOpSCEV) {
            // Fold X + (A*B*C) + (A*D*E) --> X + (A*(B*C+D*E))
            const SCEV *InnerMul1 = Mul->getOperand(MulOp == 0);
            if (Mul->getNumOperands() != 2) {
              SmallVector<const SCEV *, 4> MulOps(Mul->op_begin(),
                                                  Mul->op_end());
              MulOps.erase(MulOps.begin()+MulOp);
              InnerMul1 = getMulExpr(MulOps);
            }
            const SCEV *InnerMul2 = OtherMul->getOperand(OMulOp == 0);
            if (OtherMul->getNumOperands() != 2) {
              SmallVector<const SCEV *, 4> MulOps(OtherMul->op_begin(),
                                                  OtherMul->op_end());
              MulOps.erase(MulOps.begin()+OMulOp);
              InnerMul2 = getMulExpr(MulOps);
            }
            const SCEV *InnerMulSum = getAddExpr(InnerMul1,InnerMul2);
            const SCEV *OuterMul = getMulExpr(MulOpSCEV, InnerMulSum);
            if (Ops.size() == 2) return OuterMul;
            Ops.erase(Ops.begin()+Idx);
            Ops.erase(Ops.begin()+OtherMulIdx-1);
            Ops.push_back(OuterMul);
            return getAddExpr(Ops);
          }
      }
    }
  }

  // If there are any add recurrences in the operands list, see if any other
  // added values are loop invariant.  If so, we can fold them into the
  // recurrence.
  while (Idx < Ops.size() && Ops[Idx]->getSCEVType() < scAddRecExpr)
    ++Idx;

  // Scan over all recurrences, trying to fold loop invariants into them.
  for (; Idx < Ops.size() && isa<SCEVAddRecExpr>(Ops[Idx]); ++Idx) {
    // Scan all of the other operands to this add and add them to the vector if
    // they are loop invariant w.r.t. the recurrence.
    SmallVector<const SCEV *, 8> LIOps;
    const SCEVAddRecExpr *AddRec = cast<SCEVAddRecExpr>(Ops[Idx]);
    for (unsigned i = 0, e = Ops.size(); i != e; ++i)
      if (Ops[i]->isLoopInvariant(AddRec->getLoop())) {
        LIOps.push_back(Ops[i]);
        Ops.erase(Ops.begin()+i);
        --i; --e;
      }

    // If we found some loop invariants, fold them into the recurrence.
    if (!LIOps.empty()) {
      //  NLI + LI + {Start,+,Step}  -->  NLI + {LI+Start,+,Step}
      LIOps.push_back(AddRec->getStart());

      SmallVector<const SCEV *, 4> AddRecOps(AddRec->op_begin(),
                                           AddRec->op_end());
      AddRecOps[0] = getAddExpr(LIOps);

      const SCEV *NewRec = getAddRecExpr(AddRecOps, AddRec->getLoop());
      // If all of the other operands were loop invariant, we are done.
      if (Ops.size() == 1) return NewRec;

      // Otherwise, add the folded AddRec by the non-liv parts.
      for (unsigned i = 0;; ++i)
        if (Ops[i] == AddRec) {
          Ops[i] = NewRec;
          break;
        }
      return getAddExpr(Ops);
    }

    // Okay, if there weren't any loop invariants to be folded, check to see if
    // there are multiple AddRec's with the same loop induction variable being
    // added together.  If so, we can fold them.
    for (unsigned OtherIdx = Idx+1;
         OtherIdx < Ops.size() && isa<SCEVAddRecExpr>(Ops[OtherIdx]);++OtherIdx)
      if (OtherIdx != Idx) {
        const SCEVAddRecExpr *OtherAddRec = cast<SCEVAddRecExpr>(Ops[OtherIdx]);
        if (AddRec->getLoop() == OtherAddRec->getLoop()) {
          // Other + {A,+,B} + {C,+,D}  -->  Other + {A+C,+,B+D}
          SmallVector<const SCEV *, 4> NewOps(AddRec->op_begin(),
                                              AddRec->op_end());
          for (unsigned i = 0, e = OtherAddRec->getNumOperands(); i != e; ++i) {
            if (i >= NewOps.size()) {
              NewOps.insert(NewOps.end(), OtherAddRec->op_begin()+i,
                            OtherAddRec->op_end());
              break;
            }
            NewOps[i] = getAddExpr(NewOps[i], OtherAddRec->getOperand(i));
          }
          const SCEV *NewAddRec = getAddRecExpr(NewOps, AddRec->getLoop());

          if (Ops.size() == 2) return NewAddRec;

          Ops.erase(Ops.begin()+Idx);
          Ops.erase(Ops.begin()+OtherIdx-1);
          Ops.push_back(NewAddRec);
          return getAddExpr(Ops);
        }
      }

    // Otherwise couldn't fold anything into this recurrence.  Move onto the
    // next one.
  }

  // Okay, it looks like we really DO need an add expr.  Check to see if we
  // already have one, otherwise create a new one.
  FoldingSetNodeID ID;
  ID.AddInteger(scAddExpr);
  ID.AddInteger(Ops.size());
  for (unsigned i = 0, e = Ops.size(); i != e; ++i)
    ID.AddPointer(Ops[i]);
  void *IP = 0;
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;
  SCEV *S = SCEVAllocator.Allocate<SCEVAddExpr>();
  new (S) SCEVAddExpr(ID, Ops);
  UniqueSCEVs.InsertNode(S, IP);
  return S;
}


/// getMulExpr - Get a canonical multiply expression, or something simpler if
/// possible.
const SCEV *ScalarEvolution::getMulExpr(SmallVectorImpl<const SCEV *> &Ops) {
  assert(!Ops.empty() && "Cannot get empty mul!");
#ifndef NDEBUG
  for (unsigned i = 1, e = Ops.size(); i != e; ++i)
    assert(getEffectiveSCEVType(Ops[i]->getType()) ==
           getEffectiveSCEVType(Ops[0]->getType()) &&
           "SCEVMulExpr operand types don't match!");
#endif

  // Sort by complexity, this groups all similar expression types together.
  GroupByComplexity(Ops, LI);

  // If there are any constants, fold them together.
  unsigned Idx = 0;
  if (const SCEVConstant *LHSC = dyn_cast<SCEVConstant>(Ops[0])) {

    // C1*(C2+V) -> C1*C2 + C1*V
    if (Ops.size() == 2)
      if (const SCEVAddExpr *Add = dyn_cast<SCEVAddExpr>(Ops[1]))
        if (Add->getNumOperands() == 2 &&
            isa<SCEVConstant>(Add->getOperand(0)))
          return getAddExpr(getMulExpr(LHSC, Add->getOperand(0)),
                            getMulExpr(LHSC, Add->getOperand(1)));


    ++Idx;
    while (const SCEVConstant *RHSC = dyn_cast<SCEVConstant>(Ops[Idx])) {
      // We found two constants, fold them together!
      ConstantInt *Fold = ConstantInt::get(LHSC->getValue()->getValue() *
                                           RHSC->getValue()->getValue());
      Ops[0] = getConstant(Fold);
      Ops.erase(Ops.begin()+1);  // Erase the folded element
      if (Ops.size() == 1) return Ops[0];
      LHSC = cast<SCEVConstant>(Ops[0]);
    }

    // If we are left with a constant one being multiplied, strip it off.
    if (cast<SCEVConstant>(Ops[0])->getValue()->equalsInt(1)) {
      Ops.erase(Ops.begin());
      --Idx;
    } else if (cast<SCEVConstant>(Ops[0])->getValue()->isZero()) {
      // If we have a multiply of zero, it will always be zero.
      return Ops[0];
    }
  }

  // Skip over the add expression until we get to a multiply.
  while (Idx < Ops.size() && Ops[Idx]->getSCEVType() < scMulExpr)
    ++Idx;

  if (Ops.size() == 1)
    return Ops[0];

  // If there are mul operands inline them all into this expression.
  if (Idx < Ops.size()) {
    bool DeletedMul = false;
    while (const SCEVMulExpr *Mul = dyn_cast<SCEVMulExpr>(Ops[Idx])) {
      // If we have an mul, expand the mul operands onto the end of the operands
      // list.
      Ops.insert(Ops.end(), Mul->op_begin(), Mul->op_end());
      Ops.erase(Ops.begin()+Idx);
      DeletedMul = true;
    }

    // If we deleted at least one mul, we added operands to the end of the list,
    // and they are not necessarily sorted.  Recurse to resort and resimplify
    // any operands we just aquired.
    if (DeletedMul)
      return getMulExpr(Ops);
  }

  // If there are any add recurrences in the operands list, see if any other
  // added values are loop invariant.  If so, we can fold them into the
  // recurrence.
  while (Idx < Ops.size() && Ops[Idx]->getSCEVType() < scAddRecExpr)
    ++Idx;

  // Scan over all recurrences, trying to fold loop invariants into them.
  for (; Idx < Ops.size() && isa<SCEVAddRecExpr>(Ops[Idx]); ++Idx) {
    // Scan all of the other operands to this mul and add them to the vector if
    // they are loop invariant w.r.t. the recurrence.
    SmallVector<const SCEV *, 8> LIOps;
    const SCEVAddRecExpr *AddRec = cast<SCEVAddRecExpr>(Ops[Idx]);
    for (unsigned i = 0, e = Ops.size(); i != e; ++i)
      if (Ops[i]->isLoopInvariant(AddRec->getLoop())) {
        LIOps.push_back(Ops[i]);
        Ops.erase(Ops.begin()+i);
        --i; --e;
      }

    // If we found some loop invariants, fold them into the recurrence.
    if (!LIOps.empty()) {
      //  NLI * LI * {Start,+,Step}  -->  NLI * {LI*Start,+,LI*Step}
      SmallVector<const SCEV *, 4> NewOps;
      NewOps.reserve(AddRec->getNumOperands());
      if (LIOps.size() == 1) {
        const SCEV *Scale = LIOps[0];
        for (unsigned i = 0, e = AddRec->getNumOperands(); i != e; ++i)
          NewOps.push_back(getMulExpr(Scale, AddRec->getOperand(i)));
      } else {
        for (unsigned i = 0, e = AddRec->getNumOperands(); i != e; ++i) {
          SmallVector<const SCEV *, 4> MulOps(LIOps.begin(), LIOps.end());
          MulOps.push_back(AddRec->getOperand(i));
          NewOps.push_back(getMulExpr(MulOps));
        }
      }

      const SCEV *NewRec = getAddRecExpr(NewOps, AddRec->getLoop());

      // If all of the other operands were loop invariant, we are done.
      if (Ops.size() == 1) return NewRec;

      // Otherwise, multiply the folded AddRec by the non-liv parts.
      for (unsigned i = 0;; ++i)
        if (Ops[i] == AddRec) {
          Ops[i] = NewRec;
          break;
        }
      return getMulExpr(Ops);
    }

    // Okay, if there weren't any loop invariants to be folded, check to see if
    // there are multiple AddRec's with the same loop induction variable being
    // multiplied together.  If so, we can fold them.
    for (unsigned OtherIdx = Idx+1;
         OtherIdx < Ops.size() && isa<SCEVAddRecExpr>(Ops[OtherIdx]);++OtherIdx)
      if (OtherIdx != Idx) {
        const SCEVAddRecExpr *OtherAddRec = cast<SCEVAddRecExpr>(Ops[OtherIdx]);
        if (AddRec->getLoop() == OtherAddRec->getLoop()) {
          // F * G  -->  {A,+,B} * {C,+,D}  -->  {A*C,+,F*D + G*B + B*D}
          const SCEVAddRecExpr *F = AddRec, *G = OtherAddRec;
          const SCEV *NewStart = getMulExpr(F->getStart(),
                                                 G->getStart());
          const SCEV *B = F->getStepRecurrence(*this);
          const SCEV *D = G->getStepRecurrence(*this);
          const SCEV *NewStep = getAddExpr(getMulExpr(F, D),
                                          getMulExpr(G, B),
                                          getMulExpr(B, D));
          const SCEV *NewAddRec = getAddRecExpr(NewStart, NewStep,
                                               F->getLoop());
          if (Ops.size() == 2) return NewAddRec;

          Ops.erase(Ops.begin()+Idx);
          Ops.erase(Ops.begin()+OtherIdx-1);
          Ops.push_back(NewAddRec);
          return getMulExpr(Ops);
        }
      }

    // Otherwise couldn't fold anything into this recurrence.  Move onto the
    // next one.
  }

  // Okay, it looks like we really DO need an mul expr.  Check to see if we
  // already have one, otherwise create a new one.
  FoldingSetNodeID ID;
  ID.AddInteger(scMulExpr);
  ID.AddInteger(Ops.size());
  for (unsigned i = 0, e = Ops.size(); i != e; ++i)
    ID.AddPointer(Ops[i]);
  void *IP = 0;
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;
  SCEV *S = SCEVAllocator.Allocate<SCEVMulExpr>();
  new (S) SCEVMulExpr(ID, Ops);
  UniqueSCEVs.InsertNode(S, IP);
  return S;
}

/// getUDivExpr - Get a canonical multiply expression, or something simpler if
/// possible.
const SCEV *ScalarEvolution::getUDivExpr(const SCEV *LHS,
                                         const SCEV *RHS) {
  assert(getEffectiveSCEVType(LHS->getType()) ==
         getEffectiveSCEVType(RHS->getType()) &&
         "SCEVUDivExpr operand types don't match!");

  if (const SCEVConstant *RHSC = dyn_cast<SCEVConstant>(RHS)) {
    if (RHSC->getValue()->equalsInt(1))
      return LHS;                            // X udiv 1 --> x
    if (RHSC->isZero())
      return getIntegerSCEV(0, LHS->getType()); // value is undefined

    // Determine if the division can be folded into the operands of
    // its operands.
    // TODO: Generalize this to non-constants by using known-bits information.
    const Type *Ty = LHS->getType();
    unsigned LZ = RHSC->getValue()->getValue().countLeadingZeros();
    unsigned MaxShiftAmt = getTypeSizeInBits(Ty) - LZ;
    // For non-power-of-two values, effectively round the value up to the
    // nearest power of two.
    if (!RHSC->getValue()->getValue().isPowerOf2())
      ++MaxShiftAmt;
    const IntegerType *ExtTy =
      IntegerType::get(getTypeSizeInBits(Ty) + MaxShiftAmt);
    // {X,+,N}/C --> {X/C,+,N/C} if safe and N/C can be folded.
    if (const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(LHS))
      if (const SCEVConstant *Step =
            dyn_cast<SCEVConstant>(AR->getStepRecurrence(*this)))
        if (!Step->getValue()->getValue()
              .urem(RHSC->getValue()->getValue()) &&
            getZeroExtendExpr(AR, ExtTy) ==
            getAddRecExpr(getZeroExtendExpr(AR->getStart(), ExtTy),
                          getZeroExtendExpr(Step, ExtTy),
                          AR->getLoop())) {
          SmallVector<const SCEV *, 4> Operands;
          for (unsigned i = 0, e = AR->getNumOperands(); i != e; ++i)
            Operands.push_back(getUDivExpr(AR->getOperand(i), RHS));
          return getAddRecExpr(Operands, AR->getLoop());
        }
    // (A*B)/C --> A*(B/C) if safe and B/C can be folded.
    if (const SCEVMulExpr *M = dyn_cast<SCEVMulExpr>(LHS)) {
      SmallVector<const SCEV *, 4> Operands;
      for (unsigned i = 0, e = M->getNumOperands(); i != e; ++i)
        Operands.push_back(getZeroExtendExpr(M->getOperand(i), ExtTy));
      if (getZeroExtendExpr(M, ExtTy) == getMulExpr(Operands))
        // Find an operand that's safely divisible.
        for (unsigned i = 0, e = M->getNumOperands(); i != e; ++i) {
          const SCEV *Op = M->getOperand(i);
          const SCEV *Div = getUDivExpr(Op, RHSC);
          if (!isa<SCEVUDivExpr>(Div) && getMulExpr(Div, RHSC) == Op) {
            const SmallVectorImpl<const SCEV *> &MOperands = M->getOperands();
            Operands = SmallVector<const SCEV *, 4>(MOperands.begin(),
                                                  MOperands.end());
            Operands[i] = Div;
            return getMulExpr(Operands);
          }
        }
    }
    // (A+B)/C --> (A/C + B/C) if safe and A/C and B/C can be folded.
    if (const SCEVAddRecExpr *A = dyn_cast<SCEVAddRecExpr>(LHS)) {
      SmallVector<const SCEV *, 4> Operands;
      for (unsigned i = 0, e = A->getNumOperands(); i != e; ++i)
        Operands.push_back(getZeroExtendExpr(A->getOperand(i), ExtTy));
      if (getZeroExtendExpr(A, ExtTy) == getAddExpr(Operands)) {
        Operands.clear();
        for (unsigned i = 0, e = A->getNumOperands(); i != e; ++i) {
          const SCEV *Op = getUDivExpr(A->getOperand(i), RHS);
          if (isa<SCEVUDivExpr>(Op) || getMulExpr(Op, RHS) != A->getOperand(i))
            break;
          Operands.push_back(Op);
        }
        if (Operands.size() == A->getNumOperands())
          return getAddExpr(Operands);
      }
    }

    // Fold if both operands are constant.
    if (const SCEVConstant *LHSC = dyn_cast<SCEVConstant>(LHS)) {
      Constant *LHSCV = LHSC->getValue();
      Constant *RHSCV = RHSC->getValue();
      return getConstant(cast<ConstantInt>(ConstantExpr::getUDiv(LHSCV,
                                                                 RHSCV)));
    }
  }

  FoldingSetNodeID ID;
  ID.AddInteger(scUDivExpr);
  ID.AddPointer(LHS);
  ID.AddPointer(RHS);
  void *IP = 0;
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;
  SCEV *S = SCEVAllocator.Allocate<SCEVUDivExpr>();
  new (S) SCEVUDivExpr(ID, LHS, RHS);
  UniqueSCEVs.InsertNode(S, IP);
  return S;
}


/// getAddRecExpr - Get an add recurrence expression for the specified loop.
/// Simplify the expression as much as possible.
const SCEV *ScalarEvolution::getAddRecExpr(const SCEV *Start,
                               const SCEV *Step, const Loop *L) {
  SmallVector<const SCEV *, 4> Operands;
  Operands.push_back(Start);
  if (const SCEVAddRecExpr *StepChrec = dyn_cast<SCEVAddRecExpr>(Step))
    if (StepChrec->getLoop() == L) {
      Operands.insert(Operands.end(), StepChrec->op_begin(),
                      StepChrec->op_end());
      return getAddRecExpr(Operands, L);
    }

  Operands.push_back(Step);
  return getAddRecExpr(Operands, L);
}

/// getAddRecExpr - Get an add recurrence expression for the specified loop.
/// Simplify the expression as much as possible.
const SCEV *
ScalarEvolution::getAddRecExpr(SmallVectorImpl<const SCEV *> &Operands,
                               const Loop *L) {
  if (Operands.size() == 1) return Operands[0];
#ifndef NDEBUG
  for (unsigned i = 1, e = Operands.size(); i != e; ++i)
    assert(getEffectiveSCEVType(Operands[i]->getType()) ==
           getEffectiveSCEVType(Operands[0]->getType()) &&
           "SCEVAddRecExpr operand types don't match!");
#endif

  if (Operands.back()->isZero()) {
    Operands.pop_back();
    return getAddRecExpr(Operands, L);             // {X,+,0}  -->  X
  }

  // Canonicalize nested AddRecs in by nesting them in order of loop depth.
  if (const SCEVAddRecExpr *NestedAR = dyn_cast<SCEVAddRecExpr>(Operands[0])) {
    const Loop* NestedLoop = NestedAR->getLoop();
    if (L->getLoopDepth() < NestedLoop->getLoopDepth()) {
      SmallVector<const SCEV *, 4> NestedOperands(NestedAR->op_begin(),
                                                NestedAR->op_end());
      Operands[0] = NestedAR->getStart();
      // AddRecs require their operands be loop-invariant with respect to their
      // loops. Don't perform this transformation if it would break this
      // requirement.
      bool AllInvariant = true;
      for (unsigned i = 0, e = Operands.size(); i != e; ++i)
        if (!Operands[i]->isLoopInvariant(L)) {
          AllInvariant = false;
          break;
        }
      if (AllInvariant) {
        NestedOperands[0] = getAddRecExpr(Operands, L);
        AllInvariant = true;
        for (unsigned i = 0, e = NestedOperands.size(); i != e; ++i)
          if (!NestedOperands[i]->isLoopInvariant(NestedLoop)) {
            AllInvariant = false;
            break;
          }
        if (AllInvariant)
          // Ok, both add recurrences are valid after the transformation.
          return getAddRecExpr(NestedOperands, NestedLoop);
      }
      // Reset Operands to its original state.
      Operands[0] = NestedAR;
    }
  }

  FoldingSetNodeID ID;
  ID.AddInteger(scAddRecExpr);
  ID.AddInteger(Operands.size());
  for (unsigned i = 0, e = Operands.size(); i != e; ++i)
    ID.AddPointer(Operands[i]);
  ID.AddPointer(L);
  void *IP = 0;
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;
  SCEV *S = SCEVAllocator.Allocate<SCEVAddRecExpr>();
  new (S) SCEVAddRecExpr(ID, Operands, L);
  UniqueSCEVs.InsertNode(S, IP);
  return S;
}

const SCEV *ScalarEvolution::getSMaxExpr(const SCEV *LHS,
                                         const SCEV *RHS) {
  SmallVector<const SCEV *, 2> Ops;
  Ops.push_back(LHS);
  Ops.push_back(RHS);
  return getSMaxExpr(Ops);
}

const SCEV *
ScalarEvolution::getSMaxExpr(SmallVectorImpl<const SCEV *> &Ops) {
  assert(!Ops.empty() && "Cannot get empty smax!");
  if (Ops.size() == 1) return Ops[0];
#ifndef NDEBUG
  for (unsigned i = 1, e = Ops.size(); i != e; ++i)
    assert(getEffectiveSCEVType(Ops[i]->getType()) ==
           getEffectiveSCEVType(Ops[0]->getType()) &&
           "SCEVSMaxExpr operand types don't match!");
#endif

  // Sort by complexity, this groups all similar expression types together.
  GroupByComplexity(Ops, LI);

  // If there are any constants, fold them together.
  unsigned Idx = 0;
  if (const SCEVConstant *LHSC = dyn_cast<SCEVConstant>(Ops[0])) {
    ++Idx;
    assert(Idx < Ops.size());
    while (const SCEVConstant *RHSC = dyn_cast<SCEVConstant>(Ops[Idx])) {
      // We found two constants, fold them together!
      ConstantInt *Fold = ConstantInt::get(
                              APIntOps::smax(LHSC->getValue()->getValue(),
                                             RHSC->getValue()->getValue()));
      Ops[0] = getConstant(Fold);
      Ops.erase(Ops.begin()+1);  // Erase the folded element
      if (Ops.size() == 1) return Ops[0];
      LHSC = cast<SCEVConstant>(Ops[0]);
    }

    // If we are left with a constant minimum-int, strip it off.
    if (cast<SCEVConstant>(Ops[0])->getValue()->isMinValue(true)) {
      Ops.erase(Ops.begin());
      --Idx;
    } else if (cast<SCEVConstant>(Ops[0])->getValue()->isMaxValue(true)) {
      // If we have an smax with a constant maximum-int, it will always be
      // maximum-int.
      return Ops[0];
    }
  }

  if (Ops.size() == 1) return Ops[0];

  // Find the first SMax
  while (Idx < Ops.size() && Ops[Idx]->getSCEVType() < scSMaxExpr)
    ++Idx;

  // Check to see if one of the operands is an SMax. If so, expand its operands
  // onto our operand list, and recurse to simplify.
  if (Idx < Ops.size()) {
    bool DeletedSMax = false;
    while (const SCEVSMaxExpr *SMax = dyn_cast<SCEVSMaxExpr>(Ops[Idx])) {
      Ops.insert(Ops.end(), SMax->op_begin(), SMax->op_end());
      Ops.erase(Ops.begin()+Idx);
      DeletedSMax = true;
    }

    if (DeletedSMax)
      return getSMaxExpr(Ops);
  }

  // Okay, check to see if the same value occurs in the operand list twice.  If
  // so, delete one.  Since we sorted the list, these values are required to
  // be adjacent.
  for (unsigned i = 0, e = Ops.size()-1; i != e; ++i)
    if (Ops[i] == Ops[i+1]) {      //  X smax Y smax Y  -->  X smax Y
      Ops.erase(Ops.begin()+i, Ops.begin()+i+1);
      --i; --e;
    }

  if (Ops.size() == 1) return Ops[0];

  assert(!Ops.empty() && "Reduced smax down to nothing!");

  // Okay, it looks like we really DO need an smax expr.  Check to see if we
  // already have one, otherwise create a new one.
  FoldingSetNodeID ID;
  ID.AddInteger(scSMaxExpr);
  ID.AddInteger(Ops.size());
  for (unsigned i = 0, e = Ops.size(); i != e; ++i)
    ID.AddPointer(Ops[i]);
  void *IP = 0;
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;
  SCEV *S = SCEVAllocator.Allocate<SCEVSMaxExpr>();
  new (S) SCEVSMaxExpr(ID, Ops);
  UniqueSCEVs.InsertNode(S, IP);
  return S;
}

const SCEV *ScalarEvolution::getUMaxExpr(const SCEV *LHS,
                                         const SCEV *RHS) {
  SmallVector<const SCEV *, 2> Ops;
  Ops.push_back(LHS);
  Ops.push_back(RHS);
  return getUMaxExpr(Ops);
}

const SCEV *
ScalarEvolution::getUMaxExpr(SmallVectorImpl<const SCEV *> &Ops) {
  assert(!Ops.empty() && "Cannot get empty umax!");
  if (Ops.size() == 1) return Ops[0];
#ifndef NDEBUG
  for (unsigned i = 1, e = Ops.size(); i != e; ++i)
    assert(getEffectiveSCEVType(Ops[i]->getType()) ==
           getEffectiveSCEVType(Ops[0]->getType()) &&
           "SCEVUMaxExpr operand types don't match!");
#endif

  // Sort by complexity, this groups all similar expression types together.
  GroupByComplexity(Ops, LI);

  // If there are any constants, fold them together.
  unsigned Idx = 0;
  if (const SCEVConstant *LHSC = dyn_cast<SCEVConstant>(Ops[0])) {
    ++Idx;
    assert(Idx < Ops.size());
    while (const SCEVConstant *RHSC = dyn_cast<SCEVConstant>(Ops[Idx])) {
      // We found two constants, fold them together!
      ConstantInt *Fold = ConstantInt::get(
                              APIntOps::umax(LHSC->getValue()->getValue(),
                                             RHSC->getValue()->getValue()));
      Ops[0] = getConstant(Fold);
      Ops.erase(Ops.begin()+1);  // Erase the folded element
      if (Ops.size() == 1) return Ops[0];
      LHSC = cast<SCEVConstant>(Ops[0]);
    }

    // If we are left with a constant minimum-int, strip it off.
    if (cast<SCEVConstant>(Ops[0])->getValue()->isMinValue(false)) {
      Ops.erase(Ops.begin());
      --Idx;
    } else if (cast<SCEVConstant>(Ops[0])->getValue()->isMaxValue(false)) {
      // If we have an umax with a constant maximum-int, it will always be
      // maximum-int.
      return Ops[0];
    }
  }

  if (Ops.size() == 1) return Ops[0];

  // Find the first UMax
  while (Idx < Ops.size() && Ops[Idx]->getSCEVType() < scUMaxExpr)
    ++Idx;

  // Check to see if one of the operands is a UMax. If so, expand its operands
  // onto our operand list, and recurse to simplify.
  if (Idx < Ops.size()) {
    bool DeletedUMax = false;
    while (const SCEVUMaxExpr *UMax = dyn_cast<SCEVUMaxExpr>(Ops[Idx])) {
      Ops.insert(Ops.end(), UMax->op_begin(), UMax->op_end());
      Ops.erase(Ops.begin()+Idx);
      DeletedUMax = true;
    }

    if (DeletedUMax)
      return getUMaxExpr(Ops);
  }

  // Okay, check to see if the same value occurs in the operand list twice.  If
  // so, delete one.  Since we sorted the list, these values are required to
  // be adjacent.
  for (unsigned i = 0, e = Ops.size()-1; i != e; ++i)
    if (Ops[i] == Ops[i+1]) {      //  X umax Y umax Y  -->  X umax Y
      Ops.erase(Ops.begin()+i, Ops.begin()+i+1);
      --i; --e;
    }

  if (Ops.size() == 1) return Ops[0];

  assert(!Ops.empty() && "Reduced umax down to nothing!");

  // Okay, it looks like we really DO need a umax expr.  Check to see if we
  // already have one, otherwise create a new one.
  FoldingSetNodeID ID;
  ID.AddInteger(scUMaxExpr);
  ID.AddInteger(Ops.size());
  for (unsigned i = 0, e = Ops.size(); i != e; ++i)
    ID.AddPointer(Ops[i]);
  void *IP = 0;
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;
  SCEV *S = SCEVAllocator.Allocate<SCEVUMaxExpr>();
  new (S) SCEVUMaxExpr(ID, Ops);
  UniqueSCEVs.InsertNode(S, IP);
  return S;
}

const SCEV *ScalarEvolution::getSMinExpr(const SCEV *LHS,
                                         const SCEV *RHS) {
  // ~smax(~x, ~y) == smin(x, y).
  return getNotSCEV(getSMaxExpr(getNotSCEV(LHS), getNotSCEV(RHS)));
}

const SCEV *ScalarEvolution::getUMinExpr(const SCEV *LHS,
                                         const SCEV *RHS) {
  // ~umax(~x, ~y) == umin(x, y)
  return getNotSCEV(getUMaxExpr(getNotSCEV(LHS), getNotSCEV(RHS)));
}

const SCEV *ScalarEvolution::getUnknown(Value *V) {
  // Don't attempt to do anything other than create a SCEVUnknown object
  // here.  createSCEV only calls getUnknown after checking for all other
  // interesting possibilities, and any other code that calls getUnknown
  // is doing so in order to hide a value from SCEV canonicalization.

  FoldingSetNodeID ID;
  ID.AddInteger(scUnknown);
  ID.AddPointer(V);
  void *IP = 0;
  if (const SCEV *S = UniqueSCEVs.FindNodeOrInsertPos(ID, IP)) return S;
  SCEV *S = SCEVAllocator.Allocate<SCEVUnknown>();
  new (S) SCEVUnknown(ID, V);
  UniqueSCEVs.InsertNode(S, IP);
  return S;
}

//===----------------------------------------------------------------------===//
//            Basic SCEV Analysis and PHI Idiom Recognition Code
//

/// isSCEVable - Test if values of the given type are analyzable within
/// the SCEV framework. This primarily includes integer types, and it
/// can optionally include pointer types if the ScalarEvolution class
/// has access to target-specific information.
bool ScalarEvolution::isSCEVable(const Type *Ty) const {
  // Integers are always SCEVable.
  if (Ty->isInteger())
    return true;

  // Pointers are SCEVable if TargetData information is available
  // to provide pointer size information.
  if (isa<PointerType>(Ty))
    return TD != NULL;

  // Otherwise it's not SCEVable.
  return false;
}

/// getTypeSizeInBits - Return the size in bits of the specified type,
/// for which isSCEVable must return true.
uint64_t ScalarEvolution::getTypeSizeInBits(const Type *Ty) const {
  assert(isSCEVable(Ty) && "Type is not SCEVable!");

  // If we have a TargetData, use it!
  if (TD)
    return TD->getTypeSizeInBits(Ty);

  // Otherwise, we support only integer types.
  assert(Ty->isInteger() && "isSCEVable permitted a non-SCEVable type!");
  return Ty->getPrimitiveSizeInBits();
}

/// getEffectiveSCEVType - Return a type with the same bitwidth as
/// the given type and which represents how SCEV will treat the given
/// type, for which isSCEVable must return true. For pointer types,
/// this is the pointer-sized integer type.
const Type *ScalarEvolution::getEffectiveSCEVType(const Type *Ty) const {
  assert(isSCEVable(Ty) && "Type is not SCEVable!");

  if (Ty->isInteger())
    return Ty;

  assert(isa<PointerType>(Ty) && "Unexpected non-pointer non-integer type!");
  return TD->getIntPtrType();
}

const SCEV *ScalarEvolution::getCouldNotCompute() {
  return &CouldNotCompute;
}

/// getSCEV - Return an existing SCEV if it exists, otherwise analyze the
/// expression and create a new one.
const SCEV *ScalarEvolution::getSCEV(Value *V) {
  assert(isSCEVable(V->getType()) && "Value is not SCEVable!");

  std::map<SCEVCallbackVH, const SCEV *>::iterator I = Scalars.find(V);
  if (I != Scalars.end()) return I->second;
  const SCEV *S = createSCEV(V);
  Scalars.insert(std::make_pair(SCEVCallbackVH(V, this), S));
  return S;
}

/// getIntegerSCEV - Given a SCEVable type, create a constant for the
/// specified signed integer value and return a SCEV for the constant.
const SCEV *ScalarEvolution::getIntegerSCEV(int Val, const Type *Ty) {
  const IntegerType *ITy = cast<IntegerType>(getEffectiveSCEVType(Ty));
  return getConstant(ConstantInt::get(ITy, Val));
}

/// getNegativeSCEV - Return a SCEV corresponding to -V = -1*V
///
const SCEV *ScalarEvolution::getNegativeSCEV(const SCEV *V) {
  if (const SCEVConstant *VC = dyn_cast<SCEVConstant>(V))
    return getConstant(
               cast<ConstantInt>(Context->getConstantExprNeg(VC->getValue())));

  const Type *Ty = V->getType();
  Ty = getEffectiveSCEVType(Ty);
  return getMulExpr(V,
                  getConstant(cast<ConstantInt>(Context->getAllOnesValue(Ty))));
}

/// getNotSCEV - Return a SCEV corresponding to ~V = -1-V
const SCEV *ScalarEvolution::getNotSCEV(const SCEV *V) {
  if (const SCEVConstant *VC = dyn_cast<SCEVConstant>(V))
    return getConstant(
                cast<ConstantInt>(Context->getConstantExprNot(VC->getValue())));

  const Type *Ty = V->getType();
  Ty = getEffectiveSCEVType(Ty);
  const SCEV *AllOnes =
                   getConstant(cast<ConstantInt>(Context->getAllOnesValue(Ty)));
  return getMinusSCEV(AllOnes, V);
}

/// getMinusSCEV - Return a SCEV corresponding to LHS - RHS.
///
const SCEV *ScalarEvolution::getMinusSCEV(const SCEV *LHS,
                                          const SCEV *RHS) {
  // X - Y --> X + -Y
  return getAddExpr(LHS, getNegativeSCEV(RHS));
}

/// getTruncateOrZeroExtend - Return a SCEV corresponding to a conversion of the
/// input value to the specified type.  If the type must be extended, it is zero
/// extended.
const SCEV *
ScalarEvolution::getTruncateOrZeroExtend(const SCEV *V,
                                         const Type *Ty) {
  const Type *SrcTy = V->getType();
  assert((SrcTy->isInteger() || (TD && isa<PointerType>(SrcTy))) &&
         (Ty->isInteger() || (TD && isa<PointerType>(Ty))) &&
         "Cannot truncate or zero extend with non-integer arguments!");
  if (getTypeSizeInBits(SrcTy) == getTypeSizeInBits(Ty))
    return V;  // No conversion
  if (getTypeSizeInBits(SrcTy) > getTypeSizeInBits(Ty))
    return getTruncateExpr(V, Ty);
  return getZeroExtendExpr(V, Ty);
}

/// getTruncateOrSignExtend - Return a SCEV corresponding to a conversion of the
/// input value to the specified type.  If the type must be extended, it is sign
/// extended.
const SCEV *
ScalarEvolution::getTruncateOrSignExtend(const SCEV *V,
                                         const Type *Ty) {
  const Type *SrcTy = V->getType();
  assert((SrcTy->isInteger() || (TD && isa<PointerType>(SrcTy))) &&
         (Ty->isInteger() || (TD && isa<PointerType>(Ty))) &&
         "Cannot truncate or zero extend with non-integer arguments!");
  if (getTypeSizeInBits(SrcTy) == getTypeSizeInBits(Ty))
    return V;  // No conversion
  if (getTypeSizeInBits(SrcTy) > getTypeSizeInBits(Ty))
    return getTruncateExpr(V, Ty);
  return getSignExtendExpr(V, Ty);
}

/// getNoopOrZeroExtend - Return a SCEV corresponding to a conversion of the
/// input value to the specified type.  If the type must be extended, it is zero
/// extended.  The conversion must not be narrowing.
const SCEV *
ScalarEvolution::getNoopOrZeroExtend(const SCEV *V, const Type *Ty) {
  const Type *SrcTy = V->getType();
  assert((SrcTy->isInteger() || (TD && isa<PointerType>(SrcTy))) &&
         (Ty->isInteger() || (TD && isa<PointerType>(Ty))) &&
         "Cannot noop or zero extend with non-integer arguments!");
  assert(getTypeSizeInBits(SrcTy) <= getTypeSizeInBits(Ty) &&
         "getNoopOrZeroExtend cannot truncate!");
  if (getTypeSizeInBits(SrcTy) == getTypeSizeInBits(Ty))
    return V;  // No conversion
  return getZeroExtendExpr(V, Ty);
}

/// getNoopOrSignExtend - Return a SCEV corresponding to a conversion of the
/// input value to the specified type.  If the type must be extended, it is sign
/// extended.  The conversion must not be narrowing.
const SCEV *
ScalarEvolution::getNoopOrSignExtend(const SCEV *V, const Type *Ty) {
  const Type *SrcTy = V->getType();
  assert((SrcTy->isInteger() || (TD && isa<PointerType>(SrcTy))) &&
         (Ty->isInteger() || (TD && isa<PointerType>(Ty))) &&
         "Cannot noop or sign extend with non-integer arguments!");
  assert(getTypeSizeInBits(SrcTy) <= getTypeSizeInBits(Ty) &&
         "getNoopOrSignExtend cannot truncate!");
  if (getTypeSizeInBits(SrcTy) == getTypeSizeInBits(Ty))
    return V;  // No conversion
  return getSignExtendExpr(V, Ty);
}

/// getNoopOrAnyExtend - Return a SCEV corresponding to a conversion of
/// the input value to the specified type. If the type must be extended,
/// it is extended with unspecified bits. The conversion must not be
/// narrowing.
const SCEV *
ScalarEvolution::getNoopOrAnyExtend(const SCEV *V, const Type *Ty) {
  const Type *SrcTy = V->getType();
  assert((SrcTy->isInteger() || (TD && isa<PointerType>(SrcTy))) &&
         (Ty->isInteger() || (TD && isa<PointerType>(Ty))) &&
         "Cannot noop or any extend with non-integer arguments!");
  assert(getTypeSizeInBits(SrcTy) <= getTypeSizeInBits(Ty) &&
         "getNoopOrAnyExtend cannot truncate!");
  if (getTypeSizeInBits(SrcTy) == getTypeSizeInBits(Ty))
    return V;  // No conversion
  return getAnyExtendExpr(V, Ty);
}

/// getTruncateOrNoop - Return a SCEV corresponding to a conversion of the
/// input value to the specified type.  The conversion must not be widening.
const SCEV *
ScalarEvolution::getTruncateOrNoop(const SCEV *V, const Type *Ty) {
  const Type *SrcTy = V->getType();
  assert((SrcTy->isInteger() || (TD && isa<PointerType>(SrcTy))) &&
         (Ty->isInteger() || (TD && isa<PointerType>(Ty))) &&
         "Cannot truncate or noop with non-integer arguments!");
  assert(getTypeSizeInBits(SrcTy) >= getTypeSizeInBits(Ty) &&
         "getTruncateOrNoop cannot extend!");
  if (getTypeSizeInBits(SrcTy) == getTypeSizeInBits(Ty))
    return V;  // No conversion
  return getTruncateExpr(V, Ty);
}

/// getUMaxFromMismatchedTypes - Promote the operands to the wider of
/// the types using zero-extension, and then perform a umax operation
/// with them.
const SCEV *ScalarEvolution::getUMaxFromMismatchedTypes(const SCEV *LHS,
                                                        const SCEV *RHS) {
  const SCEV *PromotedLHS = LHS;
  const SCEV *PromotedRHS = RHS;

  if (getTypeSizeInBits(LHS->getType()) > getTypeSizeInBits(RHS->getType()))
    PromotedRHS = getZeroExtendExpr(RHS, LHS->getType());
  else
    PromotedLHS = getNoopOrZeroExtend(LHS, RHS->getType());

  return getUMaxExpr(PromotedLHS, PromotedRHS);
}

/// getUMinFromMismatchedTypes - Promote the operands to the wider of
/// the types using zero-extension, and then perform a umin operation
/// with them.
const SCEV *ScalarEvolution::getUMinFromMismatchedTypes(const SCEV *LHS,
                                                        const SCEV *RHS) {
  const SCEV *PromotedLHS = LHS;
  const SCEV *PromotedRHS = RHS;

  if (getTypeSizeInBits(LHS->getType()) > getTypeSizeInBits(RHS->getType()))
    PromotedRHS = getZeroExtendExpr(RHS, LHS->getType());
  else
    PromotedLHS = getNoopOrZeroExtend(LHS, RHS->getType());

  return getUMinExpr(PromotedLHS, PromotedRHS);
}

/// ReplaceSymbolicValueWithConcrete - This looks up the computed SCEV value for
/// the specified instruction and replaces any references to the symbolic value
/// SymName with the specified value.  This is used during PHI resolution.
void
ScalarEvolution::ReplaceSymbolicValueWithConcrete(Instruction *I,
                                                  const SCEV *SymName,
                                                  const SCEV *NewVal) {
  std::map<SCEVCallbackVH, const SCEV *>::iterator SI =
    Scalars.find(SCEVCallbackVH(I, this));
  if (SI == Scalars.end()) return;

  const SCEV *NV =
    SI->second->replaceSymbolicValuesWithConcrete(SymName, NewVal, *this);
  if (NV == SI->second) return;  // No change.

  SI->second = NV;       // Update the scalars map!

  // Any instruction values that use this instruction might also need to be
  // updated!
  for (Value::use_iterator UI = I->use_begin(), E = I->use_end();
       UI != E; ++UI)
    ReplaceSymbolicValueWithConcrete(cast<Instruction>(*UI), SymName, NewVal);
}

/// createNodeForPHI - PHI nodes have two cases.  Either the PHI node exists in
/// a loop header, making it a potential recurrence, or it doesn't.
///
const SCEV *ScalarEvolution::createNodeForPHI(PHINode *PN) {
  if (PN->getNumIncomingValues() == 2)  // The loops have been canonicalized.
    if (const Loop *L = LI->getLoopFor(PN->getParent()))
      if (L->getHeader() == PN->getParent()) {
        // If it lives in the loop header, it has two incoming values, one
        // from outside the loop, and one from inside.
        unsigned IncomingEdge = L->contains(PN->getIncomingBlock(0));
        unsigned BackEdge     = IncomingEdge^1;

        // While we are analyzing this PHI node, handle its value symbolically.
        const SCEV *SymbolicName = getUnknown(PN);
        assert(Scalars.find(PN) == Scalars.end() &&
               "PHI node already processed?");
        Scalars.insert(std::make_pair(SCEVCallbackVH(PN, this), SymbolicName));

        // Using this symbolic name for the PHI, analyze the value coming around
        // the back-edge.
        const SCEV *BEValue = getSCEV(PN->getIncomingValue(BackEdge));

        // NOTE: If BEValue is loop invariant, we know that the PHI node just
        // has a special value for the first iteration of the loop.

        // If the value coming around the backedge is an add with the symbolic
        // value we just inserted, then we found a simple induction variable!
        if (const SCEVAddExpr *Add = dyn_cast<SCEVAddExpr>(BEValue)) {
          // If there is a single occurrence of the symbolic value, replace it
          // with a recurrence.
          unsigned FoundIndex = Add->getNumOperands();
          for (unsigned i = 0, e = Add->getNumOperands(); i != e; ++i)
            if (Add->getOperand(i) == SymbolicName)
              if (FoundIndex == e) {
                FoundIndex = i;
                break;
              }

          if (FoundIndex != Add->getNumOperands()) {
            // Create an add with everything but the specified operand.
            SmallVector<const SCEV *, 8> Ops;
            for (unsigned i = 0, e = Add->getNumOperands(); i != e; ++i)
              if (i != FoundIndex)
                Ops.push_back(Add->getOperand(i));
            const SCEV *Accum = getAddExpr(Ops);

            // This is not a valid addrec if the step amount is varying each
            // loop iteration, but is not itself an addrec in this loop.
            if (Accum->isLoopInvariant(L) ||
                (isa<SCEVAddRecExpr>(Accum) &&
                 cast<SCEVAddRecExpr>(Accum)->getLoop() == L)) {
              const SCEV *StartVal =
                getSCEV(PN->getIncomingValue(IncomingEdge));
              const SCEV *PHISCEV =
                getAddRecExpr(StartVal, Accum, L);

              // Okay, for the entire analysis of this edge we assumed the PHI
              // to be symbolic.  We now need to go back and update all of the
              // entries for the scalars that use the PHI (except for the PHI
              // itself) to use the new analyzed value instead of the "symbolic"
              // value.
              ReplaceSymbolicValueWithConcrete(PN, SymbolicName, PHISCEV);
              return PHISCEV;
            }
          }
        } else if (const SCEVAddRecExpr *AddRec =
                     dyn_cast<SCEVAddRecExpr>(BEValue)) {
          // Otherwise, this could be a loop like this:
          //     i = 0;  for (j = 1; ..; ++j) { ....  i = j; }
          // In this case, j = {1,+,1}  and BEValue is j.
          // Because the other in-value of i (0) fits the evolution of BEValue
          // i really is an addrec evolution.
          if (AddRec->getLoop() == L && AddRec->isAffine()) {
            const SCEV *StartVal = getSCEV(PN->getIncomingValue(IncomingEdge));

            // If StartVal = j.start - j.stride, we can use StartVal as the
            // initial step of the addrec evolution.
            if (StartVal == getMinusSCEV(AddRec->getOperand(0),
                                            AddRec->getOperand(1))) {
              const SCEV *PHISCEV =
                 getAddRecExpr(StartVal, AddRec->getOperand(1), L);

              // Okay, for the entire analysis of this edge we assumed the PHI
              // to be symbolic.  We now need to go back and update all of the
              // entries for the scalars that use the PHI (except for the PHI
              // itself) to use the new analyzed value instead of the "symbolic"
              // value.
              ReplaceSymbolicValueWithConcrete(PN, SymbolicName, PHISCEV);
              return PHISCEV;
            }
          }
        }

        return SymbolicName;
      }

  // If it's not a loop phi, we can't handle it yet.
  return getUnknown(PN);
}

/// createNodeForGEP - Expand GEP instructions into add and multiply
/// operations. This allows them to be analyzed by regular SCEV code.
///
const SCEV *ScalarEvolution::createNodeForGEP(User *GEP) {

  const Type *IntPtrTy = TD->getIntPtrType();
  Value *Base = GEP->getOperand(0);
  // Don't attempt to analyze GEPs over unsized objects.
  if (!cast<PointerType>(Base->getType())->getElementType()->isSized())
    return getUnknown(GEP);
  const SCEV *TotalOffset = getIntegerSCEV(0, IntPtrTy);
  gep_type_iterator GTI = gep_type_begin(GEP);
  for (GetElementPtrInst::op_iterator I = next(GEP->op_begin()),
                                      E = GEP->op_end();
       I != E; ++I) {
    Value *Index = *I;
    // Compute the (potentially symbolic) offset in bytes for this index.
    if (const StructType *STy = dyn_cast<StructType>(*GTI++)) {
      // For a struct, add the member offset.
      const StructLayout &SL = *TD->getStructLayout(STy);
      unsigned FieldNo = cast<ConstantInt>(Index)->getZExtValue();
      uint64_t Offset = SL.getElementOffset(FieldNo);
      TotalOffset = getAddExpr(TotalOffset,
                                  getIntegerSCEV(Offset, IntPtrTy));
    } else {
      // For an array, add the element offset, explicitly scaled.
      const SCEV *LocalOffset = getSCEV(Index);
      if (!isa<PointerType>(LocalOffset->getType()))
        // Getelementptr indicies are signed.
        LocalOffset = getTruncateOrSignExtend(LocalOffset,
                                              IntPtrTy);
      LocalOffset =
        getMulExpr(LocalOffset,
                   getIntegerSCEV(TD->getTypeAllocSize(*GTI),
                                  IntPtrTy));
      TotalOffset = getAddExpr(TotalOffset, LocalOffset);
    }
  }
  return getAddExpr(getSCEV(Base), TotalOffset);
}

/// GetMinTrailingZeros - Determine the minimum number of zero bits that S is
/// guaranteed to end in (at every loop iteration).  It is, at the same time,
/// the minimum number of times S is divisible by 2.  For example, given {4,+,8}
/// it returns 2.  If S is guaranteed to be 0, it returns the bitwidth of S.
uint32_t
ScalarEvolution::GetMinTrailingZeros(const SCEV *S) {
  if (const SCEVConstant *C = dyn_cast<SCEVConstant>(S))
    return C->getValue()->getValue().countTrailingZeros();

  if (const SCEVTruncateExpr *T = dyn_cast<SCEVTruncateExpr>(S))
    return std::min(GetMinTrailingZeros(T->getOperand()),
                    (uint32_t)getTypeSizeInBits(T->getType()));

  if (const SCEVZeroExtendExpr *E = dyn_cast<SCEVZeroExtendExpr>(S)) {
    uint32_t OpRes = GetMinTrailingZeros(E->getOperand());
    return OpRes == getTypeSizeInBits(E->getOperand()->getType()) ?
             getTypeSizeInBits(E->getType()) : OpRes;
  }

  if (const SCEVSignExtendExpr *E = dyn_cast<SCEVSignExtendExpr>(S)) {
    uint32_t OpRes = GetMinTrailingZeros(E->getOperand());
    return OpRes == getTypeSizeInBits(E->getOperand()->getType()) ?
             getTypeSizeInBits(E->getType()) : OpRes;
  }

  if (const SCEVAddExpr *A = dyn_cast<SCEVAddExpr>(S)) {
    // The result is the min of all operands results.
    uint32_t MinOpRes = GetMinTrailingZeros(A->getOperand(0));
    for (unsigned i = 1, e = A->getNumOperands(); MinOpRes && i != e; ++i)
      MinOpRes = std::min(MinOpRes, GetMinTrailingZeros(A->getOperand(i)));
    return MinOpRes;
  }

  if (const SCEVMulExpr *M = dyn_cast<SCEVMulExpr>(S)) {
    // The result is the sum of all operands results.
    uint32_t SumOpRes = GetMinTrailingZeros(M->getOperand(0));
    uint32_t BitWidth = getTypeSizeInBits(M->getType());
    for (unsigned i = 1, e = M->getNumOperands();
         SumOpRes != BitWidth && i != e; ++i)
      SumOpRes = std::min(SumOpRes + GetMinTrailingZeros(M->getOperand(i)),
                          BitWidth);
    return SumOpRes;
  }

  if (const SCEVAddRecExpr *A = dyn_cast<SCEVAddRecExpr>(S)) {
    // The result is the min of all operands results.
    uint32_t MinOpRes = GetMinTrailingZeros(A->getOperand(0));
    for (unsigned i = 1, e = A->getNumOperands(); MinOpRes && i != e; ++i)
      MinOpRes = std::min(MinOpRes, GetMinTrailingZeros(A->getOperand(i)));
    return MinOpRes;
  }

  if (const SCEVSMaxExpr *M = dyn_cast<SCEVSMaxExpr>(S)) {
    // The result is the min of all operands results.
    uint32_t MinOpRes = GetMinTrailingZeros(M->getOperand(0));
    for (unsigned i = 1, e = M->getNumOperands(); MinOpRes && i != e; ++i)
      MinOpRes = std::min(MinOpRes, GetMinTrailingZeros(M->getOperand(i)));
    return MinOpRes;
  }

  if (const SCEVUMaxExpr *M = dyn_cast<SCEVUMaxExpr>(S)) {
    // The result is the min of all operands results.
    uint32_t MinOpRes = GetMinTrailingZeros(M->getOperand(0));
    for (unsigned i = 1, e = M->getNumOperands(); MinOpRes && i != e; ++i)
      MinOpRes = std::min(MinOpRes, GetMinTrailingZeros(M->getOperand(i)));
    return MinOpRes;
  }

  if (const SCEVUnknown *U = dyn_cast<SCEVUnknown>(S)) {
    // For a SCEVUnknown, ask ValueTracking.
    unsigned BitWidth = getTypeSizeInBits(U->getType());
    APInt Mask = APInt::getAllOnesValue(BitWidth);
    APInt Zeros(BitWidth, 0), Ones(BitWidth, 0);
    ComputeMaskedBits(U->getValue(), Mask, Zeros, Ones);
    return Zeros.countTrailingOnes();
  }

  // SCEVUDivExpr
  return 0;
}

uint32_t
ScalarEvolution::GetMinLeadingZeros(const SCEV *S) {
  // TODO: Handle other SCEV expression types here.

  if (const SCEVConstant *C = dyn_cast<SCEVConstant>(S))
    return C->getValue()->getValue().countLeadingZeros();

  if (const SCEVZeroExtendExpr *C = dyn_cast<SCEVZeroExtendExpr>(S)) {
    // A zero-extension cast adds zero bits.
    return GetMinLeadingZeros(C->getOperand()) +
           (getTypeSizeInBits(C->getType()) -
            getTypeSizeInBits(C->getOperand()->getType()));
  }

  if (const SCEVUnknown *U = dyn_cast<SCEVUnknown>(S)) {
    // For a SCEVUnknown, ask ValueTracking.
    unsigned BitWidth = getTypeSizeInBits(U->getType());
    APInt Mask = APInt::getAllOnesValue(BitWidth);
    APInt Zeros(BitWidth, 0), Ones(BitWidth, 0);
    ComputeMaskedBits(U->getValue(), Mask, Zeros, Ones, TD);
    return Zeros.countLeadingOnes();
  }

  return 1;
}

uint32_t
ScalarEvolution::GetMinSignBits(const SCEV *S) {
  // TODO: Handle other SCEV expression types here.

  if (const SCEVConstant *C = dyn_cast<SCEVConstant>(S)) {
    const APInt &A = C->getValue()->getValue();
    return A.isNegative() ? A.countLeadingOnes() :
                            A.countLeadingZeros();
  }

  if (const SCEVSignExtendExpr *C = dyn_cast<SCEVSignExtendExpr>(S)) {
    // A sign-extension cast adds sign bits.
    return GetMinSignBits(C->getOperand()) +
           (getTypeSizeInBits(C->getType()) -
            getTypeSizeInBits(C->getOperand()->getType()));
  }

  if (const SCEVAddExpr *A = dyn_cast<SCEVAddExpr>(S)) {
    unsigned BitWidth = getTypeSizeInBits(A->getType());

    // Special case decrementing a value (ADD X, -1):
    if (const SCEVConstant *CRHS = dyn_cast<SCEVConstant>(A->getOperand(0)))
      if (CRHS->isAllOnesValue()) {
        SmallVector<const SCEV *, 4> OtherOps(A->op_begin() + 1, A->op_end());
        const SCEV *OtherOpsAdd = getAddExpr(OtherOps);
        unsigned LZ = GetMinLeadingZeros(OtherOpsAdd);

        // If the input is known to be 0 or 1, the output is 0/-1, which is all
        // sign bits set.
        if (LZ == BitWidth - 1)
          return BitWidth;

        // If we are subtracting one from a positive number, there is no carry
        // out of the result.
        if (LZ > 0)
          return GetMinSignBits(OtherOpsAdd);
      }

    // Add can have at most one carry bit.  Thus we know that the output
    // is, at worst, one more bit than the inputs.
    unsigned Min = BitWidth;
    for (unsigned i = 0, e = A->getNumOperands(); i != e; ++i) {
      unsigned N = GetMinSignBits(A->getOperand(i));
      Min = std::min(Min, N) - 1;
      if (Min == 0) return 1;
    }
    return 1;
  }

  if (const SCEVUnknown *U = dyn_cast<SCEVUnknown>(S)) {
    // For a SCEVUnknown, ask ValueTracking.
    return ComputeNumSignBits(U->getValue(), TD);
  }

  return 1;
}

/// createSCEV - We know that there is no SCEV for the specified value.
/// Analyze the expression.
///
const SCEV *ScalarEvolution::createSCEV(Value *V) {
  if (!isSCEVable(V->getType()))
    return getUnknown(V);

  unsigned Opcode = Instruction::UserOp1;
  if (Instruction *I = dyn_cast<Instruction>(V))
    Opcode = I->getOpcode();
  else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(V))
    Opcode = CE->getOpcode();
  else if (ConstantInt *CI = dyn_cast<ConstantInt>(V))
    return getConstant(CI);
  else if (isa<ConstantPointerNull>(V))
    return getIntegerSCEV(0, V->getType());
  else if (isa<UndefValue>(V))
    return getIntegerSCEV(0, V->getType());
  else
    return getUnknown(V);

  User *U = cast<User>(V);
  switch (Opcode) {
  case Instruction::Add:
    return getAddExpr(getSCEV(U->getOperand(0)),
                      getSCEV(U->getOperand(1)));
  case Instruction::Mul:
    return getMulExpr(getSCEV(U->getOperand(0)),
                      getSCEV(U->getOperand(1)));
  case Instruction::UDiv:
    return getUDivExpr(getSCEV(U->getOperand(0)),
                       getSCEV(U->getOperand(1)));
  case Instruction::Sub:
    return getMinusSCEV(getSCEV(U->getOperand(0)),
                        getSCEV(U->getOperand(1)));
  case Instruction::And:
    // For an expression like x&255 that merely masks off the high bits,
    // use zext(trunc(x)) as the SCEV expression.
    if (ConstantInt *CI = dyn_cast<ConstantInt>(U->getOperand(1))) {
      if (CI->isNullValue())
        return getSCEV(U->getOperand(1));
      if (CI->isAllOnesValue())
        return getSCEV(U->getOperand(0));
      const APInt &A = CI->getValue();

      // Instcombine's ShrinkDemandedConstant may strip bits out of
      // constants, obscuring what would otherwise be a low-bits mask.
      // Use ComputeMaskedBits to compute what ShrinkDemandedConstant
      // knew about to reconstruct a low-bits mask value.
      unsigned LZ = A.countLeadingZeros();
      unsigned BitWidth = A.getBitWidth();
      APInt AllOnes = APInt::getAllOnesValue(BitWidth);
      APInt KnownZero(BitWidth, 0), KnownOne(BitWidth, 0);
      ComputeMaskedBits(U->getOperand(0), AllOnes, KnownZero, KnownOne, TD);

      APInt EffectiveMask = APInt::getLowBitsSet(BitWidth, BitWidth - LZ);

      if (LZ != 0 && !((~A & ~KnownZero) & EffectiveMask))
        return
          getZeroExtendExpr(getTruncateExpr(getSCEV(U->getOperand(0)),
                                            IntegerType::get(BitWidth - LZ)),
                            U->getType());
    }
    break;

  case Instruction::Or:
    // If the RHS of the Or is a constant, we may have something like:
    // X*4+1 which got turned into X*4|1.  Handle this as an Add so loop
    // optimizations will transparently handle this case.
    //
    // In order for this transformation to be safe, the LHS must be of the
    // form X*(2^n) and the Or constant must be less than 2^n.
    if (ConstantInt *CI = dyn_cast<ConstantInt>(U->getOperand(1))) {
      const SCEV *LHS = getSCEV(U->getOperand(0));
      const APInt &CIVal = CI->getValue();
      if (GetMinTrailingZeros(LHS) >=
          (CIVal.getBitWidth() - CIVal.countLeadingZeros()))
        return getAddExpr(LHS, getSCEV(U->getOperand(1)));
    }
    break;
  case Instruction::Xor:
    if (ConstantInt *CI = dyn_cast<ConstantInt>(U->getOperand(1))) {
      // If the RHS of the xor is a signbit, then this is just an add.
      // Instcombine turns add of signbit into xor as a strength reduction step.
      if (CI->getValue().isSignBit())
        return getAddExpr(getSCEV(U->getOperand(0)),
                          getSCEV(U->getOperand(1)));

      // If the RHS of xor is -1, then this is a not operation.
      if (CI->isAllOnesValue())
        return getNotSCEV(getSCEV(U->getOperand(0)));

      // Model xor(and(x, C), C) as and(~x, C), if C is a low-bits mask.
      // This is a variant of the check for xor with -1, and it handles
      // the case where instcombine has trimmed non-demanded bits out
      // of an xor with -1.
      if (BinaryOperator *BO = dyn_cast<BinaryOperator>(U->getOperand(0)))
        if (ConstantInt *LCI = dyn_cast<ConstantInt>(BO->getOperand(1)))
          if (BO->getOpcode() == Instruction::And &&
              LCI->getValue() == CI->getValue())
            if (const SCEVZeroExtendExpr *Z =
                  dyn_cast<SCEVZeroExtendExpr>(getSCEV(U->getOperand(0)))) {
              const Type *UTy = U->getType();
              const SCEV *Z0 = Z->getOperand();
              const Type *Z0Ty = Z0->getType();
              unsigned Z0TySize = getTypeSizeInBits(Z0Ty);

              // If C is a low-bits mask, the zero extend is zerving to
              // mask off the high bits. Complement the operand and
              // re-apply the zext.
              if (APIntOps::isMask(Z0TySize, CI->getValue()))
                return getZeroExtendExpr(getNotSCEV(Z0), UTy);

              // If C is a single bit, it may be in the sign-bit position
              // before the zero-extend. In this case, represent the xor
              // using an add, which is equivalent, and re-apply the zext.
              APInt Trunc = APInt(CI->getValue()).trunc(Z0TySize);
              if (APInt(Trunc).zext(getTypeSizeInBits(UTy)) == CI->getValue() &&
                  Trunc.isSignBit())
                return getZeroExtendExpr(getAddExpr(Z0, getConstant(Trunc)),
                                         UTy);
            }
    }
    break;

  case Instruction::Shl:
    // Turn shift left of a constant amount into a multiply.
    if (ConstantInt *SA = dyn_cast<ConstantInt>(U->getOperand(1))) {
      uint32_t BitWidth = cast<IntegerType>(V->getType())->getBitWidth();
      Constant *X = ConstantInt::get(
        APInt(BitWidth, 1).shl(SA->getLimitedValue(BitWidth)));
      return getMulExpr(getSCEV(U->getOperand(0)), getSCEV(X));
    }
    break;

  case Instruction::LShr:
    // Turn logical shift right of a constant into a unsigned divide.
    if (ConstantInt *SA = dyn_cast<ConstantInt>(U->getOperand(1))) {
      uint32_t BitWidth = cast<IntegerType>(V->getType())->getBitWidth();
      Constant *X = ConstantInt::get(
        APInt(BitWidth, 1).shl(SA->getLimitedValue(BitWidth)));
      return getUDivExpr(getSCEV(U->getOperand(0)), getSCEV(X));
    }
    break;

  case Instruction::AShr:
    // For a two-shift sext-inreg, use sext(trunc(x)) as the SCEV expression.
    if (ConstantInt *CI = dyn_cast<ConstantInt>(U->getOperand(1)))
      if (Instruction *L = dyn_cast<Instruction>(U->getOperand(0)))
        if (L->getOpcode() == Instruction::Shl &&
            L->getOperand(1) == U->getOperand(1)) {
          unsigned BitWidth = getTypeSizeInBits(U->getType());
          uint64_t Amt = BitWidth - CI->getZExtValue();
          if (Amt == BitWidth)
            return getSCEV(L->getOperand(0));       // shift by zero --> noop
          if (Amt > BitWidth)
            return getIntegerSCEV(0, U->getType()); // value is undefined
          return
            getSignExtendExpr(getTruncateExpr(getSCEV(L->getOperand(0)),
                                                      IntegerType::get(Amt)),
                                 U->getType());
        }
    break;

  case Instruction::Trunc:
    return getTruncateExpr(getSCEV(U->getOperand(0)), U->getType());

  case Instruction::ZExt:
    return getZeroExtendExpr(getSCEV(U->getOperand(0)), U->getType());

  case Instruction::SExt:
    return getSignExtendExpr(getSCEV(U->getOperand(0)), U->getType());

  case Instruction::BitCast:
    // BitCasts are no-op casts so we just eliminate the cast.
    if (isSCEVable(U->getType()) && isSCEVable(U->getOperand(0)->getType()))
      return getSCEV(U->getOperand(0));
    break;

  case Instruction::IntToPtr:
    if (!TD) break; // Without TD we can't analyze pointers.
    return getTruncateOrZeroExtend(getSCEV(U->getOperand(0)),
                                   TD->getIntPtrType());

  case Instruction::PtrToInt:
    if (!TD) break; // Without TD we can't analyze pointers.
    return getTruncateOrZeroExtend(getSCEV(U->getOperand(0)),
                                   U->getType());

  case Instruction::GetElementPtr:
    if (!TD) break; // Without TD we can't analyze pointers.
    return createNodeForGEP(U);

  case Instruction::PHI:
    return createNodeForPHI(cast<PHINode>(U));

  case Instruction::Select:
    // This could be a smax or umax that was lowered earlier.
    // Try to recover it.
    if (ICmpInst *ICI = dyn_cast<ICmpInst>(U->getOperand(0))) {
      Value *LHS = ICI->getOperand(0);
      Value *RHS = ICI->getOperand(1);
      switch (ICI->getPredicate()) {
      case ICmpInst::ICMP_SLT:
      case ICmpInst::ICMP_SLE:
        std::swap(LHS, RHS);
        // fall through
      case ICmpInst::ICMP_SGT:
      case ICmpInst::ICMP_SGE:
        if (LHS == U->getOperand(1) && RHS == U->getOperand(2))
          return getSMaxExpr(getSCEV(LHS), getSCEV(RHS));
        else if (LHS == U->getOperand(2) && RHS == U->getOperand(1))
          return getSMinExpr(getSCEV(LHS), getSCEV(RHS));
        break;
      case ICmpInst::ICMP_ULT:
      case ICmpInst::ICMP_ULE:
        std::swap(LHS, RHS);
        // fall through
      case ICmpInst::ICMP_UGT:
      case ICmpInst::ICMP_UGE:
        if (LHS == U->getOperand(1) && RHS == U->getOperand(2))
          return getUMaxExpr(getSCEV(LHS), getSCEV(RHS));
        else if (LHS == U->getOperand(2) && RHS == U->getOperand(1))
          return getUMinExpr(getSCEV(LHS), getSCEV(RHS));
        break;
      case ICmpInst::ICMP_NE:
        // n != 0 ? n : 1  ->  umax(n, 1)
        if (LHS == U->getOperand(1) &&
            isa<ConstantInt>(U->getOperand(2)) &&
            cast<ConstantInt>(U->getOperand(2))->isOne() &&
            isa<ConstantInt>(RHS) &&
            cast<ConstantInt>(RHS)->isZero())
          return getUMaxExpr(getSCEV(LHS), getSCEV(U->getOperand(2)));
        break;
      case ICmpInst::ICMP_EQ:
        // n == 0 ? 1 : n  ->  umax(n, 1)
        if (LHS == U->getOperand(2) &&
            isa<ConstantInt>(U->getOperand(1)) &&
            cast<ConstantInt>(U->getOperand(1))->isOne() &&
            isa<ConstantInt>(RHS) &&
            cast<ConstantInt>(RHS)->isZero())
          return getUMaxExpr(getSCEV(LHS), getSCEV(U->getOperand(1)));
        break;
      default:
        break;
      }
    }

  default: // We cannot analyze this expression.
    break;
  }

  return getUnknown(V);
}



//===----------------------------------------------------------------------===//
//                   Iteration Count Computation Code
//

/// getBackedgeTakenCount - If the specified loop has a predictable
/// backedge-taken count, return it, otherwise return a SCEVCouldNotCompute
/// object. The backedge-taken count is the number of times the loop header
/// will be branched to from within the loop. This is one less than the
/// trip count of the loop, since it doesn't count the first iteration,
/// when the header is branched to from outside the loop.
///
/// Note that it is not valid to call this method on a loop without a
/// loop-invariant backedge-taken count (see
/// hasLoopInvariantBackedgeTakenCount).
///
const SCEV *ScalarEvolution::getBackedgeTakenCount(const Loop *L) {
  return getBackedgeTakenInfo(L).Exact;
}

/// getMaxBackedgeTakenCount - Similar to getBackedgeTakenCount, except
/// return the least SCEV value that is known never to be less than the
/// actual backedge taken count.
const SCEV *ScalarEvolution::getMaxBackedgeTakenCount(const Loop *L) {
  return getBackedgeTakenInfo(L).Max;
}

/// PushLoopPHIs - Push PHI nodes in the header of the given loop
/// onto the given Worklist.
static void
PushLoopPHIs(const Loop *L, SmallVectorImpl<Instruction *> &Worklist) {
  BasicBlock *Header = L->getHeader();

  // Push all Loop-header PHIs onto the Worklist stack.
  for (BasicBlock::iterator I = Header->begin();
       PHINode *PN = dyn_cast<PHINode>(I); ++I)
    Worklist.push_back(PN);
}

/// PushDefUseChildren - Push users of the given Instruction
/// onto the given Worklist.
static void
PushDefUseChildren(Instruction *I,
                   SmallVectorImpl<Instruction *> &Worklist) {
  // Push the def-use children onto the Worklist stack.
  for (Value::use_iterator UI = I->use_begin(), UE = I->use_end();
       UI != UE; ++UI)
    Worklist.push_back(cast<Instruction>(UI));
}

const ScalarEvolution::BackedgeTakenInfo &
ScalarEvolution::getBackedgeTakenInfo(const Loop *L) {
  // Initially insert a CouldNotCompute for this loop. If the insertion
  // succeeds, procede to actually compute a backedge-taken count and
  // update the value. The temporary CouldNotCompute value tells SCEV
  // code elsewhere that it shouldn't attempt to request a new
  // backedge-taken count, which could result in infinite recursion.
  std::pair<std::map<const Loop*, BackedgeTakenInfo>::iterator, bool> Pair =
    BackedgeTakenCounts.insert(std::make_pair(L, getCouldNotCompute()));
  if (Pair.second) {
    BackedgeTakenInfo ItCount = ComputeBackedgeTakenCount(L);
    if (ItCount.Exact != getCouldNotCompute()) {
      assert(ItCount.Exact->isLoopInvariant(L) &&
             ItCount.Max->isLoopInvariant(L) &&
             "Computed trip count isn't loop invariant for loop!");
      ++NumTripCountsComputed;

      // Update the value in the map.
      Pair.first->second = ItCount;
    } else {
      if (ItCount.Max != getCouldNotCompute())
        // Update the value in the map.
        Pair.first->second = ItCount;
      if (isa<PHINode>(L->getHeader()->begin()))
        // Only count loops that have phi nodes as not being computable.
        ++NumTripCountsNotComputed;
    }

    // Now that we know more about the trip count for this loop, forget any
    // existing SCEV values for PHI nodes in this loop since they are only
    // conservative estimates made without the benefit of trip count
    // information. This is similar to the code in
    // forgetLoopBackedgeTakenCount, except that it handles SCEVUnknown PHI
    // nodes specially.
    if (ItCount.hasAnyInfo()) {
      SmallVector<Instruction *, 16> Worklist;
      PushLoopPHIs(L, Worklist);

      SmallPtrSet<Instruction *, 8> Visited;
      while (!Worklist.empty()) {
        Instruction *I = Worklist.pop_back_val();
        if (!Visited.insert(I)) continue;

        std::map<SCEVCallbackVH, const SCEV*>::iterator It =
          Scalars.find(static_cast<Value *>(I));
        if (It != Scalars.end()) {
          // SCEVUnknown for a PHI either means that it has an unrecognized
          // structure, or it's a PHI that's in the progress of being computed
          // by createNodeForPHI.  In the former case, additional loop trip count
          // information isn't going to change anything. In the later case,
          // createNodeForPHI will perform the necessary updates on its own when
          // it gets to that point.
          if (!isa<PHINode>(I) || !isa<SCEVUnknown>(It->second))
            Scalars.erase(It);
          ValuesAtScopes.erase(I);
          if (PHINode *PN = dyn_cast<PHINode>(I))
            ConstantEvolutionLoopExitValue.erase(PN);
        }

        PushDefUseChildren(I, Worklist);
      }
    }
  }
  return Pair.first->second;
}

/// forgetLoopBackedgeTakenCount - This method should be called by the
/// client when it has changed a loop in a way that may effect
/// ScalarEvolution's ability to compute a trip count, or if the loop
/// is deleted.
void ScalarEvolution::forgetLoopBackedgeTakenCount(const Loop *L) {
  BackedgeTakenCounts.erase(L);

  SmallVector<Instruction *, 16> Worklist;
  PushLoopPHIs(L, Worklist);

  SmallPtrSet<Instruction *, 8> Visited;
  while (!Worklist.empty()) {
    Instruction *I = Worklist.pop_back_val();
    if (!Visited.insert(I)) continue;

    std::map<SCEVCallbackVH, const SCEV*>::iterator It =
      Scalars.find(static_cast<Value *>(I));
    if (It != Scalars.end()) {
      Scalars.erase(It);
      ValuesAtScopes.erase(I);
      if (PHINode *PN = dyn_cast<PHINode>(I))
        ConstantEvolutionLoopExitValue.erase(PN);
    }

    PushDefUseChildren(I, Worklist);
  }
}

/// ComputeBackedgeTakenCount - Compute the number of times the backedge
/// of the specified loop will execute.
ScalarEvolution::BackedgeTakenInfo
ScalarEvolution::ComputeBackedgeTakenCount(const Loop *L) {
  SmallVector<BasicBlock*, 8> ExitingBlocks;
  L->getExitingBlocks(ExitingBlocks);

  // Examine all exits and pick the most conservative values.
  const SCEV *BECount = getCouldNotCompute();
  const SCEV *MaxBECount = getCouldNotCompute();
  bool CouldNotComputeBECount = false;
  for (unsigned i = 0, e = ExitingBlocks.size(); i != e; ++i) {
    BackedgeTakenInfo NewBTI =
      ComputeBackedgeTakenCountFromExit(L, ExitingBlocks[i]);

    if (NewBTI.Exact == getCouldNotCompute()) {
      // We couldn't compute an exact value for this exit, so
      // we won't be able to compute an exact value for the loop.
      CouldNotComputeBECount = true;
      BECount = getCouldNotCompute();
    } else if (!CouldNotComputeBECount) {
      if (BECount == getCouldNotCompute())
        BECount = NewBTI.Exact;
      else
        BECount = getUMinFromMismatchedTypes(BECount, NewBTI.Exact);
    }
    if (MaxBECount == getCouldNotCompute())
      MaxBECount = NewBTI.Max;
    else if (NewBTI.Max != getCouldNotCompute())
      MaxBECount = getUMinFromMismatchedTypes(MaxBECount, NewBTI.Max);
  }

  return BackedgeTakenInfo(BECount, MaxBECount);
}

/// ComputeBackedgeTakenCountFromExit - Compute the number of times the backedge
/// of the specified loop will execute if it exits via the specified block.
ScalarEvolution::BackedgeTakenInfo
ScalarEvolution::ComputeBackedgeTakenCountFromExit(const Loop *L,
                                                   BasicBlock *ExitingBlock) {

  // Okay, we've chosen an exiting block.  See what condition causes us to
  // exit at this block.
  //
  // FIXME: we should be able to handle switch instructions (with a single exit)
  BranchInst *ExitBr = dyn_cast<BranchInst>(ExitingBlock->getTerminator());
  if (ExitBr == 0) return getCouldNotCompute();
  assert(ExitBr->isConditional() && "If unconditional, it can't be in loop!");

  // At this point, we know we have a conditional branch that determines whether
  // the loop is exited.  However, we don't know if the branch is executed each
  // time through the loop.  If not, then the execution count of the branch will
  // not be equal to the trip count of the loop.
  //
  // Currently we check for this by checking to see if the Exit branch goes to
  // the loop header.  If so, we know it will always execute the same number of
  // times as the loop.  We also handle the case where the exit block *is* the
  // loop header.  This is common for un-rotated loops.
  //
  // If both of those tests fail, walk up the unique predecessor chain to the
  // header, stopping if there is an edge that doesn't exit the loop. If the
  // header is reached, the execution count of the branch will be equal to the
  // trip count of the loop.
  //
  //  More extensive analysis could be done to handle more cases here.
  //
  if (ExitBr->getSuccessor(0) != L->getHeader() &&
      ExitBr->getSuccessor(1) != L->getHeader() &&
      ExitBr->getParent() != L->getHeader()) {
    // The simple checks failed, try climbing the unique predecessor chain
    // up to the header.
    bool Ok = false;
    for (BasicBlock *BB = ExitBr->getParent(); BB; ) {
      BasicBlock *Pred = BB->getUniquePredecessor();
      if (!Pred)
        return getCouldNotCompute();
      TerminatorInst *PredTerm = Pred->getTerminator();
      for (unsigned i = 0, e = PredTerm->getNumSuccessors(); i != e; ++i) {
        BasicBlock *PredSucc = PredTerm->getSuccessor(i);
        if (PredSucc == BB)
          continue;
        // If the predecessor has a successor that isn't BB and isn't
        // outside the loop, assume the worst.
        if (L->contains(PredSucc))
          return getCouldNotCompute();
      }
      if (Pred == L->getHeader()) {
        Ok = true;
        break;
      }
      BB = Pred;
    }
    if (!Ok)
      return getCouldNotCompute();
  }

  // Procede to the next level to examine the exit condition expression.
  return ComputeBackedgeTakenCountFromExitCond(L, ExitBr->getCondition(),
                                               ExitBr->getSuccessor(0),
                                               ExitBr->getSuccessor(1));
}

/// ComputeBackedgeTakenCountFromExitCond - Compute the number of times the
/// backedge of the specified loop will execute if its exit condition
/// were a conditional branch of ExitCond, TBB, and FBB.
ScalarEvolution::BackedgeTakenInfo
ScalarEvolution::ComputeBackedgeTakenCountFromExitCond(const Loop *L,
                                                       Value *ExitCond,
                                                       BasicBlock *TBB,
                                                       BasicBlock *FBB) {
  // Check if the controlling expression for this loop is an And or Or.
  if (BinaryOperator *BO = dyn_cast<BinaryOperator>(ExitCond)) {
    if (BO->getOpcode() == Instruction::And) {
      // Recurse on the operands of the and.
      BackedgeTakenInfo BTI0 =
        ComputeBackedgeTakenCountFromExitCond(L, BO->getOperand(0), TBB, FBB);
      BackedgeTakenInfo BTI1 =
        ComputeBackedgeTakenCountFromExitCond(L, BO->getOperand(1), TBB, FBB);
      const SCEV *BECount = getCouldNotCompute();
      const SCEV *MaxBECount = getCouldNotCompute();
      if (L->contains(TBB)) {
        // Both conditions must be true for the loop to continue executing.
        // Choose the less conservative count.
        if (BTI0.Exact == getCouldNotCompute() ||
            BTI1.Exact == getCouldNotCompute())
          BECount = getCouldNotCompute();
        else
          BECount = getUMinFromMismatchedTypes(BTI0.Exact, BTI1.Exact);
        if (BTI0.Max == getCouldNotCompute())
          MaxBECount = BTI1.Max;
        else if (BTI1.Max == getCouldNotCompute())
          MaxBECount = BTI0.Max;
        else
          MaxBECount = getUMinFromMismatchedTypes(BTI0.Max, BTI1.Max);
      } else {
        // Both conditions must be true for the loop to exit.
        assert(L->contains(FBB) && "Loop block has no successor in loop!");
        if (BTI0.Exact != getCouldNotCompute() &&
            BTI1.Exact != getCouldNotCompute())
          BECount = getUMaxFromMismatchedTypes(BTI0.Exact, BTI1.Exact);
        if (BTI0.Max != getCouldNotCompute() &&
            BTI1.Max != getCouldNotCompute())
          MaxBECount = getUMaxFromMismatchedTypes(BTI0.Max, BTI1.Max);
      }

      return BackedgeTakenInfo(BECount, MaxBECount);
    }
    if (BO->getOpcode() == Instruction::Or) {
      // Recurse on the operands of the or.
      BackedgeTakenInfo BTI0 =
        ComputeBackedgeTakenCountFromExitCond(L, BO->getOperand(0), TBB, FBB);
      BackedgeTakenInfo BTI1 =
        ComputeBackedgeTakenCountFromExitCond(L, BO->getOperand(1), TBB, FBB);
      const SCEV *BECount = getCouldNotCompute();
      const SCEV *MaxBECount = getCouldNotCompute();
      if (L->contains(FBB)) {
        // Both conditions must be false for the loop to continue executing.
        // Choose the less conservative count.
        if (BTI0.Exact == getCouldNotCompute() ||
            BTI1.Exact == getCouldNotCompute())
          BECount = getCouldNotCompute();
        else
          BECount = getUMinFromMismatchedTypes(BTI0.Exact, BTI1.Exact);
        if (BTI0.Max == getCouldNotCompute())
          MaxBECount = BTI1.Max;
        else if (BTI1.Max == getCouldNotCompute())
          MaxBECount = BTI0.Max;
        else
          MaxBECount = getUMinFromMismatchedTypes(BTI0.Max, BTI1.Max);
      } else {
        // Both conditions must be false for the loop to exit.
        assert(L->contains(TBB) && "Loop block has no successor in loop!");
        if (BTI0.Exact != getCouldNotCompute() &&
            BTI1.Exact != getCouldNotCompute())
          BECount = getUMaxFromMismatchedTypes(BTI0.Exact, BTI1.Exact);
        if (BTI0.Max != getCouldNotCompute() &&
            BTI1.Max != getCouldNotCompute())
          MaxBECount = getUMaxFromMismatchedTypes(BTI0.Max, BTI1.Max);
      }

      return BackedgeTakenInfo(BECount, MaxBECount);
    }
  }

  // With an icmp, it may be feasible to compute an exact backedge-taken count.
  // Procede to the next level to examine the icmp.
  if (ICmpInst *ExitCondICmp = dyn_cast<ICmpInst>(ExitCond))
    return ComputeBackedgeTakenCountFromExitCondICmp(L, ExitCondICmp, TBB, FBB);

  // If it's not an integer or pointer comparison then compute it the hard way.
  return ComputeBackedgeTakenCountExhaustively(L, ExitCond, !L->contains(TBB));
}

/// ComputeBackedgeTakenCountFromExitCondICmp - Compute the number of times the
/// backedge of the specified loop will execute if its exit condition
/// were a conditional branch of the ICmpInst ExitCond, TBB, and FBB.
ScalarEvolution::BackedgeTakenInfo
ScalarEvolution::ComputeBackedgeTakenCountFromExitCondICmp(const Loop *L,
                                                           ICmpInst *ExitCond,
                                                           BasicBlock *TBB,
                                                           BasicBlock *FBB) {

  // If the condition was exit on true, convert the condition to exit on false
  ICmpInst::Predicate Cond;
  if (!L->contains(FBB))
    Cond = ExitCond->getPredicate();
  else
    Cond = ExitCond->getInversePredicate();

  // Handle common loops like: for (X = "string"; *X; ++X)
  if (LoadInst *LI = dyn_cast<LoadInst>(ExitCond->getOperand(0)))
    if (Constant *RHS = dyn_cast<Constant>(ExitCond->getOperand(1))) {
      const SCEV *ItCnt =
        ComputeLoadConstantCompareBackedgeTakenCount(LI, RHS, L, Cond);
      if (!isa<SCEVCouldNotCompute>(ItCnt)) {
        unsigned BitWidth = getTypeSizeInBits(ItCnt->getType());
        return BackedgeTakenInfo(ItCnt,
                                 isa<SCEVConstant>(ItCnt) ? ItCnt :
                                   getConstant(APInt::getMaxValue(BitWidth)-1));
      }
    }

  const SCEV *LHS = getSCEV(ExitCond->getOperand(0));
  const SCEV *RHS = getSCEV(ExitCond->getOperand(1));

  // Try to evaluate any dependencies out of the loop.
  LHS = getSCEVAtScope(LHS, L);
  RHS = getSCEVAtScope(RHS, L);

  // At this point, we would like to compute how many iterations of the
  // loop the predicate will return true for these inputs.
  if (LHS->isLoopInvariant(L) && !RHS->isLoopInvariant(L)) {
    // If there is a loop-invariant, force it into the RHS.
    std::swap(LHS, RHS);
    Cond = ICmpInst::getSwappedPredicate(Cond);
  }

  // If we have a comparison of a chrec against a constant, try to use value
  // ranges to answer this query.
  if (const SCEVConstant *RHSC = dyn_cast<SCEVConstant>(RHS))
    if (const SCEVAddRecExpr *AddRec = dyn_cast<SCEVAddRecExpr>(LHS))
      if (AddRec->getLoop() == L) {
        // Form the constant range.
        ConstantRange CompRange(
            ICmpInst::makeConstantRange(Cond, RHSC->getValue()->getValue()));

        const SCEV *Ret = AddRec->getNumIterationsInRange(CompRange, *this);
        if (!isa<SCEVCouldNotCompute>(Ret)) return Ret;
      }

  switch (Cond) {
  case ICmpInst::ICMP_NE: {                     // while (X != Y)
    // Convert to: while (X-Y != 0)
    const SCEV *TC = HowFarToZero(getMinusSCEV(LHS, RHS), L);
    if (!isa<SCEVCouldNotCompute>(TC)) return TC;
    break;
  }
  case ICmpInst::ICMP_EQ: {
    // Convert to: while (X-Y == 0)           // while (X == Y)
    const SCEV *TC = HowFarToNonZero(getMinusSCEV(LHS, RHS), L);
    if (!isa<SCEVCouldNotCompute>(TC)) return TC;
    break;
  }
  case ICmpInst::ICMP_SLT: {
    BackedgeTakenInfo BTI = HowManyLessThans(LHS, RHS, L, true);
    if (BTI.hasAnyInfo()) return BTI;
    break;
  }
  case ICmpInst::ICMP_SGT: {
    BackedgeTakenInfo BTI = HowManyLessThans(getNotSCEV(LHS),
                                             getNotSCEV(RHS), L, true);
    if (BTI.hasAnyInfo()) return BTI;
    break;
  }
  case ICmpInst::ICMP_ULT: {
    BackedgeTakenInfo BTI = HowManyLessThans(LHS, RHS, L, false);
    if (BTI.hasAnyInfo()) return BTI;
    break;
  }
  case ICmpInst::ICMP_UGT: {
    BackedgeTakenInfo BTI = HowManyLessThans(getNotSCEV(LHS),
                                             getNotSCEV(RHS), L, false);
    if (BTI.hasAnyInfo()) return BTI;
    break;
  }
  default:
#if 0
    errs() << "ComputeBackedgeTakenCount ";
    if (ExitCond->getOperand(0)->getType()->isUnsigned())
      errs() << "[unsigned] ";
    errs() << *LHS << "   "
         << Instruction::getOpcodeName(Instruction::ICmp)
         << "   " << *RHS << "\n";
#endif
    break;
  }
  return
    ComputeBackedgeTakenCountExhaustively(L, ExitCond, !L->contains(TBB));
}

static ConstantInt *
EvaluateConstantChrecAtConstant(const SCEVAddRecExpr *AddRec, ConstantInt *C,
                                ScalarEvolution &SE) {
  const SCEV *InVal = SE.getConstant(C);
  const SCEV *Val = AddRec->evaluateAtIteration(InVal, SE);
  assert(isa<SCEVConstant>(Val) &&
         "Evaluation of SCEV at constant didn't fold correctly?");
  return cast<SCEVConstant>(Val)->getValue();
}

/// GetAddressedElementFromGlobal - Given a global variable with an initializer
/// and a GEP expression (missing the pointer index) indexing into it, return
/// the addressed element of the initializer or null if the index expression is
/// invalid.
static Constant *
GetAddressedElementFromGlobal(LLVMContext *Context, GlobalVariable *GV,
                              const std::vector<ConstantInt*> &Indices) {
  Constant *Init = GV->getInitializer();
  for (unsigned i = 0, e = Indices.size(); i != e; ++i) {
    uint64_t Idx = Indices[i]->getZExtValue();
    if (ConstantStruct *CS = dyn_cast<ConstantStruct>(Init)) {
      assert(Idx < CS->getNumOperands() && "Bad struct index!");
      Init = cast<Constant>(CS->getOperand(Idx));
    } else if (ConstantArray *CA = dyn_cast<ConstantArray>(Init)) {
      if (Idx >= CA->getNumOperands()) return 0;  // Bogus program
      Init = cast<Constant>(CA->getOperand(Idx));
    } else if (isa<ConstantAggregateZero>(Init)) {
      if (const StructType *STy = dyn_cast<StructType>(Init->getType())) {
        assert(Idx < STy->getNumElements() && "Bad struct index!");
        Init = Context->getNullValue(STy->getElementType(Idx));
      } else if (const ArrayType *ATy = dyn_cast<ArrayType>(Init->getType())) {
        if (Idx >= ATy->getNumElements()) return 0;  // Bogus program
        Init = Context->getNullValue(ATy->getElementType());
      } else {
        LLVM_UNREACHABLE("Unknown constant aggregate type!");
      }
      return 0;
    } else {
      return 0; // Unknown initializer type
    }
  }
  return Init;
}

/// ComputeLoadConstantCompareBackedgeTakenCount - Given an exit condition of
/// 'icmp op load X, cst', try to see if we can compute the backedge
/// execution count.
const SCEV *
ScalarEvolution::ComputeLoadConstantCompareBackedgeTakenCount(
                                                LoadInst *LI,
                                                Constant *RHS,
                                                const Loop *L,
                                                ICmpInst::Predicate predicate) {
  if (LI->isVolatile()) return getCouldNotCompute();

  // Check to see if the loaded pointer is a getelementptr of a global.
  GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(LI->getOperand(0));
  if (!GEP) return getCouldNotCompute();

  // Make sure that it is really a constant global we are gepping, with an
  // initializer, and make sure the first IDX is really 0.
  GlobalVariable *GV = dyn_cast<GlobalVariable>(GEP->getOperand(0));
  if (!GV || !GV->isConstant() || !GV->hasInitializer() ||
      GEP->getNumOperands() < 3 || !isa<Constant>(GEP->getOperand(1)) ||
      !cast<Constant>(GEP->getOperand(1))->isNullValue())
    return getCouldNotCompute();

  // Okay, we allow one non-constant index into the GEP instruction.
  Value *VarIdx = 0;
  std::vector<ConstantInt*> Indexes;
  unsigned VarIdxNum = 0;
  for (unsigned i = 2, e = GEP->getNumOperands(); i != e; ++i)
    if (ConstantInt *CI = dyn_cast<ConstantInt>(GEP->getOperand(i))) {
      Indexes.push_back(CI);
    } else if (!isa<ConstantInt>(GEP->getOperand(i))) {
      if (VarIdx) return getCouldNotCompute();  // Multiple non-constant idx's.
      VarIdx = GEP->getOperand(i);
      VarIdxNum = i-2;
      Indexes.push_back(0);
    }

  // Okay, we know we have a (load (gep GV, 0, X)) comparison with a constant.
  // Check to see if X is a loop variant variable value now.
  const SCEV *Idx = getSCEV(VarIdx);
  Idx = getSCEVAtScope(Idx, L);

  // We can only recognize very limited forms of loop index expressions, in
  // particular, only affine AddRec's like {C1,+,C2}.
  const SCEVAddRecExpr *IdxExpr = dyn_cast<SCEVAddRecExpr>(Idx);
  if (!IdxExpr || !IdxExpr->isAffine() || IdxExpr->isLoopInvariant(L) ||
      !isa<SCEVConstant>(IdxExpr->getOperand(0)) ||
      !isa<SCEVConstant>(IdxExpr->getOperand(1)))
    return getCouldNotCompute();

  unsigned MaxSteps = MaxBruteForceIterations;
  for (unsigned IterationNum = 0; IterationNum != MaxSteps; ++IterationNum) {
    ConstantInt *ItCst =
      ConstantInt::get(cast<IntegerType>(IdxExpr->getType()), IterationNum);
    ConstantInt *Val = EvaluateConstantChrecAtConstant(IdxExpr, ItCst, *this);

    // Form the GEP offset.
    Indexes[VarIdxNum] = Val;

    Constant *Result = GetAddressedElementFromGlobal(Context, GV, Indexes);
    if (Result == 0) break;  // Cannot compute!

    // Evaluate the condition for this iteration.
    Result = ConstantExpr::getICmp(predicate, Result, RHS);
    if (!isa<ConstantInt>(Result)) break;  // Couldn't decide for sure
    if (cast<ConstantInt>(Result)->getValue().isMinValue()) {
#if 0
      errs() << "\n***\n*** Computed loop count " << *ItCst
             << "\n*** From global " << *GV << "*** BB: " << *L->getHeader()
             << "***\n";
#endif
      ++NumArrayLenItCounts;
      return getConstant(ItCst);   // Found terminating iteration!
    }
  }
  return getCouldNotCompute();
}


/// CanConstantFold - Return true if we can constant fold an instruction of the
/// specified type, assuming that all operands were constants.
static bool CanConstantFold(const Instruction *I) {
  if (isa<BinaryOperator>(I) || isa<CmpInst>(I) ||
      isa<SelectInst>(I) || isa<CastInst>(I) || isa<GetElementPtrInst>(I))
    return true;

  if (const CallInst *CI = dyn_cast<CallInst>(I))
    if (const Function *F = CI->getCalledFunction())
      return canConstantFoldCallTo(F);
  return false;
}

/// getConstantEvolvingPHI - Given an LLVM value and a loop, return a PHI node
/// in the loop that V is derived from.  We allow arbitrary operations along the
/// way, but the operands of an operation must either be constants or a value
/// derived from a constant PHI.  If this expression does not fit with these
/// constraints, return null.
static PHINode *getConstantEvolvingPHI(Value *V, const Loop *L) {
  // If this is not an instruction, or if this is an instruction outside of the
  // loop, it can't be derived from a loop PHI.
  Instruction *I = dyn_cast<Instruction>(V);
  if (I == 0 || !L->contains(I->getParent())) return 0;

  if (PHINode *PN = dyn_cast<PHINode>(I)) {
    if (L->getHeader() == I->getParent())
      return PN;
    else
      // We don't currently keep track of the control flow needed to evaluate
      // PHIs, so we cannot handle PHIs inside of loops.
      return 0;
  }

  // If we won't be able to constant fold this expression even if the operands
  // are constants, return early.
  if (!CanConstantFold(I)) return 0;

  // Otherwise, we can evaluate this instruction if all of its operands are
  // constant or derived from a PHI node themselves.
  PHINode *PHI = 0;
  for (unsigned Op = 0, e = I->getNumOperands(); Op != e; ++Op)
    if (!(isa<Constant>(I->getOperand(Op)) ||
          isa<GlobalValue>(I->getOperand(Op)))) {
      PHINode *P = getConstantEvolvingPHI(I->getOperand(Op), L);
      if (P == 0) return 0;  // Not evolving from PHI
      if (PHI == 0)
        PHI = P;
      else if (PHI != P)
        return 0;  // Evolving from multiple different PHIs.
    }

  // This is a expression evolving from a constant PHI!
  return PHI;
}

/// EvaluateExpression - Given an expression that passes the
/// getConstantEvolvingPHI predicate, evaluate its value assuming the PHI node
/// in the loop has the value PHIVal.  If we can't fold this expression for some
/// reason, return null.
static Constant *EvaluateExpression(Value *V, Constant *PHIVal) {
  if (isa<PHINode>(V)) return PHIVal;
  if (Constant *C = dyn_cast<Constant>(V)) return C;
  if (GlobalValue *GV = dyn_cast<GlobalValue>(V)) return GV;
  Instruction *I = cast<Instruction>(V);
  LLVMContext *Context = I->getParent()->getContext();

  std::vector<Constant*> Operands;
  Operands.resize(I->getNumOperands());

  for (unsigned i = 0, e = I->getNumOperands(); i != e; ++i) {
    Operands[i] = EvaluateExpression(I->getOperand(i), PHIVal);
    if (Operands[i] == 0) return 0;
  }

  if (const CmpInst *CI = dyn_cast<CmpInst>(I))
    return ConstantFoldCompareInstOperands(CI->getPredicate(),
                                           &Operands[0], Operands.size(),
                                           Context);
  else
    return ConstantFoldInstOperands(I->getOpcode(), I->getType(),
                                    &Operands[0], Operands.size(),
                                    Context);
}

/// getConstantEvolutionLoopExitValue - If we know that the specified Phi is
/// in the header of its containing loop, we know the loop executes a
/// constant number of times, and the PHI node is just a recurrence
/// involving constants, fold it.
Constant *
ScalarEvolution::getConstantEvolutionLoopExitValue(PHINode *PN,
                                                   const APInt& BEs,
                                                   const Loop *L) {
  std::map<PHINode*, Constant*>::iterator I =
    ConstantEvolutionLoopExitValue.find(PN);
  if (I != ConstantEvolutionLoopExitValue.end())
    return I->second;

  if (BEs.ugt(APInt(BEs.getBitWidth(),MaxBruteForceIterations)))
    return ConstantEvolutionLoopExitValue[PN] = 0;  // Not going to evaluate it.

  Constant *&RetVal = ConstantEvolutionLoopExitValue[PN];

  // Since the loop is canonicalized, the PHI node must have two entries.  One
  // entry must be a constant (coming in from outside of the loop), and the
  // second must be derived from the same PHI.
  bool SecondIsBackedge = L->contains(PN->getIncomingBlock(1));
  Constant *StartCST =
    dyn_cast<Constant>(PN->getIncomingValue(!SecondIsBackedge));
  if (StartCST == 0)
    return RetVal = 0;  // Must be a constant.

  Value *BEValue = PN->getIncomingValue(SecondIsBackedge);
  PHINode *PN2 = getConstantEvolvingPHI(BEValue, L);
  if (PN2 != PN)
    return RetVal = 0;  // Not derived from same PHI.

  // Execute the loop symbolically to determine the exit value.
  if (BEs.getActiveBits() >= 32)
    return RetVal = 0; // More than 2^32-1 iterations?? Not doing it!

  unsigned NumIterations = BEs.getZExtValue(); // must be in range
  unsigned IterationNum = 0;
  for (Constant *PHIVal = StartCST; ; ++IterationNum) {
    if (IterationNum == NumIterations)
      return RetVal = PHIVal;  // Got exit value!

    // Compute the value of the PHI node for the next iteration.
    Constant *NextPHI = EvaluateExpression(BEValue, PHIVal);
    if (NextPHI == PHIVal)
      return RetVal = NextPHI;  // Stopped evolving!
    if (NextPHI == 0)
      return 0;        // Couldn't evaluate!
    PHIVal = NextPHI;
  }
}

/// ComputeBackedgeTakenCountExhaustively - If the trip is known to execute a
/// constant number of times (the condition evolves only from constants),
/// try to evaluate a few iterations of the loop until we get the exit
/// condition gets a value of ExitWhen (true or false).  If we cannot
/// evaluate the trip count of the loop, return getCouldNotCompute().
const SCEV *
ScalarEvolution::ComputeBackedgeTakenCountExhaustively(const Loop *L,
                                                       Value *Cond,
                                                       bool ExitWhen) {
  PHINode *PN = getConstantEvolvingPHI(Cond, L);
  if (PN == 0) return getCouldNotCompute();

  // Since the loop is canonicalized, the PHI node must have two entries.  One
  // entry must be a constant (coming in from outside of the loop), and the
  // second must be derived from the same PHI.
  bool SecondIsBackedge = L->contains(PN->getIncomingBlock(1));
  Constant *StartCST =
    dyn_cast<Constant>(PN->getIncomingValue(!SecondIsBackedge));
  if (StartCST == 0) return getCouldNotCompute();  // Must be a constant.

  Value *BEValue = PN->getIncomingValue(SecondIsBackedge);
  PHINode *PN2 = getConstantEvolvingPHI(BEValue, L);
  if (PN2 != PN) return getCouldNotCompute();  // Not derived from same PHI.

  // Okay, we find a PHI node that defines the trip count of this loop.  Execute
  // the loop symbolically to determine when the condition gets a value of
  // "ExitWhen".
  unsigned IterationNum = 0;
  unsigned MaxIterations = MaxBruteForceIterations;   // Limit analysis.
  for (Constant *PHIVal = StartCST;
       IterationNum != MaxIterations; ++IterationNum) {
    ConstantInt *CondVal =
      dyn_cast_or_null<ConstantInt>(EvaluateExpression(Cond, PHIVal));

    // Couldn't symbolically evaluate.
    if (!CondVal) return getCouldNotCompute();

    if (CondVal->getValue() == uint64_t(ExitWhen)) {
      ++NumBruteForceTripCountsComputed;
      return getConstant(Type::Int32Ty, IterationNum);
    }

    // Compute the value of the PHI node for the next iteration.
    Constant *NextPHI = EvaluateExpression(BEValue, PHIVal);
    if (NextPHI == 0 || NextPHI == PHIVal)
      return getCouldNotCompute();// Couldn't evaluate or not making progress...
    PHIVal = NextPHI;
  }

  // Too many iterations were needed to evaluate.
  return getCouldNotCompute();
}

/// getSCEVAtScope - Return a SCEV expression handle for the specified value
/// at the specified scope in the program.  The L value specifies a loop
/// nest to evaluate the expression at, where null is the top-level or a
/// specified loop is immediately inside of the loop.
///
/// This method can be used to compute the exit value for a variable defined
/// in a loop by querying what the value will hold in the parent loop.
///
/// In the case that a relevant loop exit value cannot be computed, the
/// original value V is returned.
const SCEV *ScalarEvolution::getSCEVAtScope(const SCEV *V, const Loop *L) {
  // FIXME: this should be turned into a virtual method on SCEV!

  if (isa<SCEVConstant>(V)) return V;

  // If this instruction is evolved from a constant-evolving PHI, compute the
  // exit value from the loop without using SCEVs.
  if (const SCEVUnknown *SU = dyn_cast<SCEVUnknown>(V)) {
    if (Instruction *I = dyn_cast<Instruction>(SU->getValue())) {
      const Loop *LI = (*this->LI)[I->getParent()];
      if (LI && LI->getParentLoop() == L)  // Looking for loop exit value.
        if (PHINode *PN = dyn_cast<PHINode>(I))
          if (PN->getParent() == LI->getHeader()) {
            // Okay, there is no closed form solution for the PHI node.  Check
            // to see if the loop that contains it has a known backedge-taken
            // count.  If so, we may be able to force computation of the exit
            // value.
            const SCEV *BackedgeTakenCount = getBackedgeTakenCount(LI);
            if (const SCEVConstant *BTCC =
                  dyn_cast<SCEVConstant>(BackedgeTakenCount)) {
              // Okay, we know how many times the containing loop executes.  If
              // this is a constant evolving PHI node, get the final value at
              // the specified iteration number.
              Constant *RV = getConstantEvolutionLoopExitValue(PN,
                                                   BTCC->getValue()->getValue(),
                                                               LI);
              if (RV) return getSCEV(RV);
            }
          }

      // Okay, this is an expression that we cannot symbolically evaluate
      // into a SCEV.  Check to see if it's possible to symbolically evaluate
      // the arguments into constants, and if so, try to constant propagate the
      // result.  This is particularly useful for computing loop exit values.
      if (CanConstantFold(I)) {
        // Check to see if we've folded this instruction at this loop before.
        std::map<const Loop *, Constant *> &Values = ValuesAtScopes[I];
        std::pair<std::map<const Loop *, Constant *>::iterator, bool> Pair =
          Values.insert(std::make_pair(L, static_cast<Constant *>(0)));
        if (!Pair.second)
          return Pair.first->second ? &*getSCEV(Pair.first->second) : V;

        std::vector<Constant*> Operands;
        Operands.reserve(I->getNumOperands());
        for (unsigned i = 0, e = I->getNumOperands(); i != e; ++i) {
          Value *Op = I->getOperand(i);
          if (Constant *C = dyn_cast<Constant>(Op)) {
            Operands.push_back(C);
          } else {
            // If any of the operands is non-constant and if they are
            // non-integer and non-pointer, don't even try to analyze them
            // with scev techniques.
            if (!isSCEVable(Op->getType()))
              return V;

            const SCEV *OpV = getSCEVAtScope(getSCEV(Op), L);
            if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(OpV)) {
              Constant *C = SC->getValue();
              if (C->getType() != Op->getType())
                C = ConstantExpr::getCast(CastInst::getCastOpcode(C, false,
                                                                  Op->getType(),
                                                                  false),
                                          C, Op->getType());
              Operands.push_back(C);
            } else if (const SCEVUnknown *SU = dyn_cast<SCEVUnknown>(OpV)) {
              if (Constant *C = dyn_cast<Constant>(SU->getValue())) {
                if (C->getType() != Op->getType())
                  C =
                    ConstantExpr::getCast(CastInst::getCastOpcode(C, false,
                                                                  Op->getType(),
                                                                  false),
                                          C, Op->getType());
                Operands.push_back(C);
              } else
                return V;
            } else {
              return V;
            }
          }
        }

        Constant *C;
        if (const CmpInst *CI = dyn_cast<CmpInst>(I))
          C = ConstantFoldCompareInstOperands(CI->getPredicate(),
                                              &Operands[0], Operands.size(),
                                              Context);
        else
          C = ConstantFoldInstOperands(I->getOpcode(), I->getType(),
                                       &Operands[0], Operands.size(), Context);
        Pair.first->second = C;
        return getSCEV(C);
      }
    }

    // This is some other type of SCEVUnknown, just return it.
    return V;
  }

  if (const SCEVCommutativeExpr *Comm = dyn_cast<SCEVCommutativeExpr>(V)) {
    // Avoid performing the look-up in the common case where the specified
    // expression has no loop-variant portions.
    for (unsigned i = 0, e = Comm->getNumOperands(); i != e; ++i) {
      const SCEV *OpAtScope = getSCEVAtScope(Comm->getOperand(i), L);
      if (OpAtScope != Comm->getOperand(i)) {
        // Okay, at least one of these operands is loop variant but might be
        // foldable.  Build a new instance of the folded commutative expression.
        SmallVector<const SCEV *, 8> NewOps(Comm->op_begin(),
                                            Comm->op_begin()+i);
        NewOps.push_back(OpAtScope);

        for (++i; i != e; ++i) {
          OpAtScope = getSCEVAtScope(Comm->getOperand(i), L);
          NewOps.push_back(OpAtScope);
        }
        if (isa<SCEVAddExpr>(Comm))
          return getAddExpr(NewOps);
        if (isa<SCEVMulExpr>(Comm))
          return getMulExpr(NewOps);
        if (isa<SCEVSMaxExpr>(Comm))
          return getSMaxExpr(NewOps);
        if (isa<SCEVUMaxExpr>(Comm))
          return getUMaxExpr(NewOps);
        LLVM_UNREACHABLE("Unknown commutative SCEV type!");
      }
    }
    // If we got here, all operands are loop invariant.
    return Comm;
  }

  if (const SCEVUDivExpr *Div = dyn_cast<SCEVUDivExpr>(V)) {
    const SCEV *LHS = getSCEVAtScope(Div->getLHS(), L);
    const SCEV *RHS = getSCEVAtScope(Div->getRHS(), L);
    if (LHS == Div->getLHS() && RHS == Div->getRHS())
      return Div;   // must be loop invariant
    return getUDivExpr(LHS, RHS);
  }

  // If this is a loop recurrence for a loop that does not contain L, then we
  // are dealing with the final value computed by the loop.
  if (const SCEVAddRecExpr *AddRec = dyn_cast<SCEVAddRecExpr>(V)) {
    if (!L || !AddRec->getLoop()->contains(L->getHeader())) {
      // To evaluate this recurrence, we need to know how many times the AddRec
      // loop iterates.  Compute this now.
      const SCEV *BackedgeTakenCount = getBackedgeTakenCount(AddRec->getLoop());
      if (BackedgeTakenCount == getCouldNotCompute()) return AddRec;

      // Then, evaluate the AddRec.
      return AddRec->evaluateAtIteration(BackedgeTakenCount, *this);
    }
    return AddRec;
  }

  if (const SCEVZeroExtendExpr *Cast = dyn_cast<SCEVZeroExtendExpr>(V)) {
    const SCEV *Op = getSCEVAtScope(Cast->getOperand(), L);
    if (Op == Cast->getOperand())
      return Cast;  // must be loop invariant
    return getZeroExtendExpr(Op, Cast->getType());
  }

  if (const SCEVSignExtendExpr *Cast = dyn_cast<SCEVSignExtendExpr>(V)) {
    const SCEV *Op = getSCEVAtScope(Cast->getOperand(), L);
    if (Op == Cast->getOperand())
      return Cast;  // must be loop invariant
    return getSignExtendExpr(Op, Cast->getType());
  }

  if (const SCEVTruncateExpr *Cast = dyn_cast<SCEVTruncateExpr>(V)) {
    const SCEV *Op = getSCEVAtScope(Cast->getOperand(), L);
    if (Op == Cast->getOperand())
      return Cast;  // must be loop invariant
    return getTruncateExpr(Op, Cast->getType());
  }

  LLVM_UNREACHABLE("Unknown SCEV type!");
  return 0;
}

/// getSCEVAtScope - This is a convenience function which does
/// getSCEVAtScope(getSCEV(V), L).
const SCEV *ScalarEvolution::getSCEVAtScope(Value *V, const Loop *L) {
  return getSCEVAtScope(getSCEV(V), L);
}

/// SolveLinEquationWithOverflow - Finds the minimum unsigned root of the
/// following equation:
///
///     A * X = B (mod N)
///
/// where N = 2^BW and BW is the common bit width of A and B. The signedness of
/// A and B isn't important.
///
/// If the equation does not have a solution, SCEVCouldNotCompute is returned.
static const SCEV *SolveLinEquationWithOverflow(const APInt &A, const APInt &B,
                                               ScalarEvolution &SE) {
  uint32_t BW = A.getBitWidth();
  assert(BW == B.getBitWidth() && "Bit widths must be the same.");
  assert(A != 0 && "A must be non-zero.");

  // 1. D = gcd(A, N)
  //
  // The gcd of A and N may have only one prime factor: 2. The number of
  // trailing zeros in A is its multiplicity
  uint32_t Mult2 = A.countTrailingZeros();
  // D = 2^Mult2

  // 2. Check if B is divisible by D.
  //
  // B is divisible by D if and only if the multiplicity of prime factor 2 for B
  // is not less than multiplicity of this prime factor for D.
  if (B.countTrailingZeros() < Mult2)
    return SE.getCouldNotCompute();

  // 3. Compute I: the multiplicative inverse of (A / D) in arithmetic
  // modulo (N / D).
  //
  // (N / D) may need BW+1 bits in its representation.  Hence, we'll use this
  // bit width during computations.
  APInt AD = A.lshr(Mult2).zext(BW + 1);  // AD = A / D
  APInt Mod(BW + 1, 0);
  Mod.set(BW - Mult2);  // Mod = N / D
  APInt I = AD.multiplicativeInverse(Mod);

  // 4. Compute the minimum unsigned root of the equation:
  // I * (B / D) mod (N / D)
  APInt Result = (I * B.lshr(Mult2).zext(BW + 1)).urem(Mod);

  // The result is guaranteed to be less than 2^BW so we may truncate it to BW
  // bits.
  return SE.getConstant(Result.trunc(BW));
}

/// SolveQuadraticEquation - Find the roots of the quadratic equation for the
/// given quadratic chrec {L,+,M,+,N}.  This returns either the two roots (which
/// might be the same) or two SCEVCouldNotCompute objects.
///
static std::pair<const SCEV *,const SCEV *>
SolveQuadraticEquation(const SCEVAddRecExpr *AddRec, ScalarEvolution &SE) {
  assert(AddRec->getNumOperands() == 3 && "This is not a quadratic chrec!");
  const SCEVConstant *LC = dyn_cast<SCEVConstant>(AddRec->getOperand(0));
  const SCEVConstant *MC = dyn_cast<SCEVConstant>(AddRec->getOperand(1));
  const SCEVConstant *NC = dyn_cast<SCEVConstant>(AddRec->getOperand(2));

  // We currently can only solve this if the coefficients are constants.
  if (!LC || !MC || !NC) {
    const SCEV *CNC = SE.getCouldNotCompute();
    return std::make_pair(CNC, CNC);
  }

  uint32_t BitWidth = LC->getValue()->getValue().getBitWidth();
  const APInt &L = LC->getValue()->getValue();
  const APInt &M = MC->getValue()->getValue();
  const APInt &N = NC->getValue()->getValue();
  APInt Two(BitWidth, 2);
  APInt Four(BitWidth, 4);

  {
    using namespace APIntOps;
    const APInt& C = L;
    // Convert from chrec coefficients to polynomial coefficients AX^2+BX+C
    // The B coefficient is M-N/2
    APInt B(M);
    B -= sdiv(N,Two);

    // The A coefficient is N/2
    APInt A(N.sdiv(Two));

    // Compute the B^2-4ac term.
    APInt SqrtTerm(B);
    SqrtTerm *= B;
    SqrtTerm -= Four * (A * C);

    // Compute sqrt(B^2-4ac). This is guaranteed to be the nearest
    // integer value or else APInt::sqrt() will assert.
    APInt SqrtVal(SqrtTerm.sqrt());

    // Compute the two solutions for the quadratic formula.
    // The divisions must be performed as signed divisions.
    APInt NegB(-B);
    APInt TwoA( A << 1 );
    if (TwoA.isMinValue()) {
      const SCEV *CNC = SE.getCouldNotCompute();
      return std::make_pair(CNC, CNC);
    }

    LLVMContext *Context = SE.getContext();

    ConstantInt *Solution1 =
      Context->getConstantInt((NegB + SqrtVal).sdiv(TwoA));
    ConstantInt *Solution2 =
      Context->getConstantInt((NegB - SqrtVal).sdiv(TwoA));

    return std::make_pair(SE.getConstant(Solution1),
                          SE.getConstant(Solution2));
    } // end APIntOps namespace
}

/// HowFarToZero - Return the number of times a backedge comparing the specified
/// value to zero will execute.  If not computable, return CouldNotCompute.
const SCEV *ScalarEvolution::HowFarToZero(const SCEV *V, const Loop *L) {
  // If the value is a constant
  if (const SCEVConstant *C = dyn_cast<SCEVConstant>(V)) {
    // If the value is already zero, the branch will execute zero times.
    if (C->getValue()->isZero()) return C;
    return getCouldNotCompute();  // Otherwise it will loop infinitely.
  }

  const SCEVAddRecExpr *AddRec = dyn_cast<SCEVAddRecExpr>(V);
  if (!AddRec || AddRec->getLoop() != L)
    return getCouldNotCompute();

  if (AddRec->isAffine()) {
    // If this is an affine expression, the execution count of this branch is
    // the minimum unsigned root of the following equation:
    //
    //     Start + Step*N = 0 (mod 2^BW)
    //
    // equivalent to:
    //
    //             Step*N = -Start (mod 2^BW)
    //
    // where BW is the common bit width of Start and Step.

    // Get the initial value for the loop.
    const SCEV *Start = getSCEVAtScope(AddRec->getStart(),
                                       L->getParentLoop());
    const SCEV *Step = getSCEVAtScope(AddRec->getOperand(1),
                                      L->getParentLoop());

    if (const SCEVConstant *StepC = dyn_cast<SCEVConstant>(Step)) {
      // For now we handle only constant steps.

      // First, handle unitary steps.
      if (StepC->getValue()->equalsInt(1))      // 1*N = -Start (mod 2^BW), so:
        return getNegativeSCEV(Start);       //   N = -Start (as unsigned)
      if (StepC->getValue()->isAllOnesValue())  // -1*N = -Start (mod 2^BW), so:
        return Start;                           //    N = Start (as unsigned)

      // Then, try to solve the above equation provided that Start is constant.
      if (const SCEVConstant *StartC = dyn_cast<SCEVConstant>(Start))
        return SolveLinEquationWithOverflow(StepC->getValue()->getValue(),
                                            -StartC->getValue()->getValue(),
                                            *this);
    }
  } else if (AddRec->isQuadratic() && AddRec->getType()->isInteger()) {
    // If this is a quadratic (3-term) AddRec {L,+,M,+,N}, find the roots of
    // the quadratic equation to solve it.
    std::pair<const SCEV *,const SCEV *> Roots = SolveQuadraticEquation(AddRec,
                                                                    *this);
    const SCEVConstant *R1 = dyn_cast<SCEVConstant>(Roots.first);
    const SCEVConstant *R2 = dyn_cast<SCEVConstant>(Roots.second);
    if (R1) {
#if 0
      errs() << "HFTZ: " << *V << " - sol#1: " << *R1
             << "  sol#2: " << *R2 << "\n";
#endif
      // Pick the smallest positive root value.
      if (ConstantInt *CB =
          dyn_cast<ConstantInt>(Context->getConstantExprICmp(ICmpInst::ICMP_ULT,
                                   R1->getValue(), R2->getValue()))) {
        if (CB->getZExtValue() == false)
          std::swap(R1, R2);   // R1 is the minimum root now.

        // We can only use this value if the chrec ends up with an exact zero
        // value at this index.  When solving for "X*X != 5", for example, we
        // should not accept a root of 2.
        const SCEV *Val = AddRec->evaluateAtIteration(R1, *this);
        if (Val->isZero())
          return R1;  // We found a quadratic root!
      }
    }
  }

  return getCouldNotCompute();
}

/// HowFarToNonZero - Return the number of times a backedge checking the
/// specified value for nonzero will execute.  If not computable, return
/// CouldNotCompute
const SCEV *ScalarEvolution::HowFarToNonZero(const SCEV *V, const Loop *L) {
  // Loops that look like: while (X == 0) are very strange indeed.  We don't
  // handle them yet except for the trivial case.  This could be expanded in the
  // future as needed.

  // If the value is a constant, check to see if it is known to be non-zero
  // already.  If so, the backedge will execute zero times.
  if (const SCEVConstant *C = dyn_cast<SCEVConstant>(V)) {
    if (!C->getValue()->isNullValue())
      return getIntegerSCEV(0, C->getType());
    return getCouldNotCompute();  // Otherwise it will loop infinitely.
  }

  // We could implement others, but I really doubt anyone writes loops like
  // this, and if they did, they would already be constant folded.
  return getCouldNotCompute();
}

/// getLoopPredecessor - If the given loop's header has exactly one unique
/// predecessor outside the loop, return it. Otherwise return null.
///
BasicBlock *ScalarEvolution::getLoopPredecessor(const Loop *L) {
  BasicBlock *Header = L->getHeader();
  BasicBlock *Pred = 0;
  for (pred_iterator PI = pred_begin(Header), E = pred_end(Header);
       PI != E; ++PI)
    if (!L->contains(*PI)) {
      if (Pred && Pred != *PI) return 0; // Multiple predecessors.
      Pred = *PI;
    }
  return Pred;
}

/// getPredecessorWithUniqueSuccessorForBB - Return a predecessor of BB
/// (which may not be an immediate predecessor) which has exactly one
/// successor from which BB is reachable, or null if no such block is
/// found.
///
BasicBlock *
ScalarEvolution::getPredecessorWithUniqueSuccessorForBB(BasicBlock *BB) {
  // If the block has a unique predecessor, then there is no path from the
  // predecessor to the block that does not go through the direct edge
  // from the predecessor to the block.
  if (BasicBlock *Pred = BB->getSinglePredecessor())
    return Pred;

  // A loop's header is defined to be a block that dominates the loop.
  // If the header has a unique predecessor outside the loop, it must be
  // a block that has exactly one successor that can reach the loop.
  if (Loop *L = LI->getLoopFor(BB))
    return getLoopPredecessor(L);

  return 0;
}

/// HasSameValue - SCEV structural equivalence is usually sufficient for
/// testing whether two expressions are equal, however for the purposes of
/// looking for a condition guarding a loop, it can be useful to be a little
/// more general, since a front-end may have replicated the controlling
/// expression.
///
static bool HasSameValue(const SCEV *A, const SCEV *B) {
  // Quick check to see if they are the same SCEV.
  if (A == B) return true;

  // Otherwise, if they're both SCEVUnknown, it's possible that they hold
  // two different instructions with the same value. Check for this case.
  if (const SCEVUnknown *AU = dyn_cast<SCEVUnknown>(A))
    if (const SCEVUnknown *BU = dyn_cast<SCEVUnknown>(B))
      if (const Instruction *AI = dyn_cast<Instruction>(AU->getValue()))
        if (const Instruction *BI = dyn_cast<Instruction>(BU->getValue()))
          if (AI->isIdenticalTo(BI))
            return true;

  // Otherwise assume they may have a different value.
  return false;
}

/// isLoopGuardedByCond - Test whether entry to the loop is protected by
/// a conditional between LHS and RHS.  This is used to help avoid max
/// expressions in loop trip counts.
bool ScalarEvolution::isLoopGuardedByCond(const Loop *L,
                                          ICmpInst::Predicate Pred,
                                          const SCEV *LHS, const SCEV *RHS) {
  // Interpret a null as meaning no loop, where there is obviously no guard
  // (interprocedural conditions notwithstanding).
  if (!L) return false;

  BasicBlock *Predecessor = getLoopPredecessor(L);
  BasicBlock *PredecessorDest = L->getHeader();

  // Starting at the loop predecessor, climb up the predecessor chain, as long
  // as there are predecessors that can be found that have unique successors
  // leading to the original header.
  for (; Predecessor;
       PredecessorDest = Predecessor,
       Predecessor = getPredecessorWithUniqueSuccessorForBB(Predecessor)) {

    BranchInst *LoopEntryPredicate =
      dyn_cast<BranchInst>(Predecessor->getTerminator());
    if (!LoopEntryPredicate ||
        LoopEntryPredicate->isUnconditional())
      continue;

    if (isNecessaryCond(LoopEntryPredicate->getCondition(), Pred, LHS, RHS,
                        LoopEntryPredicate->getSuccessor(0) != PredecessorDest))
      return true;
  }

  return false;
}

/// isNecessaryCond - Test whether the given CondValue value is a condition
/// which is at least as strict as the one described by Pred, LHS, and RHS.
bool ScalarEvolution::isNecessaryCond(Value *CondValue,
                                      ICmpInst::Predicate Pred,
                                      const SCEV *LHS, const SCEV *RHS,
                                      bool Inverse) {
  // Recursivly handle And and Or conditions.
  if (BinaryOperator *BO = dyn_cast<BinaryOperator>(CondValue)) {
    if (BO->getOpcode() == Instruction::And) {
      if (!Inverse)
        return isNecessaryCond(BO->getOperand(0), Pred, LHS, RHS, Inverse) ||
               isNecessaryCond(BO->getOperand(1), Pred, LHS, RHS, Inverse);
    } else if (BO->getOpcode() == Instruction::Or) {
      if (Inverse)
        return isNecessaryCond(BO->getOperand(0), Pred, LHS, RHS, Inverse) ||
               isNecessaryCond(BO->getOperand(1), Pred, LHS, RHS, Inverse);
    }
  }

  ICmpInst *ICI = dyn_cast<ICmpInst>(CondValue);
  if (!ICI) return false;

  // Now that we found a conditional branch that dominates the loop, check to
  // see if it is the comparison we are looking for.
  Value *PreCondLHS = ICI->getOperand(0);
  Value *PreCondRHS = ICI->getOperand(1);
  ICmpInst::Predicate Cond;
  if (Inverse)
    Cond = ICI->getInversePredicate();
  else
    Cond = ICI->getPredicate();

  if (Cond == Pred)
    ; // An exact match.
  else if (!ICmpInst::isTrueWhenEqual(Cond) && Pred == ICmpInst::ICMP_NE)
    ; // The actual condition is beyond sufficient.
  else
    // Check a few special cases.
    switch (Cond) {
    case ICmpInst::ICMP_UGT:
      if (Pred == ICmpInst::ICMP_ULT) {
        std::swap(PreCondLHS, PreCondRHS);
        Cond = ICmpInst::ICMP_ULT;
        break;
      }
      return false;
    case ICmpInst::ICMP_SGT:
      if (Pred == ICmpInst::ICMP_SLT) {
        std::swap(PreCondLHS, PreCondRHS);
        Cond = ICmpInst::ICMP_SLT;
        break;
      }
      return false;
    case ICmpInst::ICMP_NE:
      // Expressions like (x >u 0) are often canonicalized to (x != 0),
      // so check for this case by checking if the NE is comparing against
      // a minimum or maximum constant.
      if (!ICmpInst::isTrueWhenEqual(Pred))
        if (ConstantInt *CI = dyn_cast<ConstantInt>(PreCondRHS)) {
          const APInt &A = CI->getValue();
          switch (Pred) {
          case ICmpInst::ICMP_SLT:
            if (A.isMaxSignedValue()) break;
            return false;
          case ICmpInst::ICMP_SGT:
            if (A.isMinSignedValue()) break;
            return false;
          case ICmpInst::ICMP_ULT:
            if (A.isMaxValue()) break;
            return false;
          case ICmpInst::ICMP_UGT:
            if (A.isMinValue()) break;
            return false;
          default:
            return false;
          }
          Cond = ICmpInst::ICMP_NE;
          // NE is symmetric but the original comparison may not be. Swap
          // the operands if necessary so that they match below.
          if (isa<SCEVConstant>(LHS))
            std::swap(PreCondLHS, PreCondRHS);
          break;
        }
      return false;
    default:
      // We weren't able to reconcile the condition.
      return false;
    }

  if (!PreCondLHS->getType()->isInteger()) return false;

  const SCEV *PreCondLHSSCEV = getSCEV(PreCondLHS);
  const SCEV *PreCondRHSSCEV = getSCEV(PreCondRHS);
  return (HasSameValue(LHS, PreCondLHSSCEV) &&
          HasSameValue(RHS, PreCondRHSSCEV)) ||
         (HasSameValue(LHS, getNotSCEV(PreCondRHSSCEV)) &&
          HasSameValue(RHS, getNotSCEV(PreCondLHSSCEV)));
}

/// getBECount - Subtract the end and start values and divide by the step,
/// rounding up, to get the number of times the backedge is executed. Return
/// CouldNotCompute if an intermediate computation overflows.
const SCEV *ScalarEvolution::getBECount(const SCEV *Start,
                                       const SCEV *End,
                                       const SCEV *Step) {
  const Type *Ty = Start->getType();
  const SCEV *NegOne = getIntegerSCEV(-1, Ty);
  const SCEV *Diff = getMinusSCEV(End, Start);
  const SCEV *RoundUp = getAddExpr(Step, NegOne);

  // Add an adjustment to the difference between End and Start so that
  // the division will effectively round up.
  const SCEV *Add = getAddExpr(Diff, RoundUp);

  // Check Add for unsigned overflow.
  // TODO: More sophisticated things could be done here.
  const Type *WideTy = Context->getIntegerType(getTypeSizeInBits(Ty) + 1);
  const SCEV *OperandExtendedAdd =
    getAddExpr(getZeroExtendExpr(Diff, WideTy),
               getZeroExtendExpr(RoundUp, WideTy));
  if (getZeroExtendExpr(Add, WideTy) != OperandExtendedAdd)
    return getCouldNotCompute();

  return getUDivExpr(Add, Step);
}

/// HowManyLessThans - Return the number of times a backedge containing the
/// specified less-than comparison will execute.  If not computable, return
/// CouldNotCompute.
ScalarEvolution::BackedgeTakenInfo
ScalarEvolution::HowManyLessThans(const SCEV *LHS, const SCEV *RHS,
                                  const Loop *L, bool isSigned) {
  // Only handle:  "ADDREC < LoopInvariant".
  if (!RHS->isLoopInvariant(L)) return getCouldNotCompute();

  const SCEVAddRecExpr *AddRec = dyn_cast<SCEVAddRecExpr>(LHS);
  if (!AddRec || AddRec->getLoop() != L)
    return getCouldNotCompute();

  if (AddRec->isAffine()) {
    // FORNOW: We only support unit strides.
    unsigned BitWidth = getTypeSizeInBits(AddRec->getType());
    const SCEV *Step = AddRec->getStepRecurrence(*this);

    // TODO: handle non-constant strides.
    const SCEVConstant *CStep = dyn_cast<SCEVConstant>(Step);
    if (!CStep || CStep->isZero())
      return getCouldNotCompute();
    if (CStep->isOne()) {
      // With unit stride, the iteration never steps past the limit value.
    } else if (CStep->getValue()->getValue().isStrictlyPositive()) {
      if (const SCEVConstant *CLimit = dyn_cast<SCEVConstant>(RHS)) {
        // Test whether a positive iteration iteration can step past the limit
        // value and past the maximum value for its type in a single step.
        if (isSigned) {
          APInt Max = APInt::getSignedMaxValue(BitWidth);
          if ((Max - CStep->getValue()->getValue())
                .slt(CLimit->getValue()->getValue()))
            return getCouldNotCompute();
        } else {
          APInt Max = APInt::getMaxValue(BitWidth);
          if ((Max - CStep->getValue()->getValue())
                .ult(CLimit->getValue()->getValue()))
            return getCouldNotCompute();
        }
      } else
        // TODO: handle non-constant limit values below.
        return getCouldNotCompute();
    } else
      // TODO: handle negative strides below.
      return getCouldNotCompute();

    // We know the LHS is of the form {n,+,s} and the RHS is some loop-invariant
    // m.  So, we count the number of iterations in which {n,+,s} < m is true.
    // Note that we cannot simply return max(m-n,0)/s because it's not safe to
    // treat m-n as signed nor unsigned due to overflow possibility.

    // First, we get the value of the LHS in the first iteration: n
    const SCEV *Start = AddRec->getOperand(0);

    // Determine the minimum constant start value.
    const SCEV *MinStart = isa<SCEVConstant>(Start) ? Start :
      getConstant(isSigned ? APInt::getSignedMinValue(BitWidth) :
                             APInt::getMinValue(BitWidth));

    // If we know that the condition is true in order to enter the loop,
    // then we know that it will run exactly (m-n)/s times. Otherwise, we
    // only know that it will execute (max(m,n)-n)/s times. In both cases,
    // the division must round up.
    const SCEV *End = RHS;
    if (!isLoopGuardedByCond(L,
                             isSigned ? ICmpInst::ICMP_SLT : ICmpInst::ICMP_ULT,
                             getMinusSCEV(Start, Step), RHS))
      End = isSigned ? getSMaxExpr(RHS, Start)
                     : getUMaxExpr(RHS, Start);

    // Determine the maximum constant end value.
    const SCEV *MaxEnd =
      isa<SCEVConstant>(End) ? End :
      getConstant(isSigned ? APInt::getSignedMaxValue(BitWidth)
                               .ashr(GetMinSignBits(End) - 1) :
                             APInt::getMaxValue(BitWidth)
                               .lshr(GetMinLeadingZeros(End)));

    // Finally, we subtract these two values and divide, rounding up, to get
    // the number of times the backedge is executed.
    const SCEV *BECount = getBECount(Start, End, Step);

    // The maximum backedge count is similar, except using the minimum start
    // value and the maximum end value.
    const SCEV *MaxBECount = getBECount(MinStart, MaxEnd, Step);

    return BackedgeTakenInfo(BECount, MaxBECount);
  }

  return getCouldNotCompute();
}

/// getNumIterationsInRange - Return the number of iterations of this loop that
/// produce values in the specified constant range.  Another way of looking at
/// this is that it returns the first iteration number where the value is not in
/// the condition, thus computing the exit count. If the iteration count can't
/// be computed, an instance of SCEVCouldNotCompute is returned.
const SCEV *SCEVAddRecExpr::getNumIterationsInRange(ConstantRange Range,
                                                    ScalarEvolution &SE) const {
  if (Range.isFullSet())  // Infinite loop.
    return SE.getCouldNotCompute();

  // If the start is a non-zero constant, shift the range to simplify things.
  if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(getStart()))
    if (!SC->getValue()->isZero()) {
      SmallVector<const SCEV *, 4> Operands(op_begin(), op_end());
      Operands[0] = SE.getIntegerSCEV(0, SC->getType());
      const SCEV *Shifted = SE.getAddRecExpr(Operands, getLoop());
      if (const SCEVAddRecExpr *ShiftedAddRec =
            dyn_cast<SCEVAddRecExpr>(Shifted))
        return ShiftedAddRec->getNumIterationsInRange(
                           Range.subtract(SC->getValue()->getValue()), SE);
      // This is strange and shouldn't happen.
      return SE.getCouldNotCompute();
    }

  // The only time we can solve this is when we have all constant indices.
  // Otherwise, we cannot determine the overflow conditions.
  for (unsigned i = 0, e = getNumOperands(); i != e; ++i)
    if (!isa<SCEVConstant>(getOperand(i)))
      return SE.getCouldNotCompute();


  // Okay at this point we know that all elements of the chrec are constants and
  // that the start element is zero.

  // First check to see if the range contains zero.  If not, the first
  // iteration exits.
  unsigned BitWidth = SE.getTypeSizeInBits(getType());
  if (!Range.contains(APInt(BitWidth, 0)))
    return SE.getIntegerSCEV(0, getType());

  if (isAffine()) {
    // If this is an affine expression then we have this situation:
    //   Solve {0,+,A} in Range  ===  Ax in Range

    // We know that zero is in the range.  If A is positive then we know that
    // the upper value of the range must be the first possible exit value.
    // If A is negative then the lower of the range is the last possible loop
    // value.  Also note that we already checked for a full range.
    APInt One(BitWidth,1);
    APInt A     = cast<SCEVConstant>(getOperand(1))->getValue()->getValue();
    APInt End = A.sge(One) ? (Range.getUpper() - One) : Range.getLower();

    // The exit value should be (End+A)/A.
    APInt ExitVal = (End + A).udiv(A);
    ConstantInt *ExitValue = SE.getContext()->getConstantInt(ExitVal);

    // Evaluate at the exit value.  If we really did fall out of the valid
    // range, then we computed our trip count, otherwise wrap around or other
    // things must have happened.
    ConstantInt *Val = EvaluateConstantChrecAtConstant(this, ExitValue, SE);
    if (Range.contains(Val->getValue()))
      return SE.getCouldNotCompute();  // Something strange happened

    // Ensure that the previous value is in the range.  This is a sanity check.
    assert(Range.contains(
           EvaluateConstantChrecAtConstant(this,
           SE.getContext()->getConstantInt(ExitVal - One), SE)->getValue()) &&
           "Linear scev computation is off in a bad way!");
    return SE.getConstant(ExitValue);
  } else if (isQuadratic()) {
    // If this is a quadratic (3-term) AddRec {L,+,M,+,N}, find the roots of the
    // quadratic equation to solve it.  To do this, we must frame our problem in
    // terms of figuring out when zero is crossed, instead of when
    // Range.getUpper() is crossed.
    SmallVector<const SCEV *, 4> NewOps(op_begin(), op_end());
    NewOps[0] = SE.getNegativeSCEV(SE.getConstant(Range.getUpper()));
    const SCEV *NewAddRec = SE.getAddRecExpr(NewOps, getLoop());

    // Next, solve the constructed addrec
    std::pair<const SCEV *,const SCEV *> Roots =
      SolveQuadraticEquation(cast<SCEVAddRecExpr>(NewAddRec), SE);
    const SCEVConstant *R1 = dyn_cast<SCEVConstant>(Roots.first);
    const SCEVConstant *R2 = dyn_cast<SCEVConstant>(Roots.second);
    if (R1) {
      // Pick the smallest positive root value.
      if (ConstantInt *CB =
          dyn_cast<ConstantInt>(
                       SE.getContext()->getConstantExprICmp(ICmpInst::ICMP_ULT,
                         R1->getValue(), R2->getValue()))) {
        if (CB->getZExtValue() == false)
          std::swap(R1, R2);   // R1 is the minimum root now.

        // Make sure the root is not off by one.  The returned iteration should
        // not be in the range, but the previous one should be.  When solving
        // for "X*X < 5", for example, we should not return a root of 2.
        ConstantInt *R1Val = EvaluateConstantChrecAtConstant(this,
                                                             R1->getValue(),
                                                             SE);
        if (Range.contains(R1Val->getValue())) {
          // The next iteration must be out of the range...
          ConstantInt *NextVal =
                 SE.getContext()->getConstantInt(R1->getValue()->getValue()+1);

          R1Val = EvaluateConstantChrecAtConstant(this, NextVal, SE);
          if (!Range.contains(R1Val->getValue()))
            return SE.getConstant(NextVal);
          return SE.getCouldNotCompute();  // Something strange happened
        }

        // If R1 was not in the range, then it is a good return value.  Make
        // sure that R1-1 WAS in the range though, just in case.
        ConstantInt *NextVal =
                 SE.getContext()->getConstantInt(R1->getValue()->getValue()-1);
        R1Val = EvaluateConstantChrecAtConstant(this, NextVal, SE);
        if (Range.contains(R1Val->getValue()))
          return R1;
        return SE.getCouldNotCompute();  // Something strange happened
      }
    }
  }

  return SE.getCouldNotCompute();
}



//===----------------------------------------------------------------------===//
//                   SCEVCallbackVH Class Implementation
//===----------------------------------------------------------------------===//

void ScalarEvolution::SCEVCallbackVH::deleted() {
  assert(SE && "SCEVCallbackVH called with a non-null ScalarEvolution!");
  if (PHINode *PN = dyn_cast<PHINode>(getValPtr()))
    SE->ConstantEvolutionLoopExitValue.erase(PN);
  if (Instruction *I = dyn_cast<Instruction>(getValPtr()))
    SE->ValuesAtScopes.erase(I);
  SE->Scalars.erase(getValPtr());
  // this now dangles!
}

void ScalarEvolution::SCEVCallbackVH::allUsesReplacedWith(Value *) {
  assert(SE && "SCEVCallbackVH called with a non-null ScalarEvolution!");

  // Forget all the expressions associated with users of the old value,
  // so that future queries will recompute the expressions using the new
  // value.
  SmallVector<User *, 16> Worklist;
  Value *Old = getValPtr();
  bool DeleteOld = false;
  for (Value::use_iterator UI = Old->use_begin(), UE = Old->use_end();
       UI != UE; ++UI)
    Worklist.push_back(*UI);
  while (!Worklist.empty()) {
    User *U = Worklist.pop_back_val();
    // Deleting the Old value will cause this to dangle. Postpone
    // that until everything else is done.
    if (U == Old) {
      DeleteOld = true;
      continue;
    }
    if (PHINode *PN = dyn_cast<PHINode>(U))
      SE->ConstantEvolutionLoopExitValue.erase(PN);
    if (Instruction *I = dyn_cast<Instruction>(U))
      SE->ValuesAtScopes.erase(I);
    if (SE->Scalars.erase(U))
      for (Value::use_iterator UI = U->use_begin(), UE = U->use_end();
           UI != UE; ++UI)
        Worklist.push_back(*UI);
  }
  if (DeleteOld) {
    if (PHINode *PN = dyn_cast<PHINode>(Old))
      SE->ConstantEvolutionLoopExitValue.erase(PN);
    if (Instruction *I = dyn_cast<Instruction>(Old))
      SE->ValuesAtScopes.erase(I);
    SE->Scalars.erase(Old);
    // this now dangles!
  }
  // this may dangle!
}

ScalarEvolution::SCEVCallbackVH::SCEVCallbackVH(Value *V, ScalarEvolution *se)
  : CallbackVH(V), SE(se) {}

//===----------------------------------------------------------------------===//
//                   ScalarEvolution Class Implementation
//===----------------------------------------------------------------------===//

ScalarEvolution::ScalarEvolution()
  : FunctionPass(&ID) {
}

bool ScalarEvolution::runOnFunction(Function &F) {
  this->F = &F;
  LI = &getAnalysis<LoopInfo>();
  TD = getAnalysisIfAvailable<TargetData>();
  return false;
}

void ScalarEvolution::releaseMemory() {
  Scalars.clear();
  BackedgeTakenCounts.clear();
  ConstantEvolutionLoopExitValue.clear();
  ValuesAtScopes.clear();
  UniqueSCEVs.clear();
  SCEVAllocator.Reset();
}

void ScalarEvolution::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequiredTransitive<LoopInfo>();
}

bool ScalarEvolution::hasLoopInvariantBackedgeTakenCount(const Loop *L) {
  return !isa<SCEVCouldNotCompute>(getBackedgeTakenCount(L));
}

static void PrintLoopInfo(raw_ostream &OS, ScalarEvolution *SE,
                          const Loop *L) {
  // Print all inner loops first
  for (Loop::iterator I = L->begin(), E = L->end(); I != E; ++I)
    PrintLoopInfo(OS, SE, *I);

  OS << "Loop " << L->getHeader()->getName() << ": ";

  SmallVector<BasicBlock*, 8> ExitBlocks;
  L->getExitBlocks(ExitBlocks);
  if (ExitBlocks.size() != 1)
    OS << "<multiple exits> ";

  if (SE->hasLoopInvariantBackedgeTakenCount(L)) {
    OS << "backedge-taken count is " << *SE->getBackedgeTakenCount(L);
  } else {
    OS << "Unpredictable backedge-taken count. ";
  }

  OS << "\n";
  OS << "Loop " << L->getHeader()->getName() << ": ";

  if (!isa<SCEVCouldNotCompute>(SE->getMaxBackedgeTakenCount(L))) {
    OS << "max backedge-taken count is " << *SE->getMaxBackedgeTakenCount(L);
  } else {
    OS << "Unpredictable max backedge-taken count. ";
  }

  OS << "\n";
}

void ScalarEvolution::print(raw_ostream &OS, const Module* ) const {
  // ScalarEvolution's implementaiton of the print method is to print
  // out SCEV values of all instructions that are interesting. Doing
  // this potentially causes it to create new SCEV objects though,
  // which technically conflicts with the const qualifier. This isn't
  // observable from outside the class though, so casting away the
  // const isn't dangerous.
  ScalarEvolution &SE = *const_cast<ScalarEvolution*>(this);

  OS << "Classifying expressions for: " << F->getName() << "\n";
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I)
    if (isSCEVable(I->getType())) {
      OS << *I;
      OS << "  -->  ";
      const SCEV *SV = SE.getSCEV(&*I);
      SV->print(OS);

      const Loop *L = LI->getLoopFor((*I).getParent());

      const SCEV *AtUse = SE.getSCEVAtScope(SV, L);
      if (AtUse != SV) {
        OS << "  -->  ";
        AtUse->print(OS);
      }

      if (L) {
        OS << "\t\t" "Exits: ";
        const SCEV *ExitValue = SE.getSCEVAtScope(SV, L->getParentLoop());
        if (!ExitValue->isLoopInvariant(L)) {
          OS << "<<Unknown>>";
        } else {
          OS << *ExitValue;
        }
      }

      OS << "\n";
    }

  OS << "Determining loop execution counts for: " << F->getName() << "\n";
  for (LoopInfo::iterator I = LI->begin(), E = LI->end(); I != E; ++I)
    PrintLoopInfo(OS, &SE, *I);
}

void ScalarEvolution::print(std::ostream &o, const Module *M) const {
  raw_os_ostream OS(o);
  print(OS, M);
}
