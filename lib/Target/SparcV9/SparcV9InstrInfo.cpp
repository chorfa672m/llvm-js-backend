//===-- SparcInstrInfo.cpp ------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "SparcInternals.h"
#include "SparcInstrSelectionSupport.h"
#include "llvm/CodeGen/InstrSelection.h"
#include "llvm/CodeGen/InstrSelectionSupport.h"
#include "llvm/CodeGen/MachineCodeForMethod.h"
#include "llvm/CodeGen/MachineCodeForInstruction.h"
#include "llvm/Function.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
using std::vector;

static const uint32_t MAXLO   = (1 << 10) - 1; // set bits set by %lo(*)
static const uint32_t MAXSIMM = (1 << 12) - 1; // set bits in simm13 field of OR


//----------------------------------------------------------------------------
// Function: CreateSETUWConst
// 
// Set a 32-bit unsigned constant in the register `dest', using
// SETHI, OR in the worst case.  This function correctly emulates
// the SETUW pseudo-op for SPARC v9 (if argument isSigned == false).
//
// The isSigned=true case is used to implement SETSW without duplicating code.
// 
// Optimize some common cases:
// (1) Small value that fits in simm13 field of OR: don't need SETHI.
// (2) isSigned = true and C is a small negative signed value, i.e.,
//     high bits are 1, and the remaining bits fit in simm13(OR).
//----------------------------------------------------------------------------

static inline void
CreateSETUWConst(const TargetMachine& target, uint32_t C,
                 Instruction* dest, vector<MachineInstr*>& mvec,
                 bool isSigned = false)
{
  MachineInstr *miSETHI = NULL, *miOR = NULL;

  // In order to get efficient code, we should not generate the SETHI if
  // all high bits are 1 (i.e., this is a small signed value that fits in
  // the simm13 field of OR).  So we check for and handle that case specially.
  // NOTE: The value C = 0x80000000 is bad: sC < 0 *and* -sC < 0.
  //       In fact, sC == -sC, so we have to check for this explicitly.
  int32_t sC = (int32_t) C;
  bool smallNegValue =isSigned && sC < 0 && sC != -sC && -sC < (int32_t)MAXSIMM;

  // Set the high 22 bits in dest if non-zero and simm13 field of OR not enough
  if (!smallNegValue && (C & ~MAXLO) && C > MAXSIMM)
    {
      miSETHI = Create2OperandInstr_UImmed(SETHI, C, dest);
      miSETHI->setOperandHi32(0);
      mvec.push_back(miSETHI);
    }
  
  // Set the low 10 or 12 bits in dest.  This is necessary if no SETHI
  // was generated, or if the low 10 bits are non-zero.
  if (miSETHI==NULL || C & MAXLO)
    {
      if (miSETHI)
        { // unsigned value with high-order bits set using SETHI
          miOR = Create3OperandInstr_UImmed(OR, dest, C, dest);
          miOR->setOperandLo32(1);
        }
      else
        { // unsigned or small signed value that fits in simm13 field of OR
          assert(smallNegValue || (C & ~MAXSIMM) == 0);
          miOR = new MachineInstr(OR);
          miOR->SetMachineOperandReg(0, target.getRegInfo().getZeroRegNum());
          miOR->SetMachineOperandConst(1, MachineOperand::MO_SignExtendedImmed,
                                       sC);
          miOR->SetMachineOperandVal(2,MachineOperand::MO_VirtualRegister,dest);
        }
      mvec.push_back(miOR);
    }
  
  assert((miSETHI || miOR) && "Oops, no code was generated!");
}


//----------------------------------------------------------------------------
// Function: CreateSETSWConst
// 
// Set a 32-bit signed constant in the register `dest', with sign-extension
// to 64 bits.  This uses SETHI, OR, SRA in the worst case.
// This function correctly emulates the SETSW pseudo-op for SPARC v9.
//
// Optimize the same cases as SETUWConst, plus:
// (1) SRA is not needed for positive or small negative values.
//----------------------------------------------------------------------------

