//===- NeonEmitter.cpp - Generate arm_neon.h for use with clang -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This tablegen backend is responsible for emitting arm_neon.h, which includes
// a declaration and definition of each function specified by the ARM NEON
// compiler interface.  See ARM document DUI0348B.
//
// Each NEON instruction is implemented in terms of 1 or more functions which
// are suffixed with the element type of the input vectors.  Functions may be
// implemented in terms of generic vector operations such as +, *, -, etc. or
// by calling a __builtin_-prefixed function which will be handled by clang's
// CodeGen library.
//
// Additional validation code can be generated by this file when runHeader() is
// called, rather than the normal run() entry point.
//
//===----------------------------------------------------------------------===//

#include "NeonEmitter.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include <string>

using namespace llvm;

/// ParseTypes - break down a string such as "fQf" into a vector of StringRefs,
/// which each StringRef representing a single type declared in the string.
/// for "fQf" we would end up with 2 StringRefs, "f", and "Qf", representing
/// 2xfloat and 4xfloat respectively.
static void ParseTypes(Record *r, std::string &s,
                       SmallVectorImpl<StringRef> &TV) {
  const char *data = s.data();
  int len = 0;

  for (unsigned i = 0, e = s.size(); i != e; ++i, ++len) {
    if (data[len] == 'P' || data[len] == 'Q' || data[len] == 'U')
      continue;

    switch (data[len]) {
      case 'c':
      case 's':
      case 'i':
      case 'l':
      case 'h':
      case 'f':
        break;
      default:
        throw TGError(r->getLoc(),
                      "Unexpected letter: " + std::string(data + len, 1));
        break;
    }
    TV.push_back(StringRef(data, len + 1));
    data += len + 1;
    len = -1;
  }
}

/// Widen - Convert a type code into the next wider type.  char -> short,
/// short -> int, etc.
static char Widen(const char t) {
  switch (t) {
    case 'c':
      return 's';
    case 's':
      return 'i';
    case 'i':
      return 'l';
    default: throw "unhandled type in widen!";
  }
  return '\0';
}

/// Narrow - Convert a type code into the next smaller type.  short -> char,
/// float -> half float, etc.
static char Narrow(const char t) {
  switch (t) {
    case 's':
      return 'c';
    case 'i':
      return 's';
    case 'l':
      return 'i';
    case 'f':
      return 'h';
    default: throw "unhandled type in narrow!";
  }
  return '\0';
}

/// For a particular StringRef, return the base type code, and whether it has
/// the quad-vector, polynomial, or unsigned modifiers set.
static char ClassifyType(StringRef ty, bool &quad, bool &poly, bool &usgn) {
  unsigned off = 0;

  // remember quad.
  if (ty[off] == 'Q') {
    quad = true;
    ++off;
  }

  // remember poly.
  if (ty[off] == 'P') {
    poly = true;
    ++off;
  }

  // remember unsigned.
  if (ty[off] == 'U') {
    usgn = true;
    ++off;
  }

  // base type to get the type string for.
  return ty[off];
}

/// ModType - Transform a type code and its modifiers based on a mod code. The
/// mod code definitions may be found at the top of arm_neon.td.
static char ModType(const char mod, char type, bool &quad, bool &poly,
                    bool &usgn, bool &scal, bool &cnst, bool &pntr) {
  switch (mod) {
    case 't':
      if (poly) {
        poly = false;
        usgn = true;
      }
      break;
    case 'u':
      usgn = true;
      poly = false;
      if (type == 'f')
        type = 'i';
      break;
    case 'x':
      usgn = false;
      poly = false;
      if (type == 'f')
        type = 'i';
      break;
    case 'f':
      if (type == 'h')
        quad = true;
      type = 'f';
      usgn = false;
      break;
    case 'g':
      quad = false;
      break;
    case 'w':
      type = Widen(type);
      quad = true;
      break;
    case 'n':
      type = Widen(type);
      break;
    case 'i':
      type = 'i';
      scal = true;
      break;
    case 'l':
      type = 'l';
      scal = true;
      usgn = true;
      break;
    case 's':
    case 'a':
      scal = true;
      break;
    case 'k':
      quad = true;
      break;
    case 'c':
      cnst = true;
    case 'p':
      pntr = true;
      scal = true;
      break;
    case 'h':
      type = Narrow(type);
      if (type == 'h')
        quad = false;
      break;
    case 'e':
      type = Narrow(type);
      usgn = true;
      break;
    default:
      break;
  }
  return type;
}

