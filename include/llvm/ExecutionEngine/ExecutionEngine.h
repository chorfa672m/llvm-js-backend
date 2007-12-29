//===- ExecutionEngine.h - Abstract Execution Engine Interface --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the abstract interface that implements execution support
// for LLVM.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTION_ENGINE_H
#define LLVM_EXECUTION_ENGINE_H

#include <vector>
#include <map>
#include <cassert>
#include <string>
#include "llvm/System/Mutex.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {

struct GenericValue;
class Constant;
class Function;
class GlobalVariable;
class GlobalValue;
class Module;
class ModuleProvider;
class TargetData;
class Type;
class MutexGuard;
class JITMemoryManager;

class ExecutionEngineState {
private:
  /// GlobalAddressMap - A mapping between LLVM global values and their
  /// actualized version...
  std::map<const GlobalValue*, void *> GlobalAddressMap;

  /// GlobalAddressReverseMap - This is the reverse mapping of GlobalAddressMap,
  /// used to convert raw addresses into the LLVM global value that is emitted
  /// at the address.  This map is not computed unless getGlobalValueAtAddress
  /// is called at some point.
  std::map<void *, const GlobalValue*> GlobalAddressReverseMap;

public:
  std::map<const GlobalValue*, void *> &
  getGlobalAddressMap(const MutexGuard &locked) {
    return GlobalAddressMap;
  }

  std::map<void*, const GlobalValue*> & 
  getGlobalAddressReverseMap(const MutexGuard& locked) {
    return GlobalAddressReverseMap;
  }
};


class ExecutionEngine {
  const TargetData *TD;
  ExecutionEngineState state;
  bool LazyCompilationDisabled;

protected:
  /// Modules - This is a list of ModuleProvider's that we are JIT'ing from.  We
  /// use a smallvector to optimize for the case where there is only one module.
  SmallVector<ModuleProvider*, 1> Modules;
  
  void setTargetData(const TargetData *td) {
    TD = td;
  }

  // To avoid having libexecutionengine depend on the JIT and interpreter
  // libraries, the JIT and Interpreter set these functions to ctor pointers
  // at startup time if they are linked in.
  typedef ExecutionEngine *(*EECtorFn)(ModuleProvider*, std::string*);
  static EECtorFn JITCtor, InterpCtor;

  /// LazyFunctionCreator - If an unknown function is needed, this function
  /// pointer is invoked to create it. If this returns null, the JIT will abort.
  void* (*LazyFunctionCreator)(const std::string &);
  
public:
  /// lock - This lock is protects the ExecutionEngine, JIT, JITResolver and
  /// JITEmitter classes.  It must be held while changing the internal state of
  /// any of those classes.
  sys::Mutex lock; // Used to make this class and subclasses thread-safe

  //===----------------------------------------------------------------------===//
  //  ExecutionEngine Startup
  //===----------------------------------------------------------------------===//

  virtual ~ExecutionEngine();

  /// create - This is the factory method for creating an execution engine which
  /// is appropriate for the current machine.  This takes ownership of the
  /// module provider.
  static ExecutionEngine *create(ModuleProvider *MP,
                                 bool ForceInterpreter = false,
                                 std::string *ErrorStr = 0);
  
  /// create - This is the factory method for creating an execution engine which
  /// is appropriate for the current machine.  This takes ownership of the
  /// module.
  static ExecutionEngine *create(Module *M);

  /// createJIT - This is the factory method for creating a JIT for the current
  /// machine, it does not fall back to the interpreter.  This takes ownership
  /// of the ModuleProvider and JITMemoryManager if successful.
  static ExecutionEngine *createJIT(ModuleProvider *MP,
                                    std::string *ErrorStr = 0,
                                    JITMemoryManager *JMM = 0);
  
  
  
  /// addModuleProvider - Add a ModuleProvider to the list of modules that we
  /// can JIT from.  Note that this takes ownership of the ModuleProvider: when
  /// the ExecutionEngine is destroyed, it destroys the MP as well.
  void addModuleProvider(ModuleProvider *P) {
    Modules.push_back(P);
  }
  
  //===----------------------------------------------------------------------===//

  const TargetData *getTargetData() const { return TD; }


  /// removeModuleProvider - Remove a ModuleProvider from the list of modules.
  /// Release module from ModuleProvider.
  Module* removeModuleProvider(ModuleProvider *P, std::string *ErrInfo = 0);

  /// FindFunctionNamed - Search all of the active modules to find the one that
  /// defines FnName.  This is very slow operation and shouldn't be used for
  /// general code.
  Function *FindFunctionNamed(const char *FnName);
  
  /// runFunction - Execute the specified function with the specified arguments,
  /// and return the result.
  ///
  virtual GenericValue runFunction(Function *F,
                                const std::vector<GenericValue> &ArgValues) = 0;