static inline void
CreateSETSWConst(const TargetMachine& target, int32_t C,
                 Instruction* dest, vector<MachineInstr*>& mvec)
{
  MachineInstr* MI;

  // Set the low 32 bits of dest
  CreateSETUWConst(target, (uint32_t) C,  dest, mvec, /*isSigned*/true);

  // Sign-extend to the high 32 bits if needed
  if (C < 0 && (-C) > (int32_t) MAXSIMM)
    {
      MI = Create3OperandInstr_UImmed(SRA, dest, 0, dest);
      mvec.push_back(MI);
    }
}


//----------------------------------------------------------------------------
// Function: CreateSETXConst
// 
// Set a 64-bit signed or unsigned constant in the register `dest'.
// Use SETUWConst for each 32 bit word, plus a left-shift-by-32 in between.
// This function correctly emulates the SETX pseudo-op for SPARC v9.
//
// Optimize the same cases as SETUWConst for each 32 bit word.
//----------------------------------------------------------------------------

static inline void
CreateSETXConst(const TargetMachine& target, uint64_t C,
                Instruction* tmpReg, Instruction* dest,
                vector<MachineInstr*>& mvec)
{
  assert(C > (unsigned int) ~0 && "Use SETUW/SETSW for 32-bit values!");
  
  MachineInstr* MI;
  
  // Code to set the upper 32 bits of the value in register `tmpReg'
  CreateSETUWConst(target, (C >> 32), tmpReg, mvec);
  
  // Shift tmpReg left by 32 bits
  MI = Create3OperandInstr_UImmed(SLLX, tmpReg, 32, tmpReg);
  mvec.push_back(MI);
  
  // Code to set the low 32 bits of the value in register `dest'
  CreateSETUWConst(target, C, dest, mvec);
  
  // dest = OR(tmpReg, dest)
  MI = Create3OperandInstr(OR, dest, tmpReg, dest);
  mvec.push_back(MI);
}


//----------------------------------------------------------------------------
// Function: CreateSETUWLabel
// 
// Set a 32-bit constant (given by a symbolic label) in the register `dest'.
//----------------------------------------------------------------------------

static inline void
CreateSETUWLabel(const TargetMachine& target, Value* val,
                 Instruction* dest, vector<MachineInstr*>& mvec)
{
  MachineInstr* MI;
  
  // Set the high 22 bits in dest
  MI = Create2OperandInstr(SETHI, val, dest);
  MI->setOperandHi32(0);
  mvec.push_back(MI);
  
  // Set the low 10 bits in dest
  MI = Create3OperandInstr(OR, dest, val, dest);
  MI->setOperandLo32(1);
  mvec.push_back(MI);
}


//----------------------------------------------------------------------------
// Function: CreateSETXLabel
// 
// Set a 64-bit constant (given by a symbolic label) in the register `dest'.
//----------------------------------------------------------------------------

static inline void
CreateSETXLabel(const TargetMachine& target,
                Value* val, Instruction* tmpReg, Instruction* dest,
                vector<MachineInstr*>& mvec)
{
  assert(isa<Constant>(val) || isa<GlobalValue>(val) &&
         "I only know about constant values and global addresses");
  
  MachineInstr* MI;
  
  MI = Create2OperandInstr_Addr(SETHI, val, tmpReg);
  MI->setOperandHi64(0);
  mvec.push_back(MI);
  
  MI = Create3OperandInstr_Addr(OR, tmpReg, val, tmpReg);
  MI->setOperandLo64(1);
  mvec.push_back(MI);
  
  MI = Create3OperandInstr_UImmed(SLLX, tmpReg, 32, tmpReg);
  mvec.push_back(MI);
  
  MI = Create2OperandInstr_Addr(SETHI, val, dest);
  MI->setOperandHi32(0);
  mvec.push_back(MI);
  
  MI = Create3OperandInstr(OR, dest, tmpReg, dest);
  mvec.push_back(MI);
  
  MI = Create3OperandInstr_Addr(OR, dest, val, dest);
  MI->setOperandLo32(1);
  mvec.push_back(MI);
}