/// TypeString - for a modifier and type, generate the name of the typedef for
/// that type.  QUc -> uint8x8_t.
static std::string TypeString(const char mod, StringRef typestr) {
  bool quad = false;
  bool poly = false;
  bool usgn = false;
  bool scal = false;
  bool cnst = false;
  bool pntr = false;

  if (mod == 'v')
    return "void";
  if (mod == 'i')
    return "int";

  // base type to get the type string for.
  char type = ClassifyType(typestr, quad, poly, usgn);

  // Based on the modifying character, change the type and width if necessary.
  type = ModType(mod, type, quad, poly, usgn, scal, cnst, pntr);

  SmallString<128> s;

  if (usgn)
    s.push_back('u');

  switch (type) {
    case 'c':
      s += poly ? "poly8" : "int8";
      if (scal)
        break;
      s += quad ? "x16" : "x8";
      break;
    case 's':
      s += poly ? "poly16" : "int16";
      if (scal)
        break;
      s += quad ? "x8" : "x4";
      break;
    case 'i':
      s += "int32";
      if (scal)
        break;
      s += quad ? "x4" : "x2";
      break;
    case 'l':
      s += "int64";
      if (scal)
        break;
      s += quad ? "x2" : "x1";
      break;
    case 'h':
      s += "float16";
      if (scal)
        break;
      s += quad ? "x8" : "x4";
      break;
    case 'f':
      s += "float32";
      if (scal)
        break;
      s += quad ? "x4" : "x2";
      break;
    default:
      throw "unhandled type!";
      break;
  }

  if (mod == '2')
    s += "x2";
  if (mod == '3')
    s += "x3";
  if (mod == '4')
    s += "x4";

  // Append _t, finishing the type string typedef type.
  s += "_t";

  if (cnst)
    s += " const";

  if (pntr)
    s += " *";

  return s.str();
}

/// BuiltinTypeString - for a modifier and type, generate the clang
/// BuiltinsARM.def prototype code for the function.  See the top of clang's
/// Builtins.def for a description of the type strings.
static std::string BuiltinTypeString(const char mod, StringRef typestr,
                                     ClassKind ck, bool ret) {
  bool quad = false;
  bool poly = false;
  bool usgn = false;
  bool scal = false;
  bool cnst = false;
  bool pntr = false;

  if (mod == 'v')
    return "v"; // void
  if (mod == 'i')
    return "i"; // int

  // base type to get the type string for.
  char type = ClassifyType(typestr, quad, poly, usgn);

  // Based on the modifying character, change the type and width if necessary.
  type = ModType(mod, type, quad, poly, usgn, scal, cnst, pntr);

  // All pointers are void* pointers.  Change type to 'v' now.
  if (pntr) {
    usgn = false;
    poly = false;
    type = 'v';
  }
  // Treat half-float ('h') types as unsigned short ('s') types.
  if (type == 'h') {
    type = 's';
    usgn = true;
  }
  usgn = usgn | poly | ((ck == ClassI || ck == ClassW) && scal && type != 'f');

  if (scal) {
    SmallString<128> s;

    if (usgn)
      s.push_back('U');
    else if (type == 'c')
      s.push_back('S'); // make chars explicitly signed

    if (type == 'l') // 64-bit long
      s += "LLi";
    else
      s.push_back(type);

    if (cnst)
      s.push_back('C');
    if (pntr)
      s.push_back('*');
    return s.str();
  }

  // Since the return value must be one type, return a vector type of the
  // appropriate width which we will bitcast.  An exception is made for
  // returning structs of 2, 3, or 4 vectors which are returned in a sret-like
  // fashion, storing them to a pointer arg.
  if (ret) {
    if (mod >= '2' && mod <= '4')
      return "vv*"; // void result with void* first argument
    if (mod == 'f' || (ck != ClassB && type == 'f'))
      return quad ? "V4f" : "V2f";
    if (ck != ClassB && type == 's')
      return quad ? "V8s" : "V4s";
    if (ck != ClassB && type == 'i')
      return quad ? "V4i" : "V2i";
    if (ck != ClassB && type == 'l')
      return quad ? "V2LLi" : "V1LLi";

    return quad ? "V16Sc" : "V8Sc";
  }

  // Non-return array types are passed as individual vectors.
  if (mod == '2')
    return quad ? "V16ScV16Sc" : "V8ScV8Sc";
  if (mod == '3')
    return quad ? "V16ScV16ScV16Sc" : "V8ScV8ScV8Sc";
  if (mod == '4')
    return quad ? "V16ScV16ScV16ScV16Sc" : "V8ScV8ScV8ScV8Sc";

  if (mod == 'f' || (ck != ClassB && type == 'f'))
    return quad ? "V4f" : "V2f";
  if (ck != ClassB && type == 's')
    return quad ? "V8s" : "V4s";
  if (ck != ClassB && type == 'i')
    return quad ? "V4i" : "V2i";
  if (ck != ClassB && type == 'l')
    return quad ? "V2LLi" : "V1LLi";

  return quad ? "V16Sc" : "V8Sc";
}