  /// runStaticConstructorsDestructors - This method is used to execute all of
  /// the static constructors or destructors for a module, depending on the
  /// value of isDtors.
  void runStaticConstructorsDestructors(bool isDtors);
  
  
  /// runFunctionAsMain - This is a helper function which wraps runFunction to
  /// handle the common task of starting up main with the specified argc, argv,
  /// and envp parameters.
  int runFunctionAsMain(Function *Fn, const std::vector<std::string> &argv,
                        const char * const * envp);


  /// addGlobalMapping - Tell the execution engine that the specified global is
  /// at the specified location.  This is used internally as functions are JIT'd
  /// and as global variables are laid out in memory.  It can and should also be
  /// used by clients of the EE that want to have an LLVM global overlay
  /// existing data in memory.
  void addGlobalMapping(const GlobalValue *GV, void *Addr);
  
  /// clearAllGlobalMappings - Clear all global mappings and start over again
  /// use in dynamic compilation scenarios when you want to move globals
  void clearAllGlobalMappings();
  
  /// updateGlobalMapping - Replace an existing mapping for GV with a new
  /// address.  This updates both maps as required.  If "Addr" is null, the
  /// entry for the global is removed from the mappings.
  void updateGlobalMapping(const GlobalValue *GV, void *Addr);
  
  /// getPointerToGlobalIfAvailable - This returns the address of the specified
  /// global value if it is has already been codegen'd, otherwise it returns
  /// null.
  ///
  void *getPointerToGlobalIfAvailable(const GlobalValue *GV);

  /// getPointerToGlobal - This returns the address of the specified global
  /// value.  This may involve code generation if it's a function.
  ///
  void *getPointerToGlobal(const GlobalValue *GV);

  /// getPointerToFunction - The different EE's represent function bodies in
  /// different ways.  They should each implement this to say what a function
  /// pointer should look like.
  ///
  virtual void *getPointerToFunction(Function *F) = 0;

  /// getPointerToFunctionOrStub - If the specified function has been
  /// code-gen'd, return a pointer to the function.  If not, compile it, or use
  /// a stub to implement lazy compilation if available.
  ///
  virtual void *getPointerToFunctionOrStub(Function *F) {
    // Default implementation, just codegen the function.
    return getPointerToFunction(F);
  }

  /// getGlobalValueAtAddress - Return the LLVM global value object that starts
  /// at the specified address.
  ///
  const GlobalValue *getGlobalValueAtAddress(void *Addr);


  void StoreValueToMemory(const GenericValue &Val, GenericValue *Ptr, const Type *Ty);
  void InitializeMemory(const Constant *Init, void *Addr);

  /// recompileAndRelinkFunction - This method is used to force a function
  /// which has already been compiled to be compiled again, possibly
  /// after it has been modified. Then the entry to the old copy is overwritten
  /// with a branch to the new copy. If there was no old copy, this acts
  /// just like VM::getPointerToFunction().
  ///
  virtual void *recompileAndRelinkFunction(Function *F) = 0;

  /// freeMachineCodeForFunction - Release memory in the ExecutionEngine
  /// corresponding to the machine code emitted to execute this function, useful
  /// for garbage-collecting generated code.
  ///
  virtual void freeMachineCodeForFunction(Function *F) = 0;

  /// getOrEmitGlobalVariable - Return the address of the specified global
  /// variable, possibly emitting it to memory if needed.  This is used by the
  /// Emitter.
  virtual void *getOrEmitGlobalVariable(const GlobalVariable *GV) {
    return getPointerToGlobal((GlobalValue*)GV);
  }
  
  /// DisableLazyCompilation - If called, the JIT will abort if lazy compilation
  // is ever attempted.
  void DisableLazyCompilation() {
    LazyCompilationDisabled = true;
  }
  bool isLazyCompilationDisabled() const {
    return LazyCompilationDisabled;
  }
  
  
  /// InstallLazyFunctionCreator - If an unknown function is needed, the
  /// specified function pointer is invoked to create it.  If it returns null,
  /// the JIT will abort.
  void InstallLazyFunctionCreator(void* (*P)(const std::string &)) {
    LazyFunctionCreator = P;
  }

protected:
  explicit ExecutionEngine(ModuleProvider *P);

  void emitGlobals();

  // EmitGlobalVariable - This method emits the specified global variable to the
  // address specified in GlobalAddresses, or allocates new memory if it's not
  // already in the map.
  void EmitGlobalVariable(const GlobalVariable *GV);

  GenericValue getConstantValue(const Constant *C);
  void LoadValueFromMemory(GenericValue &Result, GenericValue *Ptr, 
                           const Type *Ty);
};

} // End llvm namespace

#endif