//----------------------------------------------------------------------------
// Function: CreateUIntSetInstruction
// 
// Create code to Set an unsigned constant in the register `dest'.
// Uses CreateSETUWConst, CreateSETSWConst or CreateSETXConst as needed.
// CreateSETSWConst is an optimization for the case that the unsigned value
// has all ones in the 33 high bits (so that sign-extension sets them all).
//----------------------------------------------------------------------------

static inline void
CreateUIntSetInstruction(const TargetMachine& target,
                         uint64_t C, Instruction* dest,
                         std::vector<MachineInstr*>& mvec,
                         MachineCodeForInstruction& mcfi)
{
  static const uint64_t lo32 = (uint32_t) ~0;
  if (C <= lo32)                        // High 32 bits are 0.  Set low 32 bits.
    CreateSETUWConst(target, (uint32_t) C, dest, mvec);
  else if ((C & ~lo32) == ~lo32 && (C & (1 << 31)))
    { // All high 33 (not 32) bits are 1s: sign-extension will take care
      // of high 32 bits, so use the sequence for signed int
      CreateSETSWConst(target, (int32_t) C, dest, mvec);
    }
  else if (C > lo32)
    { // C does not fit in 32 bits
      TmpInstruction* tmpReg = new TmpInstruction(Type::IntTy);
      mcfi.addTemp(tmpReg);
      CreateSETXConst(target, C, tmpReg, dest, mvec);
    }
}


//----------------------------------------------------------------------------
// Function: CreateIntSetInstruction
// 
// Create code to Set a signed constant in the register `dest'.
// Really the same as CreateUIntSetInstruction.
//----------------------------------------------------------------------------

static inline void
CreateIntSetInstruction(const TargetMachine& target,
                        int64_t C, Instruction* dest,
                        std::vector<MachineInstr*>& mvec,
                        MachineCodeForInstruction& mcfi)
{
  CreateUIntSetInstruction(target, (uint64_t) C, dest, mvec, mcfi);
}


//---------------------------------------------------------------------------
// class UltraSparcInstrInfo 
// 
// Purpose:
//   Information about individual instructions.
//   Most information is stored in the SparcMachineInstrDesc array above.
//   Other information is computed on demand, and most such functions
//   default to member functions in base class MachineInstrInfo. 
//---------------------------------------------------------------------------

/*ctor*/
UltraSparcInstrInfo::UltraSparcInstrInfo(const TargetMachine& tgt)
  : MachineInstrInfo(tgt, SparcMachineInstrDesc,
		     /*descSize = */ NUM_TOTAL_OPCODES,
		     /*numRealOpCodes = */ NUM_REAL_OPCODES)
{
}

