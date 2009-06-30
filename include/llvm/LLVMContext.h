//===-- llvm/LLVMContext.h - Class for managing "global" state --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LLVMCONTEXT_H
#define LLVM_LLVMCONTEXT_H

#include "llvm/Support/DataTypes.h"
#include <vector>
#include <string>

namespace llvm {

class LLVMContextImpl;
class Constant;
class ConstantInt;
class ConstantPointerNull;
class ConstantStruct;
class ConstantAggregateZero;
class ConstantArray;
class ConstantFP;
class ConstantVector;
class IntegerType;
class PointerType;
class StructType;
class ArrayType;
class VectorType;
class Type;
class APInt;
class APFloat;
class Value;

class LLVMContext {
  LLVMContextImpl* pImpl;
public:
  LLVMContext();
  ~LLVMContext();
  
  // ConstantInt accessors
  ConstantInt* getConstantIntTrue();
  ConstantInt* getConstantIntFalse();
  ConstantInt* getConstantInt(const IntegerType* Ty, uint64_t V,
                              bool isSigned = false);
  ConstantInt* getConstantIntSigned(const IntegerType* Ty, int64_t V);
  ConstantInt* getConstantInt(const APInt& V);
  Constant* getConstantInt(const Type* Ty, const APInt& V);
  ConstantInt* getAllOnesConstantInt(const Type* Ty);
  
  // ConstantPointerNull accessors
  ConstantPointerNull* getConstantPointerNull(const PointerType* T);
  
  // ConstantStruct accessors
  Constant* getConstantStruct(const StructType* T,
                              const std::vector<Constant*>& V);
  Constant* getConstantStruct(const std::vector<Constant*>& V,
                              bool Packed = false);
  Constant* getConstantStruct(Constant* const *Vals, unsigned NumVals,
                              bool Packed = false);
                              
  // ConstantAggregateZero accessors
  ConstantAggregateZero* getConstantAggregateZero(const Type* Ty);
  
  // ConstantArray accessors
  Constant* getConstantArray(const ArrayType* T,
                             const std::vector<Constant*>& V);
  Constant* getConstantArray(const ArrayType* T, Constant* const* Vals,
                             unsigned NumVals);
  Constant* getConstantArray(const std::string& Initializer,
                             bool AddNull = false);
                             
