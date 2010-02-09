//===- AsmWriterInst.h - Classes encapsulating a printable inst -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// These classes implement a parser for assembly strings.
//
//===----------------------------------------------------------------------===//

#include "AsmWriterInst.h"
#include "CodeGenTarget.h"
#include "Record.h"
#include "llvm/ADT/StringExtras.h"

using namespace llvm;

static bool isIdentChar(char C) {
  return (C >= 'a' && C <= 'z') ||
  (C >= 'A' && C <= 'Z') ||
  (C >= '0' && C <= '9') ||
  C == '_';
}

std::string AsmWriterOperand::getCode() const {
  if (OperandType == isLiteralTextOperand) {
    if (Str.size() == 1)
      return "O << '" + Str + "'; ";
    return "O << \"" + Str + "\"; ";
  }
  
  if (OperandType == isLiteralStatementOperand)
    return Str;
  
  std::string Result = Str + "(MI";
  if (MIOpNo != ~0U)
    Result += ", " + utostr(MIOpNo);
  if (!MiModifier.empty())
    Result += ", \"" + MiModifier + '"';
  return Result + "); ";
}

/// ParseAsmString - Parse the specified Instruction's AsmString into this
/// AsmWriterInst.
///
AsmWriterInst::AsmWriterInst(const CodeGenInstruction &CGI, Record *AsmWriter) {
  this->CGI = &CGI;
  
  unsigned Variant       = AsmWriter->getValueAsInt("Variant");
  int FirstOperandColumn = AsmWriter->getValueAsInt("FirstOperandColumn");
  int OperandSpacing     = AsmWriter->getValueAsInt("OperandSpacing");
  
  unsigned CurVariant = ~0U;  // ~0 if we are outside a {.|.|.} region, other #.
  
  // This is the number of tabs we've seen if we're doing columnar layout.
  unsigned CurColumn = 0;
  
  
  // NOTE: Any extensions to this code need to be mirrored in the 
  // AsmPrinter::printInlineAsm code that executes as compile time (assuming
  // that inline asm strings should also get the new feature)!
  const std::string &AsmString = CGI.AsmString;
  std::string::size_type LastEmitted = 0;
  while (LastEmitted != AsmString.size()) {
    std::string::size_type DollarPos =
    AsmString.find_first_of("${|}\\", LastEmitted);
    if (DollarPos == std::string::npos) DollarPos = AsmString.size();
    
    // Emit a constant string fragment.
    
    if (DollarPos != LastEmitted) {
      if (CurVariant == Variant || CurVariant == ~0U) {
        for (; LastEmitted != DollarPos; ++LastEmitted)
          switch (AsmString[LastEmitted]) {
            case '\n':
              AddLiteralString("\\n");
              break;
            case '\t':
              // If the asm writer is not using a columnar layout, \t is not
              // magic.
              if (FirstOperandColumn == -1 || OperandSpacing == -1) {
                AddLiteralString("\\t");
              } else {
                // We recognize a tab as an operand delimeter.
                unsigned DestColumn = FirstOperandColumn + 
                CurColumn++ * OperandSpacing;
                Operands.push_back(
                                   AsmWriterOperand("O.PadToColumn(" +
                                                    utostr(DestColumn) + ");\n",
                                                    AsmWriterOperand::isLiteralStatementOperand));
              }
              break;
            case '"':
              AddLiteralString("\\\"");
              break;
            case '\\':
              AddLiteralString("\\\\");
              break;
            default:
              AddLiteralString(std::string(1, AsmString[LastEmitted]));
              break;
          }
      } else {
        LastEmitted = DollarPos;
      }
    } else if (AsmString[DollarPos] == '\\') {
      if (DollarPos+1 != AsmString.size() &&
          (CurVariant == Variant || CurVariant == ~0U)) {
        if (AsmString[DollarPos+1] == 'n') {
          AddLiteralString("\\n");
        } else if (AsmString[DollarPos+1] == 't') {
          // If the asm writer is not using a columnar layout, \t is not
          // magic.
          if (FirstOperandColumn == -1 || OperandSpacing == -1) {
            AddLiteralString("\\t");
            break;
          }
          
          // We recognize a tab as an operand delimeter.
          unsigned DestColumn = FirstOperandColumn + 
          CurColumn++ * OperandSpacing;
          Operands.push_back(
                             AsmWriterOperand("O.PadToColumn(" + utostr(DestColumn) + ");\n",
                                              AsmWriterOperand::isLiteralStatementOperand));
          break;
        } else if (std::string("${|}\\").find(AsmString[DollarPos+1]) 
                   != std::string::npos) {
          AddLiteralString(std::string(1, AsmString[DollarPos+1]));
        } else {
          throw "Non-supported escaped character found in instruction '" +
          CGI.TheDef->getName() + "'!";
        }
        LastEmitted = DollarPos+2;
        continue;
      }
    } else if (AsmString[DollarPos] == '{') {
      if (CurVariant != ~0U)
        throw "Nested variants found for instruction '" +
        CGI.TheDef->getName() + "'!";
      LastEmitted = DollarPos+1;
      CurVariant = 0;   // We are now inside of the variant!
    } else if (AsmString[DollarPos] == '|') {
      if (CurVariant == ~0U)
        throw "'|' character found outside of a variant in instruction '"
        + CGI.TheDef->getName() + "'!";
      ++CurVariant;
      ++LastEmitted;
    } else if (AsmString[DollarPos] == '}') {
      if (CurVariant == ~0U)
        throw "'}' character found outside of a variant in instruction '"
        + CGI.TheDef->getName() + "'!";
      ++LastEmitted;
      CurVariant = ~0U;
    } else if (DollarPos+1 != AsmString.size() &&
               AsmString[DollarPos+1] == '$') {
      if (CurVariant == Variant || CurVariant == ~0U) {
        AddLiteralString("$");  // "$$" -> $
      }
      LastEmitted = DollarPos+2;
    } else {
      // Get the name of the variable.
      std::string::size_type VarEnd = DollarPos+1;
      
      // handle ${foo}bar as $foo by detecting whether the character following
      // the dollar sign is a curly brace.  If so, advance VarEnd and DollarPos
      // so the variable name does not contain the leading curly brace.
      bool hasCurlyBraces = false;
      if (VarEnd < AsmString.size() && '{' == AsmString[VarEnd]) {
        hasCurlyBraces = true;
        ++DollarPos;
        ++VarEnd;
      }
      
      while (VarEnd < AsmString.size() && isIdentChar(AsmString[VarEnd]))
        ++VarEnd;
      std::string VarName(AsmString.begin()+DollarPos+1,
                          AsmString.begin()+VarEnd);
      
      // Modifier - Support ${foo:modifier} syntax, where "modifier" is passed
      // into printOperand.  Also support ${:feature}, which is passed into
      // PrintSpecial.
      std::string Modifier;
      
      // In order to avoid starting the next string at the terminating curly
      // brace, advance the end position past it if we found an opening curly
      // brace.
      if (hasCurlyBraces) {
        if (VarEnd >= AsmString.size())
          throw "Reached end of string before terminating curly brace in '"
          + CGI.TheDef->getName() + "'";
        
        // Look for a modifier string.
        if (AsmString[VarEnd] == ':') {
          ++VarEnd;
          if (VarEnd >= AsmString.size())
            throw "Reached end of string before terminating curly brace in '"
            + CGI.TheDef->getName() + "'";
          
          unsigned ModifierStart = VarEnd;
          while (VarEnd < AsmString.size() && isIdentChar(AsmString[VarEnd]))
            ++VarEnd;
          Modifier = std::string(AsmString.begin()+ModifierStart,
                                 AsmString.begin()+VarEnd);
          if (Modifier.empty())
            throw "Bad operand modifier name in '"+ CGI.TheDef->getName() + "'";
        }
        
        if (AsmString[VarEnd] != '}')
          throw "Variable name beginning with '{' did not end with '}' in '"
          + CGI.TheDef->getName() + "'";
        ++VarEnd;
      }
      if (VarName.empty() && Modifier.empty())
        throw "Stray '$' in '" + CGI.TheDef->getName() +
        "' asm string, maybe you want $$?";
      
      if (VarName.empty()) {
        // Just a modifier, pass this into PrintSpecial.
        Operands.push_back(AsmWriterOperand("PrintSpecial", ~0U, Modifier));
      } else {
        // Otherwise, normal operand.
        unsigned OpNo = CGI.getOperandNamed(VarName);
        CodeGenInstruction::OperandInfo OpInfo = CGI.OperandList[OpNo];
        
        if (CurVariant == Variant || CurVariant == ~0U) {
          unsigned MIOp = OpInfo.MIOperandNo;
          Operands.push_back(AsmWriterOperand(OpInfo.PrinterMethodName, MIOp,
                                              Modifier));
        }
      }
      LastEmitted = VarEnd;
    }
  }
  
  Operands.push_back(AsmWriterOperand("return;",
                                      AsmWriterOperand::isLiteralStatementOperand));
}

/// MatchesAllButOneOp - If this instruction is exactly identical to the
/// specified instruction except for one differing operand, return the differing
/// operand number.  If more than one operand mismatches, return ~1, otherwise
/// if the instructions are identical return ~0.
unsigned AsmWriterInst::MatchesAllButOneOp(const AsmWriterInst &Other)const{
  if (Operands.size() != Other.Operands.size()) return ~1;
  
  unsigned MismatchOperand = ~0U;
  for (unsigned i = 0, e = Operands.size(); i != e; ++i) {
    if (Operands[i] != Other.Operands[i]) {
      if (MismatchOperand != ~0U)  // Already have one mismatch?
        return ~1U;
      else
        MismatchOperand = i;
    }
  }
  return MismatchOperand;
}