// 
// Create an instruction sequence to put the constant `val' into
// the virtual register `dest'.  `val' may be a Constant or a
// GlobalValue, viz., the constant address of a global variable or function.
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineCodeForMethod.
// 
void
UltraSparcInstrInfo::CreateCodeToLoadConst(const TargetMachine& target,
                                           Function* F,
                                           Value* val,
                                           Instruction* dest,
                                           vector<MachineInstr*>& mvec,
                                       MachineCodeForInstruction& mcfi) const
{
  assert(isa<Constant>(val) || isa<GlobalValue>(val) &&
         "I only know about constant values and global addresses");
  
  // Use a "set" instruction for known constants or symbolic constants (labels)
  // that can go in an integer reg.
  // We have to use a "load" instruction for all other constants,
  // in particular, floating point constants.
  // 
  const Type* valType = val->getType();
  
  if (isa<GlobalValue>(val))
    {
      TmpInstruction* tmpReg =
        new TmpInstruction(PointerType::get(val->getType()), val);
      mcfi.addTemp(tmpReg);
      CreateSETXLabel(target, val, tmpReg, dest, mvec);
    }
  else if (valType->isIntegral())
    {
      bool isValidConstant;
      unsigned opSize = target.DataLayout.getTypeSize(val->getType());
      unsigned destSize = target.DataLayout.getTypeSize(dest->getType());
      
      if (! dest->getType()->isSigned())
        {
          uint64_t C = GetConstantValueAsUnsignedInt(val, isValidConstant);
          assert(isValidConstant && "Unrecognized constant");

          if (opSize > destSize ||
              (val->getType()->isSigned()
               && destSize < target.DataLayout.getIntegerRegize()))
            { // operand is larger than dest,
              //    OR both are equal but smaller than the full register size
              //       AND operand is signed, so it may have extra sign bits:
              // mask high bits
              C = C & ((1U << 8*destSize) - 1);
            }
          CreateUIntSetInstruction(target, C, dest, mvec, mcfi);
        }
      else
        {
          int64_t C = GetConstantValueAsSignedInt(val, isValidConstant);
          assert(isValidConstant && "Unrecognized constant");

          if (opSize > destSize)
            // operand is larger than dest: mask high bits
            C = C & ((1U << 8*destSize) - 1);

          if (opSize > destSize ||
              (opSize == destSize && !val->getType()->isSigned()))
            // sign-extend from destSize to 64 bits
            C = ((C & (1U << (8*destSize - 1)))
                 ? C | ~((1U << 8*destSize) - 1)
                 : C);
          
          CreateIntSetInstruction(target, C, dest, mvec, mcfi);
        }
    }
  else
    {
      // Make an instruction sequence to load the constant, viz:
      //            SETX <addr-of-constant>, tmpReg, addrReg
      //            LOAD  /*addr*/ addrReg, /*offset*/ 0, dest
      
      // First, create a tmp register to be used by the SETX sequence.
      TmpInstruction* tmpReg =
        new TmpInstruction(PointerType::get(val->getType()), val);
      mcfi.addTemp(tmpReg);
      
      // Create another TmpInstruction for the address register
      TmpInstruction* addrReg =
            new TmpInstruction(PointerType::get(val->getType()), val);
      mcfi.addTemp(addrReg);
      
      // Put the address (a symbolic name) into a register
      CreateSETXLabel(target, val, tmpReg, addrReg, mvec);
      
      // Generate the load instruction
      int64_t zeroOffset = 0;           // to avoid ambiguity with (Value*) 0
      MachineInstr* MI =
        Create3OperandInstr_SImmed(ChooseLoadInstruction(val->getType()),
                                   addrReg, zeroOffset, dest);
      mvec.push_back(MI);
      
      // Make sure constant is emitted to constant pool in assembly code.
      MachineCodeForMethod::get(F).addToConstantPool(cast<Constant>(val));
    }
}


// Create an instruction sequence to copy an integer register `val'
// to a floating point register `dest' by copying to memory and back.
// val must be an integral type.  dest must be a Float or Double.
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineCodeForMethod.
// 
void
UltraSparcInstrInfo::CreateCodeToCopyIntToFloat(const TargetMachine& target,
                                        Function* F,
                                        Value* val,
                                        Instruction* dest,
                                        vector<MachineInstr*>& mvec,
                                        MachineCodeForInstruction& mcfi) const
{
  assert((val->getType()->isIntegral() || isa<PointerType>(val->getType()))
         && "Source type must be integral (integer or bool) or pointer");
  assert(dest->getType()->isFloatingPoint()
         && "Dest type must be float/double");

  // Get a stack slot to use for the copy
  int offset = MachineCodeForMethod::get(F).allocateLocalVar(target, val); 

  // Get the size of the source value being copied. 
  size_t srcSize = target.DataLayout.getTypeSize(val->getType());

  // Store instruction stores `val' to [%fp+offset].
  // The store and load opCodes are based on the size of the source value.
  // If the value is smaller than 32 bits, we must sign- or zero-extend it
  // to 32 bits since the load-float will load 32 bits.
  // Note that the store instruction is the same for signed and unsigned ints.
  const Type* storeType = (srcSize <= 4)? Type::IntTy : Type::LongTy;
  Value* storeVal = val;
  if (srcSize < target.DataLayout.getTypeSize(Type::FloatTy))
    { // sign- or zero-extend respectively
      storeVal = new TmpInstruction(storeType, val);
      if (val->getType()->isSigned())
        CreateSignExtensionInstructions(target, F, val, 8*srcSize, storeVal,
                                        mvec, mcfi);
      else
        CreateZeroExtensionInstructions(target, F, val, 8*srcSize, storeVal,
                                        mvec, mcfi);
    }
  MachineInstr* store=new MachineInstr(ChooseStoreInstruction(storeType));
  store->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister, storeVal);
  store->SetMachineOperandReg(1, target.getRegInfo().getFramePointer());
  store->SetMachineOperandConst(2,MachineOperand::MO_SignExtendedImmed,offset);
  mvec.push_back(store);

  // Load instruction loads [%fp+offset] to `dest'.
  // The type of the load opCode is the floating point type that matches the
  // stored type in size:
  // On SparcV9: float for int or smaller, double for long.
  // 
  const Type* loadType = (srcSize <= 4)? Type::FloatTy : Type::DoubleTy;
  MachineInstr* load = new MachineInstr(ChooseLoadInstruction(loadType));
  load->SetMachineOperandReg(0, target.getRegInfo().getFramePointer());
  load->SetMachineOperandConst(1, MachineOperand::MO_SignExtendedImmed,offset);
  load->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister, dest);
  mvec.push_back(load);
}

