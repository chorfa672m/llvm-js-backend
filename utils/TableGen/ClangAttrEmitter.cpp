//===- ClangAttrEmitter.cpp - Generate Clang attribute handling =-*- C++ -*--=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// These tablegen backends emit Clang attribute processing code
//
//===----------------------------------------------------------------------===//

#include "ClangAttrEmitter.h"
#include "Record.h"
#include <algorithm>

using namespace llvm;

void ClangAttrClassEmitter::run(raw_ostream &OS) {
  OS << "// This file is generated by TableGen. Do not edit.\n\n";
  OS << "#ifndef LLVM_CLANG_ATTR_CLASSES_INC\n";
  OS << "#define LLVM_CLANG_ATTR_CLASSES_INC\n\n";

  std::vector<Record*> Attrs = Records.getAllDerivedDefinitions("Attr");

  for (std::vector<Record*>::iterator i = Attrs.begin(), e = Attrs.end();
       i != e; ++i) {
    Record &R = **i;

    if (R.getValueAsBit("DoNotEmit"))
      continue;

    OS << "class " << R.getName() << "Attr : public Attr {\n";

    std::vector<Record*> Args = R.getValueAsListOfDefs("Args");
    std::vector<Record*>::iterator ai, ae = Args.end();

    // FIXME: Handle arguments
    assert(Args.empty() && "Can't yet handle arguments");

    OS << "\n public:\n";
    OS << "  " << R.getName() << "Attr(";
    
    // Arguments go here
    
    OS << ")\n";
    OS << "    : Attr(attr::" << R.getName() << ")";

    // Arguments go here
    
    OS << " {}\n\n";

    OS << "  virtual Attr *clone (ASTContext &C) const;\n";
    OS << "  static bool classof(const Attr *A) { return A->getKind() == "
       << "attr::" << R.getName() << "; }\n";
    OS << "  static bool classof(const " << R.getName()
       << "Attr *) { return true; }\n";
    OS << "};\n\n";
  }

  OS << "#endif\n";
}

void ClangAttrListEmitter::run(raw_ostream &OS) {
  OS << "// This file is generated by TableGen. Do not edit.\n\n";

  OS << "#ifndef LAST_ATTR\n";
  OS << "#define LAST_ATTR(NAME) ATTR(NAME)\n";
  OS << "#endif\n\n";
   
  std::vector<Record*> Attrs = Records.getAllDerivedDefinitions("Attr");
  std::vector<Record*>::iterator i = Attrs.begin(), e = Attrs.end();

  if (i != e) {
    // Move the end iterator back to emit the last attribute.
    for(--e; i != e; ++i)
      OS << "ATTR(" << (*i)->getName() << ")\n";
    
    OS << "LAST_ATTR(" << (*i)->getName() << ")\n\n";
  }

  OS << "#undef LAST_ATTR\n";
  OS << "#undef ATTR\n";
}
