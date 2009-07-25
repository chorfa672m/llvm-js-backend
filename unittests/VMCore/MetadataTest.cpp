//===- llvm/unittest/VMCore/Metadata.cpp - Metadata unit tests ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/MDNode.h"
#include "llvm/Type.h"
#include "llvm/Support/ValueHandle.h"
#include <sstream>

using namespace llvm;

namespace {

// Test that construction of MDString with different value produces different
// MDString objects, even with the same string pointer and nulls in the string.
TEST(MDStringTest, CreateDifferent) {
  char x[3] = { 'f', 0, 'A' };
  MDString *s1 = getGlobalContext().getMDString(StringRef(&x[0], 3));
  x[2] = 'B';
  MDString *s2 = getGlobalContext().getMDString(StringRef(&x[0], 3));
  EXPECT_NE(s1, s2);
}

// Test that creation of MDStrings with the same string contents produces the
// same MDString object, even with different pointers.
TEST(MDStringTest, CreateSame) {
  char x[4] = { 'a', 'b', 'c', 'X' };
  char y[4] = { 'a', 'b', 'c', 'Y' };

  MDString *s1 = getGlobalContext().getMDString(StringRef(&x[0], 3));
  MDString *s2 = getGlobalContext().getMDString(StringRef(&y[0], 3));
  EXPECT_EQ(s1, s2);
}

// Test that MDString prints out the string we fed it.
TEST(MDStringTest, PrintingSimple) {
  char *str = new char[13];
  strncpy(str, "testing 1 2 3", 13);
  MDString *s = getGlobalContext().getMDString(StringRef(str, 13));
  strncpy(str, "aaaaaaaaaaaaa", 13);
  delete[] str;

  std::ostringstream oss;
  s->print(oss);
  EXPECT_STREQ("metadata !\"testing 1 2 3\"", oss.str().c_str());
}

// Test printing of MDString with non-printable characters.
TEST(MDStringTest, PrintingComplex) {
  char str[5] = {0, '\n', '"', '\\', -1};
  MDString *s = getGlobalContext().getMDString(StringRef(str+0, 5));
  std::ostringstream oss;
  s->print(oss);
  EXPECT_STREQ("metadata !\"\\00\\0A\\22\\5C\\FF\"", oss.str().c_str());
}

// Test the two constructors, and containing other Constants.
TEST(MDNodeTest, Simple) {
  char x[3] = { 'a', 'b', 'c' };
  char y[3] = { '1', '2', '3' };

  MDString *s1 = getGlobalContext().getMDString(StringRef(&x[0], 3));
  MDString *s2 = getGlobalContext().getMDString(StringRef(&y[0], 3));
  ConstantInt *CI = ConstantInt::get(getGlobalContext(), APInt(8, 0));

  std::vector<Value *> V;
  V.push_back(s1);
  V.push_back(CI);
  V.push_back(s2);

  MDNode *n1 = getGlobalContext().getMDNode(&V[0], 3);
  Value *const c1 = n1;
  MDNode *n2 = getGlobalContext().getMDNode(&c1, 1);
  MDNode *n3 = getGlobalContext().getMDNode(&V[0], 3);
  EXPECT_NE(n1, n2);
  EXPECT_EQ(n1, n3);

  EXPECT_EQ(3u, n1->getNumElements());
  EXPECT_EQ(s1, n1->getElement(0));
  EXPECT_EQ(CI, n1->getElement(1));
  EXPECT_EQ(s2, n1->getElement(2));

  EXPECT_EQ(1u, n2->getNumElements());
  EXPECT_EQ(n1, n2->getElement(0));

  std::ostringstream oss1, oss2;
  n1->print(oss1);
  n2->print(oss2);
  EXPECT_STREQ("!0 = metadata !{metadata !\"abc\", i8 0, metadata !\"123\"}\n",
               oss1.str().c_str());
  EXPECT_STREQ("!0 = metadata !{metadata !1}\n"
               "!1 = metadata !{metadata !\"abc\", i8 0, metadata !\"123\"}\n",
               oss2.str().c_str());
}

TEST(MDNodeTest, Delete) {
  Constant *C = ConstantInt::get(Type::Int32Ty, 1);
  Instruction *I = new BitCastInst(C, Type::Int32Ty);

  Value *const V = I;
  MDNode *n = getGlobalContext().getMDNode(&V, 1);
  WeakVH wvh = n;

  EXPECT_EQ(n, wvh);

  delete I;

  std::ostringstream oss;
  wvh->print(oss);
  EXPECT_STREQ("!0 = metadata !{null}\n", oss.str().c_str());
}
}
