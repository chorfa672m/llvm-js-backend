//==-- llvm/ADT/ilist_node.h - Intrusive Linked List Helper ------*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the ilist_node class template, which is a convenient
// base class for creating classes that can be used with ilists.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ADT_ILIST_NODE_H
#define LLVM_ADT_ILIST_NODE_H

#include "llvm/Config/config.h"

namespace llvm {

template<typename NodeTy>
struct ilist_traits;

/// ilist_half_node - Base class that provides prev services for sentinels.
///
template<typename NodeTy>
class ilist_half_node {
  friend struct ilist_traits<NodeTy>;
  NodeTy *Prev;
protected:
  NodeTy *getPrev() { return Prev; }
  const NodeTy *getPrev() const { return Prev; }
  void setPrev(NodeTy *P) { Prev = P; }
  ilist_half_node() : Prev(0) {}
};

template<typename NodeTy>
struct ilist_nextprev_traits;

/// ilist_node - Base class that provides next/prev services for nodes
/// that use ilist_nextprev_traits or ilist_default_traits.
///
template<typename NodeTy>
class ilist_node : ilist_half_node<NodeTy> {
  friend struct ilist_nextprev_traits<NodeTy>;
  friend struct ilist_traits<NodeTy>;
  NodeTy *Next;
  NodeTy *getNext() { return Next; }
  const NodeTy *getNext() const { return Next; }
  void setNext(NodeTy *N) { Next = N; }
protected:
  ilist_node() : Next(0) {}
};

/// When assertions are off, the Next field of sentinels
/// will not be accessed. So it is not necessary to allocate
/// space for it. The following macro selects the most
/// efficient traits class. The LLVM_COMPACT_SENTINELS
/// configuration variable controls this.
///
#if defined(LLVM_COMPACT_SENTINELS) && LLVM_COMPACT_SENTINELS
#   define ILIST_NODE ilist_half_node
#else
#   define ILIST_NODE ilist_node
#endif

} // End llvm namespace

#endif
