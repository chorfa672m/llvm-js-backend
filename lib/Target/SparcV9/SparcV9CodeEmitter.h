//===-- SparcV9CodeEmitter.h ------------------------------------*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
// 
// TODO: Need a description here.
//
//===----------------------------------------------------------------------===//

#ifndef SPARCV9CODEEMITTER_H
#define SPARCV9CODEEMITTER_H

#include "llvm/BasicBlock.h"
#include "llvm/CodeGen/MachineCodeEmitter.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Target/TargetMachine.h"

class GlobalValue;
class MachineInstr;
class MachineOperand;

class SparcV9CodeEmitter : public MachineFunctionPass {
  TargetMachine &TM;
  MachineCodeEmitter &MCE;
  const BasicBlock *currBB;

  // Tracks which instruction references which BasicBlock
  std::vector<std::pair<const BasicBlock*,
                        std::pair<unsigned*,MachineInstr*> > > BBRefs;
  // Tracks where each BasicBlock starts
  std::map<const BasicBlock*, long> BBLocations;

  // Tracks locations of Constants which are laid out in memory (e.g. FP)
  // But we also need to map Constants to ConstantPool indices
  std::map<const Constant*, unsigned> ConstantMap;

public:
  SparcV9CodeEmitter(TargetMachine &T, MachineCodeEmitter &M);
  ~SparcV9CodeEmitter();

  /// runOnMachineFunction - emits the given machine function to memory.
  ///
  bool runOnMachineFunction(MachineFunction &F);

  /// emitWord - writes out the given 32-bit value to memory at the current PC.
  ///
  void emitWord(unsigned Val);
    
  /// getBinaryCodeForInstr - This function, generated by the
  /// CodeEmitterGenerator using TableGen, produces the binary encoding for
  /// machine instructions.
  ///
  unsigned getBinaryCodeForInstr(MachineInstr &MI);

  /// emitFarCall - produces a code sequence to make a call to a destination
  /// that does not fit in the 30 bits that a call instruction allows.
  /// If the function F is non-null, this also saves the return address in
  /// the LazyResolver map of the JITResolver.
  void emitFarCall(uint64_t Addr, Function *F = 0);

private:    
  /// getMachineOpValue - 
  ///
  int64_t getMachineOpValue(MachineInstr &MI, MachineOperand &MO);

  /// emitBasicBlock - 
  ///
  void emitBasicBlock(MachineBasicBlock &MBB);

  /// getValueBit - 
  ///
  unsigned getValueBit(int64_t Val, unsigned bit);

  /// getGlobalAddress - 
  ///
  void* getGlobalAddress(GlobalValue *V, MachineInstr &MI,
                         bool isPCRelative);
  /// emitFarCall - 
  ///
  unsigned getRealRegNum(unsigned fakeReg, MachineInstr &MI);

};

#endif
