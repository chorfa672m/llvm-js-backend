//===- RegisterInfoEmitter.cpp - Generate a Register File Desc. -*- C++ -*-===//
//
// This tablegen backend is responsible for emitting a description of a target
// register file for a code generator.  It uses instances of the Register,
// RegisterAliases, and RegisterClass classes to gather this information.
//
//===----------------------------------------------------------------------===//

#include "RegisterInfoEmitter.h"
#include "Record.h"
#include "Support/StringExtras.h"
#include <set>

static void EmitSourceHeader(const std::string &Desc, std::ostream &o) {
  o << "//===- TableGen'erated file -------------------------------------*-"
       " C++ -*-===//\n//\n// " << Desc << "\n//\n// Automatically generate"
       "d file, do not edit!\n//\n//===------------------------------------"
       "----------------------------------===//\n\n";
}

// runEnums - Print out enum values for all of the registers.
void RegisterInfoEmitter::runEnums(std::ostream &OS) {
  std::vector<Record*> Registers = Records.getAllDerivedDefinitions("Register");

  if (Registers.size() == 0)
    throw std::string("No 'Register' subclasses defined!");

  std::string Namespace = Registers[0]->getValueAsString("Namespace");

  EmitSourceHeader("Target Register Enum Values", OS);

  if (!Namespace.empty())
    OS << "namespace " << Namespace << " {\n";
  OS << "  enum {\n    NoRegister,\n";

  for (unsigned i = 0, e = Registers.size(); i != e; ++i)
    OS << "    " << Registers[i]->getName() << ", \t// " << i+1 << "\n";
  
  OS << "  };\n";
  if (!Namespace.empty())
    OS << "}\n";
}

static Record *getRegisterInfo(RecordKeeper &RC) {
  std::vector<Record*> RegisterInfos =
    Records.getAllDerivedDefinitions("RegisterInfo");

  if (RegisterInfos.size() != 1)
    throw std::string("ERROR: Multiple subclasses of RegisterInfo defined!");
  return RegisterInfos[0];
}

void RegisterInfoEmitter::runHeader(std::ostream &OS) {
  EmitSourceHeader("Register Information Header Fragment", OS);
  
  std::string ClassName =
    getRegisterInfo(Records)->getValueAsString("ClassName");

  OS << "#include \"llvm/Target/MRegisterInfo.h\"\n\n";

  OS << "struct " << ClassName << " : public MRegisterInfo {\n"
     << "  " << ClassName
     << "(int CallFrameSetupOpcode = -1, int CallFrameDestroyOpcode = -1);\n"
     << "  const unsigned* getCalleeSaveRegs() const;\n"
     << "};\n\n";
}

static std::string getQualifiedName(Record *R) {
  std::string Namespace = R->getValueAsString("Namespace");
  if (Namespace.empty()) return R->getName();
  return Namespace + "::" + R->getName();
}

