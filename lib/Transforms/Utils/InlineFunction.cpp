//===- InlineFunction.cpp - Code to perform function inlining -------------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements inlining of a function into a call site, resolving
// parameters and the return value as appropriate.
//
// FIXME: This pass should transform alloca instructions in the called function
//        into malloc/free pairs!  Or perhaps it should refuse to inline them!
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Constant.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/Instructions.h"
#include "llvm/Intrinsics.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Transforms/Utils/Local.h"
using namespace llvm;

bool llvm::InlineFunction(CallInst *CI) { return InlineFunction(CallSite(CI)); }
bool llvm::InlineFunction(InvokeInst *II) {return InlineFunction(CallSite(II));}

// InlineFunction - This function inlines the called function into the basic
// block of the caller.  This returns false if it is not possible to inline this
// call.  The program is still in a well defined state if this occurs though.
//
// Note that this only does one level of inlining.  For example, if the 
// instruction 'call B' is inlined, and 'B' calls 'C', then the call to 'C' now 
// exists in the instruction stream.  Similiarly this will inline a recursive
// function by one level.
//
bool llvm::InlineFunction(CallSite CS) {
  Instruction *TheCall = CS.getInstruction();
  assert(TheCall->getParent() && TheCall->getParent()->getParent() &&
         "Instruction not in function!");

  const Function *CalledFunc = CS.getCalledFunction();
  if (CalledFunc == 0 ||          // Can't inline external function or indirect
      CalledFunc->isExternal() || // call, or call to a vararg function!
      CalledFunc->getFunctionType()->isVarArg()) return false;

  BasicBlock *OrigBB = TheCall->getParent();
  Function *Caller = OrigBB->getParent();

  // We want to clone the entire callee function into the whole between the
  // "starter" and "ender" blocks.  How we accomplish this depends on whether
  // this is an invoke instruction or a call instruction.

  BasicBlock *InvokeDest = 0;     // Exception handling destination
  std::vector<Value*> InvokeDestPHIValues; // Values for PHI nodes in InvokeDest
  BasicBlock *AfterCallBB;

  if (InvokeInst *II = dyn_cast<InvokeInst>(TheCall)) {
    InvokeDest = II->getExceptionalDest();

    // If there are PHI nodes in the exceptional destination block, we need to
    // keep track of which values came into them from this invoke, then remove
    // the entry for this block.
    for (BasicBlock::iterator I = InvokeDest->begin();
         PHINode *PN = dyn_cast<PHINode>(I); ++I) {
      // Save the value to use for this edge...
      InvokeDestPHIValues.push_back(PN->getIncomingValueForBlock(OrigBB));
    }

    // Add an unconditional branch to make this look like the CallInst case...
    BranchInst *NewBr = new BranchInst(II->getNormalDest(), TheCall);

    // Split the basic block.  This guarantees that no PHI nodes will have to be
    // updated due to new incoming edges, and make the invoke case more
    // symmetric to the call case.
    AfterCallBB = OrigBB->splitBasicBlock(NewBr,
                                          CalledFunc->getName()+".entry");

    // Remove (unlink) the InvokeInst from the function...
    OrigBB->getInstList().remove(TheCall);

  } else {  // It's a call
    // If this is a call instruction, we need to split the basic block that the
    // call lives in.
    //
    AfterCallBB = OrigBB->splitBasicBlock(TheCall,
                                          CalledFunc->getName()+".entry");
    // Remove (unlink) the CallInst from the function...
    AfterCallBB->getInstList().remove(TheCall);
  }

  // If we have a return value generated by this call, convert it into a PHI 
  // node that gets values from each of the old RET instructions in the original
  // function.
  //
  PHINode *PHI = 0;
  if (!TheCall->use_empty()) {
    // The PHI node should go at the front of the new basic block to merge all 
    // possible incoming values.
    //
    PHI = new PHINode(CalledFunc->getReturnType(), TheCall->getName(),
                      AfterCallBB->begin());

    // Anything that used the result of the function call should now use the PHI
    // node as their operand.
    //
    TheCall->replaceAllUsesWith(PHI);
  }

  // Get an iterator to the last basic block in the function, which will have
  // the new function inlined after it.
  //
  Function::iterator LastBlock = &Caller->back();

  // Calculate the vector of arguments to pass into the function cloner...
  std::map<const Value*, Value*> ValueMap;
  assert(std::distance(CalledFunc->abegin(), CalledFunc->aend()) == 
         std::distance(CS.arg_begin(), CS.arg_end()) &&
         "No varargs calls can be inlined!");

  CallSite::arg_iterator AI = CS.arg_begin();
  for (Function::const_aiterator I = CalledFunc->abegin(), E=CalledFunc->aend();
       I != E; ++I, ++AI)
    ValueMap[I] = *AI;

  // Since we are now done with the Call/Invoke, we can delete it.
  delete TheCall;

  // Make a vector to capture the return instructions in the cloned function...
  std::vector<ReturnInst*> Returns;

  // Do all of the hard part of cloning the callee into the caller...
  CloneFunctionInto(Caller, CalledFunc, ValueMap, Returns, ".i");

  // Loop over all of the return instructions, turning them into unconditional
  // branches to the merge point now...
  for (unsigned i = 0, e = Returns.size(); i != e; ++i) {
    ReturnInst *RI = Returns[i];
    BasicBlock *BB = RI->getParent();

    // Add a branch to the merge point where the PHI node lives if it exists.
    new BranchInst(AfterCallBB, RI);

    if (PHI) {   // The PHI node should include this value!
      assert(RI->getReturnValue() && "Ret should have value!");
      assert(RI->getReturnValue()->getType() == PHI->getType() && 
             "Ret value not consistent in function!");
      PHI->addIncoming(RI->getReturnValue(), BB);
    }

    // Delete the return instruction now
    BB->getInstList().erase(RI);
  }

  // Check to see if the PHI node only has one argument.  This is a common
  // case resulting from there only being a single return instruction in the
  // function call.  Because this is so common, eliminate the PHI node.
  //
  if (PHI && PHI->getNumIncomingValues() == 1) {
    PHI->replaceAllUsesWith(PHI->getIncomingValue(0));
    PHI->getParent()->getInstList().erase(PHI);
  }

  // Change the branch that used to go to AfterCallBB to branch to the first
  // basic block of the inlined function.
  //
  TerminatorInst *Br = OrigBB->getTerminator();
  assert(Br && Br->getOpcode() == Instruction::Br && 
	 "splitBasicBlock broken!");
  Br->setOperand(0, ++LastBlock);

  // If there are any alloca instructions in the block that used to be the entry
  // block for the callee, move them to the entry block of the caller.  First
  // calculate which instruction they should be inserted before.  We insert the
  // instructions at the end of the current alloca list.
  //
  if (isa<AllocaInst>(LastBlock->begin())) {
    BasicBlock::iterator InsertPoint = Caller->begin()->begin();
    while (isa<AllocaInst>(InsertPoint)) ++InsertPoint;
    
    for (BasicBlock::iterator I = LastBlock->begin(), E = LastBlock->end();
         I != E; )
      if (AllocaInst *AI = dyn_cast<AllocaInst>(I++))
        if (isa<Constant>(AI->getArraySize())) {
          LastBlock->getInstList().remove(AI);
          Caller->front().getInstList().insert(InsertPoint, AI);      
        }
  }

  // If we just inlined a call due to an invoke instruction, scan the inlined
  // function checking for function calls that should now be made into invoke
  // instructions, and for unwind's which should be turned into branches.
  if (InvokeDest) {
    for (Function::iterator BB = LastBlock, E = Caller->end(); BB != E; ++BB) {
      for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ) {
        // We only need to check for function calls: inlined invoke instructions
        // require no special handling...
        if (CallInst *CI = dyn_cast<CallInst>(I)) {
          // Convert this function call into an invoke instruction...

          // First, split the basic block...
          BasicBlock *Split = BB->splitBasicBlock(CI, CI->getName()+".noexc");
          
          // Next, create the new invoke instruction, inserting it at the end
          // of the old basic block.
          InvokeInst *II =
            new InvokeInst(CI->getCalledValue(), Split, InvokeDest, 
                           std::vector<Value*>(CI->op_begin()+1, CI->op_end()),
                           CI->getName(), BB->getTerminator());

          // Make sure that anything using the call now uses the invoke!
          CI->replaceAllUsesWith(II);

          // Delete the unconditional branch inserted by splitBasicBlock
          BB->getInstList().pop_back();
          Split->getInstList().pop_front();  // Delete the original call
          
          // Update any PHI nodes in the exceptional block to indicate that
          // there is now a new entry in them.
          unsigned i = 0;
          for (BasicBlock::iterator I = InvokeDest->begin();
               PHINode *PN = dyn_cast<PHINode>(I); ++I, ++i)
            PN->addIncoming(InvokeDestPHIValues[i], BB);

          // This basic block is now complete, start scanning the next one.
          break;
        } else {
          ++I;
        }
      }

      if (UnwindInst *UI = dyn_cast<UnwindInst>(BB->getTerminator())) {
        // An UnwindInst requires special handling when it gets inlined into an
        // invoke site.  Once this happens, we know that the unwind would cause
        // a control transfer to the invoke exception destination, so we can
        // transform it into a direct branch to the exception destination.
        new BranchInst(InvokeDest, UI);

        // Delete the unwind instruction!
        UI->getParent()->getInstList().pop_back();

        // Update any PHI nodes in the exceptional block to indicate that
        // there is now a new entry in them.
        unsigned i = 0;
        for (BasicBlock::iterator I = InvokeDest->begin();
             PHINode *PN = dyn_cast<PHINode>(I); ++I, ++i)
          PN->addIncoming(InvokeDestPHIValues[i], BB);
      }
    }

    // Now that everything is happy, we have one final detail.  The PHI nodes in
    // the exception destination block still have entries due to the original
    // invoke instruction.  Eliminate these entries (which might even delete the
    // PHI node) now.
    for (BasicBlock::iterator I = InvokeDest->begin();
         PHINode *PN = dyn_cast<PHINode>(I); ++I)
      PN->removeIncomingValue(AfterCallBB);
  }
  // Now that the function is correct, make it a little bit nicer.  In
  // particular, move the basic blocks inserted from the end of the function
  // into the space made by splitting the source basic block.
  //
  Caller->getBasicBlockList().splice(AfterCallBB, Caller->getBasicBlockList(), 
                                     LastBlock, Caller->end());

  // We should always be able to fold the entry block of the function into the
  // single predecessor of the block...
  assert(cast<BranchInst>(Br)->isUnconditional() && "splitBasicBlock broken!");
  BasicBlock *CalleeEntry = cast<BranchInst>(Br)->getSuccessor(0);
  SimplifyCFG(CalleeEntry);
  
  // Okay, continue the CFG cleanup.  It's often the case that there is only a
  // single return instruction in the callee function.  If this is the case,
  // then we have an unconditional branch from the return block to the
  // 'AfterCallBB'.  Check for this case, and eliminate the branch is possible.
  SimplifyCFG(AfterCallBB);
  return true;
}