/// MangleName - Append a type or width suffix to a base neon function name,
/// and insert a 'q' in the appropriate location if the operation works on
/// 128b rather than 64b.   E.g. turn "vst2_lane" into "vst2q_lane_f32", etc.
static std::string MangleName(const std::string &name, StringRef typestr,
                              ClassKind ck) {
  if (name == "vcvt_f32_f16")
    return name;

  bool quad = false;
  bool poly = false;
  bool usgn = false;
  char type = ClassifyType(typestr, quad, poly, usgn);

  std::string s = name;

  switch (type) {
  case 'c':
    switch (ck) {
    case ClassS: s += poly ? "_p8" : usgn ? "_u8" : "_s8"; break;
    case ClassI: s += "_i8"; break;
    case ClassW: s += "_8"; break;
    default: break;
    }
    break;
  case 's':
    switch (ck) {
    case ClassS: s += poly ? "_p16" : usgn ? "_u16" : "_s16"; break;
    case ClassI: s += "_i16"; break;
    case ClassW: s += "_16"; break;
    default: break;
    }
    break;
  case 'i':
    switch (ck) {
    case ClassS: s += usgn ? "_u32" : "_s32"; break;
    case ClassI: s += "_i32"; break;
    case ClassW: s += "_32"; break;
    default: break;
    }
    break;
  case 'l':
    switch (ck) {
    case ClassS: s += usgn ? "_u64" : "_s64"; break;
    case ClassI: s += "_i64"; break;
    case ClassW: s += "_64"; break;
    default: break;
    }
    break;
  case 'h':
    switch (ck) {
    case ClassS:
    case ClassI: s += "_f16"; break;
    case ClassW: s += "_16"; break;
    default: break;
    }
    break;
  case 'f':
    switch (ck) {
    case ClassS:
    case ClassI: s += "_f32"; break;
    case ClassW: s += "_32"; break;
    default: break;
    }
    break;
  default:
    throw "unhandled type!";
    break;
  }
  if (ck == ClassB)
    s += "_v";

  // Insert a 'q' before the first '_' character so that it ends up before
  // _lane or _n on vector-scalar operations.
  if (quad) {
    size_t pos = s.find('_');
    s = s.insert(pos, "q");
  }
  return s;
}

// Generate the string "(argtype a, argtype b, ...)"
static std::string GenArgs(const std::string &proto, StringRef typestr) {
  bool define = proto.find('i') != std::string::npos;
  char arg = 'a';

  std::string s;
  s += "(";

  for (unsigned i = 1, e = proto.size(); i != e; ++i, ++arg) {
    if (define) {
      // Immediate macro arguments are used directly instead of being assigned
      // to local temporaries; prepend an underscore prefix to make their
      // names consistent with the local temporaries.
      if (proto[i] == 'i')
        s += "__";
    } else {
      s += TypeString(proto[i], typestr) + " __";
    }
    s.push_back(arg);
    if ((i + 1) < e)
      s += ", ";
  }

  s += ")";
  return s;
}

// Macro arguments are not type-checked like inline function arguments, so
// assign them to local temporaries to get the right type checking.
static std::string GenMacroLocals(const std::string &proto, StringRef typestr) {
  char arg = 'a';
  std::string s;

  for (unsigned i = 1, e = proto.size(); i != e; ++i, ++arg) {
    // Do not create a temporary for an immediate argument.
    // That would defeat the whole point of using a macro!
    if (proto[i] == 'i') continue;

    s += TypeString(proto[i], typestr) + " __";
    s.push_back(arg);
    s += " = (";
    s.push_back(arg);
    s += "); ";
  }

  s += "\\\n  ";
  return s;
}

// Use the vmovl builtin to sign-extend or zero-extend a vector.
static std::string Extend(const std::string &proto, StringRef typestr,
                          const std::string &a) {
  std::string s;
  s = MangleName("vmovl", typestr, ClassS);
  s += "(" + a + ")";
  return s;
}

static std::string Duplicate(unsigned nElts, StringRef typestr,
                             const std::string &a) {
  std::string s;

  s = "(" + TypeString('d', typestr) + "){ ";
  for (unsigned i = 0; i != nElts; ++i) {
    s += a;
    if ((i + 1) < nElts)
      s += ", ";
  }
  s += " }";

  return s;
}

static std::string SplatLane(unsigned nElts, const std::string &vec,
                             const std::string &lane) {
  std::string s = "__builtin_shufflevector(" + vec + ", " + vec;
  for (unsigned i = 0; i < nElts; ++i)
    s += ", " + lane;
  s += ")";
  return s;
}

static unsigned GetNumElements(StringRef typestr, bool &quad) {
  quad = false;
  bool dummy = false;
  char type = ClassifyType(typestr, quad, dummy, dummy);
  unsigned nElts = 0;
  switch (type) {
  case 'c': nElts = 8; break;
  case 's': nElts = 4; break;
  case 'i': nElts = 2; break;
  case 'l': nElts = 1; break;
  case 'h': nElts = 4; break;
  case 'f': nElts = 2; break;
  default:
    throw "unhandled type!";
    break;
  }
  if (quad) nElts <<= 1;
  return nElts;
}

