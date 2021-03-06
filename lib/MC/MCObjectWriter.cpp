//===- lib/MC/MCObjectWriter.cpp - MCObjectWriter implementation ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCObjectWriter.h"

using namespace llvm;

MCObjectWriter::~MCObjectWriter() {
}

/// Utility function to encode a SLEB128 value.
void MCObjectWriter::EncodeSLEB128(int64_t Value, raw_ostream &OS) {
  bool More;
  do {
    uint8_t Byte = Value & 0x7f;
    // NOTE: this assumes that this signed shift is an arithmetic right shift.
    Value >>= 7;
    More = !((((Value == 0 ) && ((Byte & 0x40) == 0)) ||
              ((Value == -1) && ((Byte & 0x40) != 0))));
    if (More)
      Byte |= 0x80; // Mark this byte that that more bytes will follow.
    OS << char(Byte);
  } while (More);
}

/// Utility function to encode a ULEB128 value.
void MCObjectWriter::EncodeULEB128(uint64_t Value, raw_ostream &OS) {
  do {
    uint8_t Byte = Value & 0x7f;
    Value >>= 7;
    if (Value != 0)
      Byte |= 0x80; // Mark this byte that that more bytes will follow.
    OS << char(Byte);
  } while (Value != 0);
}