  // ConstantExpr accessors
  Constant* getConstantExpr(unsigned Opcode, Constant* C1, Constant* C2);
  Constant* getConstantExprTrunc(Constant* C, const Type* Ty);
  Constant* getConstantExprSExt(Constant* C, const Type* Ty);
  Constant* getConstantExprZExt(Constant* C, const Type* Ty);
  Constant* getConstantExprFPTrunc(Constant* C, const Type* Ty);
  Constant* getConstantExprFPExtend(Constant* C, const Type* Ty);
  Constant* getConstantExprUIToFP(Constant* C, const Type* Ty);
  Constant* getConstantExprSIToFP(Constant* C, const Type* Ty);
  Constant* getConstantExprFPToUI(Constant* C, const Type* Ty);
  Constant* getConstantExprFPToSI(Constant* C, const Type* Ty);
  Constant* getConstantExprPtrToInt(Constant* C, const Type* Ty);
  Constant* getConstantExprIntToPtr(Constant* C, const Type* Ty);
  Constant* getConstantExprBitCast(Constant* C, const Type* Ty);
  Constant* getConstantExprCast(unsigned ops, Constant* C, const Type* Ty);
  Constant* getConstantExprZExtOrBitCast(Constant* C, const Type* Ty);
  Constant* getConstantExprSExtOrBitCast(Constant* C, const Type* Ty);
  Constant* getConstantExprTruncOrBitCast(Constant* C, const Type* Ty);
  Constant* getConstantExprPointerCast(Constant* C, const Type* Ty);
  Constant* getConstantExprIntegerCast(Constant* C, const Type* Ty,
                                       bool isSigned);
  Constant* getConstantExprFPCast(Constant* C, const Type* Ty);
  Constant* getConstantExprSelect(Constant* C, Constant* V1, Constant* V2);
  Constant* getConstantExprAlignOf(const Type* Ty);
  Constant* getConstantExprCompare(unsigned short pred,
                                   Constant* C1, Constant* C2);
  Constant* getConstantExprNeg(Constant* C);
  Constant* getConstantExprFNeg(Constant* C);
  Constant* getConstantExprNot(Constant* C);
  Constant* getConstantExprAdd(Constant* C1, Constant* C2);
  Constant* getConstantExprFAdd(Constant* C1, Constant* C2);
  Constant* getConstantExprSub(Constant* C1, Constant* C2);
  Constant* getConstantExprFSub(Constant* C1, Constant* C2);
  Constant* getConstantExprMul(Constant* C1, Constant* C2);
  Constant* getConstantExprFMul(Constant* C1, Constant* C2);
  Constant* getConstantExprUDiv(Constant* C1, Constant* C2);
  Constant* getConstantExprSDiv(Constant* C1, Constant* C2);
  Constant* getConstantExprFDiv(Constant* C1, Constant* C2);
  Constant* getConstantExprURem(Constant* C1, Constant* C2);
  Constant* getConstantExprSRem(Constant* C1, Constant* C2);
  Constant* getConstantExprFRem(Constant* C1, Constant* C2);
  Constant* getConstantExprAnd(Constant* C1, Constant* C2);
  Constant* getConstantExprOr(Constant* C1, Constant* C2);
  Constant* getConstantExprXor(Constant* C1, Constant* C2);
  Constant* getConstantExprICmp(unsigned short pred, Constant* LHS,
                                Constant* RHS);
  Constant* getConstantExprFCmp(unsigned short pred, Constant* LHS,
                                Constant* RHS);
  Constant* getConstantExprVICmp(unsigned short pred, Constant* LHS,
                                 Constant* RHS);
  Constant* getConstantExprVFCmp(unsigned short pred, Constant* LHS,
                                 Constant* RHS);
  Constant* getConstantExprShl(Constant* C1, Constant* C2);
  Constant* getConstantExprLShr(Constant* C1, Constant* C2);
  Constant* getConstantExprAShr(Constant* C1, Constant* C2);
  Constant* getConstantExprGetElementPtr(Constant* C, Constant* const* IdxList, 
                                         unsigned NumIdx);
  Constant* getConstantExprGetElementPtr(Constant* C, Value* const* IdxList, 
                                          unsigned NumIdx);
  Constant* getConstantExprExtractElement(Constant* Vec, Constant* Idx);
  Constant* getConstantExprInsertElement(Constant* Vec, Constant* Elt,
                                         Constant* Idx);
  Constant* getConstantExprShuffleVector(Constant* V1, Constant* V2,
                                         Constant* Mask);
  Constant* getConstantExprExtractValue(Constant* Agg, const unsigned* IdxList, 
                                        unsigned NumIdx);
  Constant* getConstantExprInsertValue(Constant* Agg, Constant* Val,
                                       const unsigned* IdxList,
                                       unsigned NumIdx);
  Constant* getZeroValueForNegation(const Type* Ty);
  
  // ConstantFP accessors
  ConstantFP* getConstantFP(const APFloat& V);
  Constant* getConstantFP(const Type* Ty, double V);
  ConstantFP* getConstantFPNegativeZero(const Type* Ty);
  
  // ConstantVector accessors
  Constant* getConstantVector(const VectorType* T,
                              const std::vector<Constant*>& V);
  Constant* getConstantVector(const std::vector<Constant*>& V);
  Constant* getConstantVector(Constant* const* Vals, unsigned NumVals);
  ConstantVector* getConstantVectorAllOnes(const VectorType* Ty);
};

}

#endif