// Generate the definition for this intrinsic, e.g. "a + b" for OpAdd.
static std::string GenOpString(OpKind op, const std::string &proto,
                               StringRef typestr) {
  bool quad;
  unsigned nElts = GetNumElements(typestr, quad);

  // If this builtin takes an immediate argument, we need to #define it rather
  // than use a standard declaration, so that SemaChecking can range check
  // the immediate passed by the user.
  bool define = proto.find('i') != std::string::npos;

  std::string ts = TypeString(proto[0], typestr);
  std::string s;
  if (op == OpHi || op == OpLo) {
    s = "union { " + ts + " r; double d; } u; u.d = ";
  } else if (!define) {
    s = "return ";
  }

  switch(op) {
  case OpAdd:
    s += "__a + __b;";
    break;
  case OpSub:
    s += "__a - __b;";
    break;
  case OpMulN:
    s += "__a * " + Duplicate(nElts, typestr, "__b") + ";";
    break;
  case OpMulLane:
    s += "__a * " + SplatLane(nElts, "__b", "__c") + ";";
    break;
  case OpMul:
    s += "__a * __b;";
    break;
  case OpMullN:
    s += Extend(proto, typestr, "__a") + " * " +
      Extend(proto, typestr,
             Duplicate(nElts << (int)quad, typestr, "__b")) + ";";
    break;
  case OpMullLane:
    s += Extend(proto, typestr, "__a") + " * " +
      Extend(proto, typestr,
             SplatLane(nElts, "__b", "__c")) + ";";
    break;
  case OpMull:
    s += Extend(proto, typestr, "__a") + " * " +
      Extend(proto, typestr, "__b") + ";";
    break;
  case OpMlaN:
    s += "__a + (__b * " + Duplicate(nElts, typestr, "__c") + ");";
    break;
  case OpMlaLane:
    s += "__a + (__b * " + SplatLane(nElts, "__c", "__d") + ");";
    break;
  case OpMla:
    s += "__a + (__b * __c);";
    break;
  case OpMlsN:
    s += "__a - (__b * " + Duplicate(nElts, typestr, "__c") + ");";
    break;
  case OpMlsLane:
    s += "__a - (__b * " + SplatLane(nElts, "__c", "__d") + ");";
    break;
  case OpMls:
    s += "__a - (__b * __c);";
    break;
  case OpEq:
    s += "(" + ts + ")(__a == __b);";
    break;
  case OpGe:
    s += "(" + ts + ")(__a >= __b);";
    break;
  case OpLe:
    s += "(" + ts + ")(__a <= __b);";
    break;
  case OpGt:
    s += "(" + ts + ")(__a > __b);";
    break;
  case OpLt:
    s += "(" + ts + ")(__a < __b);";
    break;
  case OpNeg:
    s += " -__a;";
    break;
  case OpNot:
    s += " ~__a;";
    break;
  case OpAnd:
    s += "__a & __b;";
    break;
  case OpOr:
    s += "__a | __b;";
    break;
  case OpXor:
    s += "__a ^ __b;";
    break;
  case OpAndNot:
    s += "__a & ~__b;";
    break;
  case OpOrNot:
    s += "__a | ~__b;";
    break;
  case OpCast:
    s += "(" + ts + ")__a;";
    break;
  case OpConcat:
    s += "(" + ts + ")__builtin_shufflevector((int64x1_t)__a";
    s += ", (int64x1_t)__b, 0, 1);";
    break;
  case OpHi:
    s += "(((float64x2_t)__a)[1]);";
    break;
  case OpLo:
    s += "(((float64x2_t)__a)[0]);";
    break;
  case OpDup:
    s += Duplicate(nElts, typestr, "__a") + ";";
    break;
  case OpDupLane:
    s += SplatLane(nElts, "__a", "__b") + ";";
    break;
  case OpSelect:
    // ((0 & 1) | (~0 & 2))
    s += "(" + ts + ")";
    ts = TypeString(proto[1], typestr);
    s += "((__a & (" + ts + ")__b) | ";
    s += "(~__a & (" + ts + ")__c));";
    break;
  case OpRev16:
    s += "__builtin_shufflevector(__a, __a";
    for (unsigned i = 2; i <= nElts; i += 2)
      for (unsigned j = 0; j != 2; ++j)
        s += ", " + utostr(i - j - 1);
    s += ");";
    break;
  case OpRev32: {
    unsigned WordElts = nElts >> (1 + (int)quad);
    s += "__builtin_shufflevector(__a, __a";
    for (unsigned i = WordElts; i <= nElts; i += WordElts)
      for (unsigned j = 0; j != WordElts; ++j)
        s += ", " + utostr(i - j - 1);
    s += ");";
    break;
  }
  case OpRev64: {
    unsigned DblWordElts = nElts >> (int)quad;
    s += "__builtin_shufflevector(__a, __a";
    for (unsigned i = DblWordElts; i <= nElts; i += DblWordElts)
      for (unsigned j = 0; j != DblWordElts; ++j)
        s += ", " + utostr(i - j - 1);
    s += ");";
    break;
  }
  default:
    throw "unknown OpKind!";
    break;
  }
  if (op == OpHi || op == OpLo) {
    if (!define)
      s += " return";
    s += " u.r;";
  }
  return s;
}