// RegisterInfoEmitter::run - Main register file description emitter.
//
void RegisterInfoEmitter::run(std::ostream &OS) {
  EmitSourceHeader("Register Information Source Fragment", OS);

  // Start out by emitting each of the register classes... to do this, we build
  // a set of registers which belong to a register class, this is to ensure that
  // each register is only in a single register class.
  //
  std::vector<Record*> RegisterClasses =
    Records.getAllDerivedDefinitions("RegisterClass");

  std::vector<Record*> Registers = Records.getAllDerivedDefinitions("Register");
  Record *RegisterClass = Records.getClass("Register");

  std::set<Record*> RegistersFound;
  std::vector<std::string> RegClassNames;

  // Loop over all of the register classes... emitting each one.
  OS << "namespace {     // Register classes...\n";

  for (unsigned rc = 0, e = RegisterClasses.size(); rc != e; ++rc) {
    Record *RC = RegisterClasses[rc];
    std::string Name = RC->getName();
    if (Name[9] == '.') {
      static unsigned AnonCounter = 0;
      Name = "AnonRegClass_"+utostr(AnonCounter++);
    }

    RegClassNames.push_back(Name);

    // Emit the register list now...
    OS << "  // " << Name << " Register Class...\n  const unsigned " << Name
       << "[] = {\n    ";
    ListInit *RegList = RC->getValueAsListInit("MemberList");
    for (unsigned i = 0, e = RegList->getSize(); i != e; ++i) {
      Record *Reg = RegList->getElement(i);
      if (!Reg->isSubClassOf(RegisterClass))
        throw "Register Class member '" + Reg->getName() +
              " does not derive from the Register class!";
      if (RegistersFound.count(Reg))
        throw "Register '" + Reg->getName() +
              "' included in multiple register classes!";
      RegistersFound.insert(Reg);
      OS << getQualifiedName(Reg) << ", ";
    }
    OS << "\n  };\n\n";

    OS << "  struct " << Name << "Class : public TargetRegisterClass {\n"
       << "    " << Name << "Class() : TargetRegisterClass("
       << RC->getValueAsInt("Size")/8 << ", " << RC->getValueAsInt("Alignment")
       << ", " << Name << ", " << Name << " + " << RegList->getSize()
       << ") {}\n";
    
    if (CodeInit *CI = dynamic_cast<CodeInit*>(RC->getValueInit("Methods")))
      OS << CI->getValue();
    else
      throw "Expected 'code' fragment for 'Methods' value in register class '"+
            RC->getName() + "'!";

    OS << "  } " << Name << "Instance;\n\n";
  }

  OS << "  const TargetRegisterClass* const RegisterClasses[] = {\n";
  for (unsigned i = 0, e = RegClassNames.size(); i != e; ++i)
    OS << "    &" << RegClassNames[i] << "Instance,\n";
  OS << "  };\n";

  // Emit register class aliases...
  std::vector<Record*> RegisterAliasesRecs =
    Records.getAllDerivedDefinitions("RegisterAliases");
  std::map<Record*, std::set<Record*> > RegisterAliases;
  
  for (unsigned i = 0, e = RegisterAliasesRecs.size(); i != e; ++i) {
    Record *AS = RegisterAliasesRecs[i];
    Record *R = AS->getValueAsDef("Reg");
    ListInit *LI = AS->getValueAsListInit("Aliases");

    // Add information that R aliases all of the elements in the list... and
    // that everything in the list aliases R.
    for (unsigned j = 0, e = LI->getSize(); j != e; ++j) {
      if (RegisterAliases[R].count(LI->getElement(j)))
        std::cerr << "Warning: register alias between " << getQualifiedName(R)
                  << " and " << getQualifiedName(LI->getElement(j))
                  << " specified multiple times!\n";
      RegisterAliases[R].insert(LI->getElement(j));

      if (RegisterAliases[LI->getElement(j)].count(R))
        std::cerr << "Warning: register alias between " << getQualifiedName(R)
                  << " and " << getQualifiedName(LI->getElement(j))
                  << " specified multiple times!\n";
      RegisterAliases[LI->getElement(j)].insert(R);
    }
  }

  if (!RegisterAliases.empty())
    OS << "\n\n  // Register Alias Sets...\n";
  
  // Loop over all of the registers which have aliases, emitting the alias list
  // to memory.
  for (std::map<Record*, std::set<Record*> >::iterator
         I = RegisterAliases.begin(), E = RegisterAliases.end(); I != E; ++I) {
    OS << "  const unsigned " << I->first->getName() << "_AliasSet[] = { ";
    for (std::set<Record*>::iterator ASI = I->second.begin(),
           E = I->second.end(); ASI != E; ++ASI)
      OS << getQualifiedName(*ASI) << ", ";
    OS << "0 };\n";
  }

  OS << "\n  const MRegisterDesc RegisterDescriptors[] = { // Descriptors\n";
  OS << "    { \"NOREG\",\t0,\t\t0,\t0 },\n";
  // Now that register alias sets have been emitted, emit the register
  // descriptors now.
  for (unsigned i = 0, e = Registers.size(); i != e; ++i) {
    Record *Reg = Registers[i];
    OS << "    { \"" << Reg->getName() << "\",\t";
    if (RegisterAliases.count(Reg))
      OS << Reg->getName() << "_AliasSet,\t";
    else
      OS << "0,\t\t";
    OS << "0, 0 },\n";    
  }
  OS << "  };\n";      // End of register descriptors...
  OS << "}\n\n";       // End of anonymous namespace...

  Record *RegisterInfo = getRegisterInfo(Records);
  std::string ClassName = RegisterInfo->getValueAsString("ClassName");
  
  // Emit the constructor of the class...
  OS << ClassName << "::" << ClassName
     << "(int CallFrameSetupOpcode, int CallFrameDestroyOpcode)\n"
     << "  : MRegisterInfo(RegisterDescriptors, " << Registers.size()+1
     << ", RegisterClasses, RegisterClasses+" << RegClassNames.size() << ",\n "
     << "                 CallFrameSetupOpcode, CallFrameDestroyOpcode) {}\n\n";
  
  // Emit the getCalleeSaveRegs method...
  OS << "const unsigned* " << ClassName << "::getCalleeSaveRegs() const {\n"
     << "  static const unsigned CalleeSaveRegs[] = {\n    ";

  ListInit *LI = RegisterInfo->getValueAsListInit("CalleeSavedRegisters");
  for (unsigned i = 0, e = LI->getSize(); i != e; ++i)
    OS << getQualifiedName(LI->getElement(i)) << ", ";  
  OS << " 0\n  };\n  return CalleeSaveRegs;\n}\n\n";
}
