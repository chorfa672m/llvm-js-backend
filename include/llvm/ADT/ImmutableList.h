//==--- ImmutableList.h - Immutable (functional) list interface --*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the ImmutableList class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ADT_IMLIST_H
#define LLVM_ADT_IMLIST_H

#include "llvm/Support/Allocator.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/DataTypes.h"
#include <cassert>

namespace llvm {

template <typename T> class ImmutableListFactory;
  
template <typename T>
class ImmutableListImpl : public FoldingSetNode {
  T Head;
  ImmutableListImpl* Tail;

  ImmutableListImpl(const T& head, ImmutableListImpl* tail = 0)
    : Head(head), Tail(tail) {}
  
  friend class ImmutableListFactory<T>;
  
  // Do not implement.
  void operator=(const ImmutableListImpl&);
  ImmutableListImpl(const ImmutableListImpl&);
  
public:
  const T& getHead() const { return Head; }
  ImmutableListImpl* getTail() const { return Tail; }
  
  static inline void Profile(FoldingSetNodeID& ID, const T& H,
                             ImmutableListImpl* L){
    ID.AddPointer(L);
    ID.Add(H);
  }
  
  void Profile(FoldingSetNodeID& ID) {
    Profile(ID, Head, Tail);
  }
};
  
/// ImmutableList - This class represents an immutable (functional) list.
///  It is implemented as a smart pointer (wraps ImmutableListImpl), so it
///  it is intended to always be copied by value as if it were a pointer.
///  This interface matches ImmutableSet and ImmutableMap.  ImmutableList
///  objects should almost never be created directly, and instead should
///  be created by ImmutableListFactory objects that manage the lifetime
///  of a group of lists.  When the factory object is reclaimed, all lists
///  created by that factory are released as well.
template <typename T>
class ImmutableList {
public:
  typedef T value_type;
  typedef ImmutableListFactory<T> Factory;

private:
  ImmutableListImpl<T>* X;

public:
  // This constructor should normally only be called by ImmutableListFactory<T>.
  // There may be cases, however, when one needs to extract the internal pointer
  // and reconstruct a list object from that pointer.
  ImmutableList(ImmutableListImpl<T>* x) : X(x) {}

  ImmutableListImpl<T>* getInternalPointer() const {
    return X;
  }
  
  class iterator {
    ImmutableListImpl<T>* L;
  public:
    iterator() : L(0) {}
    iterator(ImmutableList l) : L(l.getInternalPointer()) {}
    
    iterator& operator++() { L = L->Tail; }
    bool operator==(const iterator& I) const { return L == I.L; }
    ImmutableList operator*() const { return L; }
  };

  iterator begin() const { return iterator(X); }
  iterator end() const { return iterator(); }

  bool isEmpty() const { return !X; }
  
  bool isEqual(const ImmutableList& L) const { return X == L.X; }  
  bool operator==(const ImmutableList& L) const { return isEqual(L); }

  const T& getHead() {
    assert (!isEmpty() && "Cannot get the head of an empty list.");
    return X->getHead();
  }
  
  ImmutableList getTail() {
    return X ? X->getTail() : 0;
  }  
};
  
template <typename T>
class ImmutableListFactory {
  typedef ImmutableListImpl<T> ListTy;  
  typedef FoldingSet<ListTy>   CacheTy;
  
  CacheTy Cache;
  uintptr_t Allocator;
  
  bool ownsAllocator() const {
    return Allocator & 0x1 ? false : true;
  }
  
  BumpPtrAllocator& getAllocator() const { 
    return *reinterpret_cast<BumpPtrAllocator*>(Allocator & ~0x1);
  }  

public:
  ImmutableListFactory()
    : Allocator(reinterpret_cast<uintptr_t>(new BumpPtrAllocator())) {}
  
  ImmutableListFactory(BumpPtrAllocator& Alloc)
  : Allocator(reinterpret_cast<uintptr_t>(&Alloc) | 0x1) {}
  
  ~ImmutableListFactory() {
    if (ownsAllocator()) delete &getAllocator();
  }
  
  ImmutableList<T> Concat(const T& Head, ImmutableList<T> Tail) {
    // Profile the new list to see if it already exists in our cache.
    FoldingSetNodeID ID;
    void* InsertPos;
    
    ListTy* TailImpl = Tail.getInternalPointer();
    ListTy::Profile(ID, Head, TailImpl);
    ListTy* L = Cache.FindNodeOrInsertPos(ID, InsertPos);
    
    if (!L) {
      // The list does not exist in our cache.  Create it.
      BumpPtrAllocator& A = getAllocator();
      L = (ListTy*) A.Allocate<ListTy>();
      new (L) ListTy(Head, TailImpl);
    
      // Insert the new list into the cache.
      Cache.InsertNode(L, InsertPos);
    }
    
    return L;
  }
  
  ImmutableList<T> Add(const T& D, ImmutableList<T> L) {
    return Concat(D, L);
  }
  
  ImmutableList<T> GetEmptyList() const {
    return ImmutableList<T>(0);
  }
  
  ImmutableList<T> Create(const T& X) {
    return Concat(X, GetEmptyList());
  }
};
  
} // end llvm namespace

#endif