static unsigned GetNeonEnum(const std::string &proto, StringRef typestr) {
  unsigned mod = proto[0];
  unsigned ret = 0;

  if (mod == 'v' || mod == 'f')
    mod = proto[1];

  bool quad = false;
  bool poly = false;
  bool usgn = false;
  bool scal = false;
  bool cnst = false;
  bool pntr = false;

  // Base type to get the type string for.
  char type = ClassifyType(typestr, quad, poly, usgn);

  // Based on the modifying character, change the type and width if necessary.
  type = ModType(mod, type, quad, poly, usgn, scal, cnst, pntr);

  if (usgn)
    ret |= 0x08;
  if (quad && proto[1] != 'g')
    ret |= 0x10;

  switch (type) {
    case 'c':
      ret |= poly ? 5 : 0;
      break;
    case 's':
      ret |= poly ? 6 : 1;
      break;
    case 'i':
      ret |= 2;
      break;
    case 'l':
      ret |= 3;
      break;
    case 'h':
      ret |= 7;
      break;
    case 'f':
      ret |= 4;
      break;
    default:
      throw "unhandled type!";
      break;
  }
  return ret;
}

// Generate the definition for this intrinsic, e.g. __builtin_neon_cls(a)
static std::string GenBuiltin(const std::string &name, const std::string &proto,
                              StringRef typestr, ClassKind ck) {
  std::string s;

  // If this builtin returns a struct 2, 3, or 4 vectors, pass it as an implicit
  // sret-like argument.
  bool sret = (proto[0] >= '2' && proto[0] <= '4');

  // If this builtin takes an immediate argument, we need to #define it rather
  // than use a standard declaration, so that SemaChecking can range check
  // the immediate passed by the user.
  bool define = proto.find('i') != std::string::npos;

  // Check if the prototype has a scalar operand with the type of the vector
  // elements.  If not, bitcasting the args will take care of arg checking.
  // The actual signedness etc. will be taken care of with special enums.
  if (proto.find('s') == std::string::npos)
    ck = ClassB;

  if (proto[0] != 'v') {
    std::string ts = TypeString(proto[0], typestr);

    if (define) {
      if (sret)
        s += ts + " r; ";
      else
        s += "(" + ts + ")";
    } else if (sret) {
      s += ts + " r; ";
    } else {
      s += "return (" + ts + ")";
    }
  }

  bool splat = proto.find('a') != std::string::npos;

  s += "__builtin_neon_";
  if (splat) {
    // Call the non-splat builtin: chop off the "_n" suffix from the name.
    std::string vname(name, 0, name.size()-2);
    s += MangleName(vname, typestr, ck);
  } else {
    s += MangleName(name, typestr, ck);
  }
  s += "(";

  // Pass the address of the return variable as the first argument to sret-like
  // builtins.
  if (sret)
    s += "&r, ";

  char arg = 'a';
  for (unsigned i = 1, e = proto.size(); i != e; ++i, ++arg) {
    std::string args = std::string(&arg, 1);

    // Use the local temporaries instead of the macro arguments.
    args = "__" + args;

    bool argQuad = false;
    bool argPoly = false;
    bool argUsgn = false;
    bool argScalar = false;
    bool dummy = false;
    char argType = ClassifyType(typestr, argQuad, argPoly, argUsgn);
    argType = ModType(proto[i], argType, argQuad, argPoly, argUsgn, argScalar,
                      dummy, dummy);

    // Handle multiple-vector values specially, emitting each subvector as an
    // argument to the __builtin.
    if (proto[i] >= '2' && proto[i] <= '4') {
      // Check if an explicit cast is needed.
      if (argType != 'c' || argPoly || argUsgn)
        args = (argQuad ? "(int8x16_t)" : "(int8x8_t)") + args;

      for (unsigned vi = 0, ve = proto[i] - '0'; vi != ve; ++vi) {
        s += args + ".val[" + utostr(vi) + "]";
        if ((vi + 1) < ve)
          s += ", ";
      }
      if ((i + 1) < e)
        s += ", ";

      continue;
    }

    if (splat && (i + 1) == e)
      args = Duplicate(GetNumElements(typestr, argQuad), typestr, args);

    // Check if an explicit cast is needed.
    if ((splat || !argScalar) &&
        ((ck == ClassB && argType != 'c') || argPoly || argUsgn)) {
      std::string argTypeStr = "c";
      if (ck != ClassB)
        argTypeStr = argType;
      if (argQuad)
        argTypeStr = "Q" + argTypeStr;
      args = "(" + TypeString('d', argTypeStr) + ")" + args;
    }

    s += args;
    if ((i + 1) < e)
      s += ", ";
  }

  // Extra constant integer to hold type class enum for this function, e.g. s8
  if (ck == ClassB)
    s += ", " + utostr(GetNeonEnum(proto, typestr));

  s += ");";

  if (proto[0] != 'v' && sret) {
    if (define)
      s += " r;";
    else
      s += " return r;";
  }
  return s;
}