// Similarly, create an instruction sequence to copy an FP register
// `val' to an integer register `dest' by copying to memory and back.
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineCodeForMethod.
// 
void
UltraSparcInstrInfo::CreateCodeToCopyFloatToInt(const TargetMachine& target,
                                        Function* F,
                                        Value* val,
                                        Instruction* dest,
                                        vector<MachineInstr*>& mvec,
                                        MachineCodeForInstruction& mcfi) const
{
  const Type* opTy   = val->getType();
  const Type* destTy = dest->getType();

  assert(opTy->isFloatingPoint() && "Source type must be float/double");
  assert((destTy->isIntegral() || isa<PointerType>(destTy))
         && "Dest type must be integer, bool or pointer");

  int offset = MachineCodeForMethod::get(F).allocateLocalVar(target, val); 

  // Store instruction stores `val' to [%fp+offset].
  // The store opCode is based only the source value being copied.
  // 
  MachineInstr* store=new MachineInstr(ChooseStoreInstruction(opTy));
  store->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister, val);
  store->SetMachineOperandReg(1, target.getRegInfo().getFramePointer());
  store->SetMachineOperandConst(2,MachineOperand::MO_SignExtendedImmed,offset);
  mvec.push_back(store);

  // Load instruction loads [%fp+offset] to `dest'.
  // The type of the load opCode is the integer type that matches the
  // source type in size:
  // On SparcV9: int for float, long for double.
  // Note that we *must* use signed loads even for unsigned dest types, to
  // ensure correct sign-extension for UByte, UShort or UInt:
  // 
  const Type* loadTy = (opTy == Type::FloatTy)? Type::IntTy : Type::LongTy;
  MachineInstr* load = new MachineInstr(ChooseLoadInstruction(loadTy));
  load->SetMachineOperandReg(0, target.getRegInfo().getFramePointer());
  load->SetMachineOperandConst(1, MachineOperand::MO_SignExtendedImmed,offset);
  load->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister, dest);
  mvec.push_back(load);
}