static std::string GenBuiltinDef(const std::string &name,
                                 const std::string &proto,
                                 StringRef typestr, ClassKind ck) {
  std::string s("BUILTIN(__builtin_neon_");

  // If all types are the same size, bitcasting the args will take care
  // of arg checking.  The actual signedness etc. will be taken care of with
  // special enums.
  if (proto.find('s') == std::string::npos)
    ck = ClassB;

  s += MangleName(name, typestr, ck);
  s += ", \"";

  for (unsigned i = 0, e = proto.size(); i != e; ++i)
    s += BuiltinTypeString(proto[i], typestr, ck, i == 0);

  // Extra constant integer to hold type class enum for this function, e.g. s8
  if (ck == ClassB)
    s += "i";

  s += "\", \"n\")";
  return s;
}

static std::string GenIntrinsic(const std::string &name,
                                const std::string &proto,
                                StringRef outTypeStr, StringRef inTypeStr,
                                OpKind kind, ClassKind classKind) {
  assert(!proto.empty() && "");
  bool define = proto.find('i') != std::string::npos;
  std::string s;

  // static always inline + return type
  if (define)
    s += "#define ";
  else
    s += "__ai " + TypeString(proto[0], outTypeStr) + " ";

  // Function name with type suffix
  std::string mangledName = MangleName(name, outTypeStr, ClassS);
  if (outTypeStr != inTypeStr) {
    // If the input type is different (e.g., for vreinterpret), append a suffix
    // for the input type.  String off a "Q" (quad) prefix so that MangleName
    // does not insert another "q" in the name.
    unsigned typeStrOff = (inTypeStr[0] == 'Q' ? 1 : 0);
    StringRef inTypeNoQuad = inTypeStr.substr(typeStrOff);
    mangledName = MangleName(mangledName, inTypeNoQuad, ClassS);
  }
  s += mangledName;

  // Function arguments
  s += GenArgs(proto, inTypeStr);

  // Definition.
  if (define) {
    s += " __extension__ ({ \\\n  ";
    s += GenMacroLocals(proto, inTypeStr);
  } else {
    s += " { \\\n  ";
  }

  if (kind != OpNone)
    s += GenOpString(kind, proto, outTypeStr);
  else
    s += GenBuiltin(name, proto, outTypeStr, classKind);
  if (define)
    s += " })";
  else
    s += " }";
  s += "\n";
  return s;
}

/// run - Read the records in arm_neon.td and output arm_neon.h.  arm_neon.h
/// is comprised of type definitions and function declarations.
void NeonEmitter::run(raw_ostream &OS) {
  EmitSourceFileHeader("ARM NEON Header", OS);

  // FIXME: emit license into file?

  OS << "#ifndef __ARM_NEON_H\n";
  OS << "#define __ARM_NEON_H\n\n";

  OS << "#ifndef __ARM_NEON__\n";
  OS << "#error \"NEON support not enabled\"\n";
  OS << "#endif\n\n";

  OS << "#include <stdint.h>\n\n";

  // Emit NEON-specific scalar typedefs.
  OS << "typedef float float32_t;\n";
  OS << "typedef int8_t poly8_t;\n";
  OS << "typedef int16_t poly16_t;\n";
  OS << "typedef uint16_t float16_t;\n";

  // Emit Neon vector typedefs.
  std::string TypedefTypes("cQcsQsiQilQlUcQUcUsQUsUiQUiUlQUlhQhfQfPcQPcPsQPs");
  SmallVector<StringRef, 24> TDTypeVec;
  ParseTypes(0, TypedefTypes, TDTypeVec);

  // Emit vector typedefs.
  for (unsigned i = 0, e = TDTypeVec.size(); i != e; ++i) {
    bool dummy, quad = false, poly = false;
    (void) ClassifyType(TDTypeVec[i], quad, poly, dummy);
    if (poly)
      OS << "typedef __attribute__((neon_polyvector_type(";
    else
      OS << "typedef __attribute__((neon_vector_type(";

    unsigned nElts = GetNumElements(TDTypeVec[i], quad);
    OS << utostr(nElts) << "))) ";
    if (nElts < 10)
      OS << " ";

    OS << TypeString('s', TDTypeVec[i]);
    OS << " " << TypeString('d', TDTypeVec[i]) << ";\n";
  }
  OS << "\n";
  OS << "typedef __attribute__((__vector_size__(8)))  "
    "double float64x1_t;\n";
  OS << "typedef __attribute__((__vector_size__(16))) "
    "double float64x2_t;\n";
  OS << "\n";

  // Emit struct typedefs.
  for (unsigned vi = 2; vi != 5; ++vi) {
    for (unsigned i = 0, e = TDTypeVec.size(); i != e; ++i) {
      std::string ts = TypeString('d', TDTypeVec[i]);
      std::string vs = TypeString('0' + vi, TDTypeVec[i]);
      OS << "typedef struct " << vs << " {\n";
      OS << "  " << ts << " val";
      OS << "[" << utostr(vi) << "]";
      OS << ";\n} ";
      OS << vs << ";\n\n";
    }
  }

  OS << "#define __ai static __attribute__((__always_inline__))\n\n";

  std::vector<Record*> RV = Records.getAllDerivedDefinitions("Inst");

  // Emit vmovl intrinsics first so they can be used by other intrinsics.
  emitIntrinsic(OS, Records.getDef("VMOVL"));

  // Unique the return+pattern types, and assign them.
  for (unsigned i = 0, e = RV.size(); i != e; ++i) {
    Record *R = RV[i];
    if (R->getName() != "VMOVL")
      emitIntrinsic(OS, R);
  }

  OS << "#undef __ai\n\n";
  OS << "#endif /* __ARM_NEON_H */\n";
}

/// emitIntrinsic - Write out the arm_neon.h header file definitions for the
/// intrinsics specified by record R.
void NeonEmitter::emitIntrinsic(raw_ostream &OS, Record *R) {
  std::string name = R->getValueAsString("Name");
  std::string Proto = R->getValueAsString("Prototype");
  std::string Types = R->getValueAsString("Types");

  SmallVector<StringRef, 16> TypeVec;
  ParseTypes(R, Types, TypeVec);

  OpKind kind = OpMap[R->getValueAsDef("Operand")->getName()];

  ClassKind classKind = ClassNone;
  if (R->getSuperClasses().size() >= 2)
    classKind = ClassMap[R->getSuperClasses()[1]];
  if (classKind == ClassNone && kind == OpNone)
    throw TGError(R->getLoc(), "Builtin has no class kind");

  for (unsigned ti = 0, te = TypeVec.size(); ti != te; ++ti) {
    if (kind == OpReinterpret) {
      bool outQuad = false;
      bool dummy = false;
      (void)ClassifyType(TypeVec[ti], outQuad, dummy, dummy);
      for (unsigned srcti = 0, srcte = TypeVec.size();
           srcti != srcte; ++srcti) {
        bool inQuad = false;
        (void)ClassifyType(TypeVec[srcti], inQuad, dummy, dummy);
        if (srcti == ti || inQuad != outQuad)
          continue;
        OS << GenIntrinsic(name, Proto, TypeVec[ti], TypeVec[srcti],
                           OpCast, ClassS);
      }
    } else {
      OS << GenIntrinsic(name, Proto, TypeVec[ti], TypeVec[ti],
                         kind, classKind);
    }
  }
  OS << "\n";
}

static unsigned RangeFromType(StringRef typestr) {
  // base type to get the type string for.
  bool quad = false, dummy = false;
  char type = ClassifyType(typestr, quad, dummy, dummy);

  switch (type) {
    case 'c':
      return (8 << (int)quad) - 1;
    case 'h':
    case 's':
      return (4 << (int)quad) - 1;
    case 'f':
    case 'i':
      return (2 << (int)quad) - 1;
    case 'l':
      return (1 << (int)quad) - 1;
    default:
      throw "unhandled type!";
      break;
  }
  assert(0 && "unreachable");
  return 0;
}