// Create instruction(s) to copy src to dest, for arbitrary types
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineCodeForMethod.
// 
void
UltraSparcInstrInfo::CreateCopyInstructionsByType(const TargetMachine& target,
                                                  Function *F,
                                                  Value* src,
                                                  Instruction* dest,
                                                  vector<MachineInstr*>& mvec,
                                          MachineCodeForInstruction& mcfi) const
{
  bool loadConstantToReg = false;
  
  const Type* resultType = dest->getType();
  
  MachineOpCode opCode = ChooseAddInstructionByType(resultType);
  if (opCode == INVALID_OPCODE)
    {
      assert(0 && "Unsupported result type in CreateCopyInstructionsByType()");
      return;
    }
  
  // if `src' is a constant that doesn't fit in the immed field or if it is
  // a global variable (i.e., a constant address), generate a load
  // instruction instead of an add
  // 
  if (isa<Constant>(src))
    {
      unsigned int machineRegNum;
      int64_t immedValue;
      MachineOperand::MachineOperandType opType =
        ChooseRegOrImmed(src, opCode, target, /*canUseImmed*/ true,
                         machineRegNum, immedValue);
      
      if (opType == MachineOperand::MO_VirtualRegister)
        loadConstantToReg = true;
    }
  else if (isa<GlobalValue>(src))
    loadConstantToReg = true;
  
  if (loadConstantToReg)
    { // `src' is constant and cannot fit in immed field for the ADD
      // Insert instructions to "load" the constant into a register
      target.getInstrInfo().CreateCodeToLoadConst(target, F, src, dest,
                                                  mvec, mcfi);
    }
  else
    { // Create an add-with-0 instruction of the appropriate type.
      // Make `src' the second operand, in case it is a constant
      // Use (unsigned long) 0 for a NULL pointer value.
      // 
      const Type* zeroValueType =
        isa<PointerType>(resultType) ? Type::ULongTy : resultType;
      MachineInstr* minstr =
        Create3OperandInstr(opCode, Constant::getNullValue(zeroValueType),
                            src, dest);
      mvec.push_back(minstr);
    }
}


// Helper function for sign-extension and zero-extension.
// For SPARC v9, we sign-extend the given operand using SLL; SRA/SRL.
inline void
CreateBitExtensionInstructions(bool signExtend,
                               const TargetMachine& target,
                               Function* F,
                               Value* srcVal,
                               unsigned int srcSizeInBits,
                               Value* dest,
                               vector<MachineInstr*>& mvec,
                               MachineCodeForInstruction& mcfi)
{
  MachineInstr* M;
  assert(srcSizeInBits <= 32 &&
         "Hmmm... 32 < srcSizeInBits < 64 unexpected but could be handled.");

  if (srcSizeInBits < 32)
    { // SLL is needed since operand size is < 32 bits.
      TmpInstruction *tmpI = new TmpInstruction(dest->getType(),
                                                srcVal, dest,"make32");
      mcfi.addTemp(tmpI);
      M = Create3OperandInstr_UImmed(SLLX, srcVal, 32-srcSizeInBits, tmpI);
      mvec.push_back(M);
      srcVal = tmpI;
    }

  M = Create3OperandInstr_UImmed(signExtend? SRA : SRL,
                                 srcVal, 32-srcSizeInBits, dest);
  mvec.push_back(M);
}


// Create instruction sequence to produce a sign-extended register value
// from an arbitrary-sized integer value (sized in bits, not bytes).
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineCodeForMethod.
// 
void
UltraSparcInstrInfo::CreateSignExtensionInstructions(
                                        const TargetMachine& target,
                                        Function* F,
                                        Value* srcVal,
                                        unsigned int srcSizeInBits,
                                        Value* dest,
                                        vector<MachineInstr*>& mvec,
                                        MachineCodeForInstruction& mcfi) const
{
  CreateBitExtensionInstructions(/*signExtend*/ true, target, F, srcVal,
                                 srcSizeInBits, dest, mvec, mcfi);
}


// Create instruction sequence to produce a zero-extended register value
// from an arbitrary-sized integer value (sized in bits, not bytes).
// For SPARC v9, we sign-extend the given operand using SLL; SRL.
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineCodeForMethod.
// 
void
UltraSparcInstrInfo::CreateZeroExtensionInstructions(
                                        const TargetMachine& target,
                                        Function* F,
                                        Value* srcVal,
                                        unsigned int srcSizeInBits,
                                        Value* dest,
                                        vector<MachineInstr*>& mvec,
                                        MachineCodeForInstruction& mcfi) const
{
  CreateBitExtensionInstructions(/*signExtend*/ false, target, F, srcVal,
                                 srcSizeInBits, dest, mvec, mcfi);
}