/// runHeader - Emit a file with sections defining:
/// 1. the NEON section of BuiltinsARM.def.
/// 2. the SemaChecking code for the type overload checking.
/// 3. the SemaChecking code for validation of intrinsic immedate arguments.
void NeonEmitter::runHeader(raw_ostream &OS) {
  std::vector<Record*> RV = Records.getAllDerivedDefinitions("Inst");

  StringMap<OpKind> EmittedMap;

  // Generate BuiltinsARM.def for NEON
  OS << "#ifdef GET_NEON_BUILTINS\n";
  for (unsigned i = 0, e = RV.size(); i != e; ++i) {
    Record *R = RV[i];
    OpKind k = OpMap[R->getValueAsDef("Operand")->getName()];
    if (k != OpNone)
      continue;

    std::string Proto = R->getValueAsString("Prototype");

    // Functions with 'a' (the splat code) in the type prototype should not get
    // their own builtin as they use the non-splat variant.
    if (Proto.find('a') != std::string::npos)
      continue;

    std::string Types = R->getValueAsString("Types");
    SmallVector<StringRef, 16> TypeVec;
    ParseTypes(R, Types, TypeVec);

    if (R->getSuperClasses().size() < 2)
      throw TGError(R->getLoc(), "Builtin has no class kind");

    std::string name = R->getValueAsString("Name");
    ClassKind ck = ClassMap[R->getSuperClasses()[1]];

    for (unsigned ti = 0, te = TypeVec.size(); ti != te; ++ti) {
      // Generate the BuiltinsARM.def declaration for this builtin, ensuring
      // that each unique BUILTIN() macro appears only once in the output
      // stream.
      std::string bd = GenBuiltinDef(name, Proto, TypeVec[ti], ck);
      if (EmittedMap.count(bd))
        continue;

      EmittedMap[bd] = OpNone;
      OS << bd << "\n";
    }
  }
  OS << "#endif\n\n";

  // Generate the overloaded type checking code for SemaChecking.cpp
  OS << "#ifdef GET_NEON_OVERLOAD_CHECK\n";
  for (unsigned i = 0, e = RV.size(); i != e; ++i) {
    Record *R = RV[i];
    OpKind k = OpMap[R->getValueAsDef("Operand")->getName()];
    if (k != OpNone)
      continue;

    std::string Proto = R->getValueAsString("Prototype");
    std::string Types = R->getValueAsString("Types");
    std::string name = R->getValueAsString("Name");

    // Functions with 'a' (the splat code) in the type prototype should not get
    // their own builtin as they use the non-splat variant.
    if (Proto.find('a') != std::string::npos)
      continue;

    // Functions which have a scalar argument cannot be overloaded, no need to
    // check them if we are emitting the type checking code.
    if (Proto.find('s') != std::string::npos)
      continue;

    SmallVector<StringRef, 16> TypeVec;
    ParseTypes(R, Types, TypeVec);

    if (R->getSuperClasses().size() < 2)
      throw TGError(R->getLoc(), "Builtin has no class kind");

    int si = -1, qi = -1;
    unsigned mask = 0, qmask = 0;
    for (unsigned ti = 0, te = TypeVec.size(); ti != te; ++ti) {
      // Generate the switch case(s) for this builtin for the type validation.
      bool quad = false, poly = false, usgn = false;
      (void) ClassifyType(TypeVec[ti], quad, poly, usgn);

      if (quad) {
        qi = ti;
        qmask |= 1 << GetNeonEnum(Proto, TypeVec[ti]);
      } else {
        si = ti;
        mask |= 1 << GetNeonEnum(Proto, TypeVec[ti]);
      }
    }
    if (mask)
      OS << "case ARM::BI__builtin_neon_"
         << MangleName(name, TypeVec[si], ClassB)
         << ": mask = " << "0x" << utohexstr(mask) << "; break;\n";
    if (qmask)
      OS << "case ARM::BI__builtin_neon_"
         << MangleName(name, TypeVec[qi], ClassB)
         << ": mask = " << "0x" << utohexstr(qmask) << "; break;\n";
  }
  OS << "#endif\n\n";

  // Generate the intrinsic range checking code for shift/lane immediates.
  OS << "#ifdef GET_NEON_IMMEDIATE_CHECK\n";
  for (unsigned i = 0, e = RV.size(); i != e; ++i) {
    Record *R = RV[i];

    OpKind k = OpMap[R->getValueAsDef("Operand")->getName()];
    if (k != OpNone)
      continue;

    std::string name = R->getValueAsString("Name");
    std::string Proto = R->getValueAsString("Prototype");
    std::string Types = R->getValueAsString("Types");

    // Functions with 'a' (the splat code) in the type prototype should not get
    // their own builtin as they use the non-splat variant.
    if (Proto.find('a') != std::string::npos)
      continue;

    // Functions which do not have an immediate do not need to have range
    // checking code emitted.
    if (Proto.find('i') == std::string::npos)
      continue;

    SmallVector<StringRef, 16> TypeVec;
    ParseTypes(R, Types, TypeVec);

    if (R->getSuperClasses().size() < 2)
      throw TGError(R->getLoc(), "Builtin has no class kind");

    ClassKind ck = ClassMap[R->getSuperClasses()[1]];

    for (unsigned ti = 0, te = TypeVec.size(); ti != te; ++ti) {
      std::string namestr, shiftstr, rangestr;

      // Builtins which are overloaded by type will need to have their upper
      // bound computed at Sema time based on the type constant.
      if (Proto.find('s') == std::string::npos) {
        ck = ClassB;
        if (R->getValueAsBit("isShift")) {
          shiftstr = ", true";

          // Right shifts have an 'r' in the name, left shifts do not.
          if (name.find('r') != std::string::npos)
            rangestr = "l = 1; ";
        }
        rangestr += "u = RFT(TV" + shiftstr + ")";
      } else {
        rangestr = "u = " + utostr(RangeFromType(TypeVec[ti]));
      }
      // Make sure cases appear only once by uniquing them in a string map.
      namestr = MangleName(name, TypeVec[ti], ck);
      if (EmittedMap.count(namestr))
        continue;
      EmittedMap[namestr] = OpNone;

      // Calculate the index of the immediate that should be range checked.
      unsigned immidx = 0;

      // Builtins that return a struct of multiple vectors have an extra
      // leading arg for the struct return.
      if (Proto[0] >= '2' && Proto[0] <= '4')
        ++immidx;

      // Add one to the index for each argument until we reach the immediate
      // to be checked.  Structs of vectors are passed as multiple arguments.
      for (unsigned ii = 1, ie = Proto.size(); ii != ie; ++ii) {
        switch (Proto[ii]) {
          default:  immidx += 1; break;
          case '2': immidx += 2; break;
          case '3': immidx += 3; break;
          case '4': immidx += 4; break;
          case 'i': ie = ii + 1; break;
        }
      }
      OS << "case ARM::BI__builtin_neon_" << MangleName(name, TypeVec[ti], ck)
         << ": i = " << immidx << "; " << rangestr << "; break;\n";
    }
  }
  OS << "#endif\n\n";
}
