/*===-- tracelib.c - Runtime routines for tracing ---------------*- C++ -*-===*
 *
 * Runtime routines for supporting tracing of execution for code generated by
 * LLVM.
 *
 *===----------------------------------------------------------------------===*/

#include "tracelib.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "llvm/Support/DataTypes.h"

/*===---------------------------------------------------------------------=====
 * HASH FUNCTIONS
 *===---------------------------------------------------------------------===*/

/* use #defines until we have inlining */
typedef uintptr_t Index;                 /* type of keys, size for hash table */
typedef uint32_t  Generic;               /* type of values stored in table */ 

/* Index IntegerHashFunc(const Generic value, const Index size) */
#define IntegerHashFunc(value, size) \
  ( ((((Index) value) << 3) ^ (((Index) value) >> 3)) % size )

/* Index IntegerRehashFunc(const Generic oldHashValue, const Index size) */
#define IntegerRehashFunc(oldHashValue, size) \
  ((Index) ((oldHashValue+16) % size)) /* 16 is relatively prime to a Mersenne prime! */

/* Index PointerHashFunc(const void* value, const Index size) */
#define PointerHashFunc(value, size) \
  IntegerHashFunc((Index) value, size)

/* Index PointerRehashFunc(const void* value, const Index size) */
#define PointerRehashFunc(value, size) \
  IntegerRehashFunc((Index) value, size)

/*===---------------------------------------------------------------------=====
 * POINTER-TO-GENERIC HASH TABLE.
 * These should be moved to a separate location: HashTable.[ch]
 *===---------------------------------------------------------------------===*/

typedef enum { FIND, ENTER } ACTION;
typedef char FULLEMPTY;
const FULLEMPTY EMPTY = '\0';
const FULLEMPTY FULL  = '\1';

// List of primes closest to powers of 2 in [2^20 -- 2^30], obtained from
// http://www.utm.edu/research/primes/lists/2small/0bit.html.
// Use these as the successive sizes of the hash table.
#define NUMPRIMES 11
#define FIRSTENTRY 2
const uint PRIMES[NUMPRIMES] = { (1<<20)-3,  (1<<21)-9,  (1<<22)-3, (1<<23)-15,
                                 (1<<24)-3,  (1<<25)-39, (1<<26)-5, (1<<27)-39,
                                 (1<<28)-57, (1<<29)-3,  (1<<30)-35 };
uint CurrentSizeEntry = FIRSTENTRY;

const uint MAX_NUM_PROBES = 4;

typedef struct PtrValueHashEntry_struct {
  void*   key;
  Generic value;
} PtrValueHashEntry;

typedef struct PtrValueHashTable_struct {
  PtrValueHashEntry* table;
  FULLEMPTY* fullEmptyFlags;
  Index capacity;
  Index size;
} PtrValueHashTable;


static Generic LookupOrInsertPtr(PtrValueHashTable* ptrTable, void* ptr,
                                 ACTION action, Generic value);

static void Insert(PtrValueHashTable* ptrTable, void* ptr, Generic value);

static void Delete(PtrValueHashTable* ptrTable, void* ptr);

/* Returns 0 if the item is not found. */
/* void* LookupPtr(PtrValueHashTable* ptrTable, void* ptr) */
#define LookupPtr(ptrTable, ptr) \
  LookupOrInsertPtr(ptrTable, ptr, FIND, (Generic) 0)

void
InitializeTable(PtrValueHashTable* ptrTable, Index newSize)
{
  ptrTable->table = (PtrValueHashEntry*) calloc(newSize,
                                                sizeof(PtrValueHashEntry));
  ptrTable->fullEmptyFlags = (FULLEMPTY*) calloc(newSize, sizeof(FULLEMPTY));
  ptrTable->capacity = newSize;
  ptrTable->size = 0;
}

PtrValueHashTable*
CreateTable(Index initialSize)
{
  PtrValueHashTable* ptrTable =
    (PtrValueHashTable*) malloc(sizeof(PtrValueHashTable));
  InitializeTable(ptrTable, initialSize);
  return ptrTable;
}

void
ReallocTable(PtrValueHashTable* ptrTable, Index newSize)
{
  if (newSize <= ptrTable->capacity)
    return;

#ifndef NDEBUG
  printf("\n***\n*** REALLOCATING SPACE FOR POINTER HASH TABLE.\n");
  printf("*** oldSize = %ld, oldCapacity = %ld\n***\n\n",
         (long) ptrTable->size, (long) ptrTable->capacity); 
#endif

  unsigned int i;
  PtrValueHashEntry* oldTable = ptrTable->table;
  FULLEMPTY* oldFlags = ptrTable->fullEmptyFlags; 
  Index oldSize = ptrTable->size;
  Index oldCapacity = ptrTable->capacity;
  
  /* allocate the new storage and flags and re-insert the old entries */
  InitializeTable(ptrTable, newSize);
  for (i=0; i < oldCapacity; ++i)
    if (oldFlags[i] == FULL)
      Insert(ptrTable, oldTable[i].key, oldTable[i].value);

  assert(ptrTable->size == oldSize && "Incorrect number of entries copied?");

#ifndef NDEBUG
  for (i=0; i < oldCapacity; ++i)
    if (oldFlags[i] == FULL)
      assert(LookupPtr(ptrTable, oldTable[i].key) == oldTable[i].value);
#endif
  
  free(oldTable);
  free(oldFlags);
}

void
DeleteTable(PtrValueHashTable* ptrTable)
{
  free(ptrTable->table);
  free(ptrTable->fullEmptyFlags);
  memset(ptrTable, '\0', sizeof(PtrValueHashTable));
  free(ptrTable);
}

void
InsertAtIndex(PtrValueHashTable* ptrTable, void* ptr, Generic value, Index index)
{
  assert(ptrTable->fullEmptyFlags[index] == EMPTY && "Slot is in use!");
  ptrTable->table[index].key = ptr; 
  ptrTable->table[index].value = value; 
  ptrTable->fullEmptyFlags[index] = FULL;
  ptrTable->size++;
}

void
DeleteAtIndex(PtrValueHashTable* ptrTable, Index index)
{
  assert(ptrTable->fullEmptyFlags[index] == FULL && "Deleting empty slot!");
  ptrTable->table[index].key = 0; 
  ptrTable->table[index].value = (Generic) 0; 
  ptrTable->fullEmptyFlags[index] = EMPTY;
  ptrTable->size--;
}

Index
FindIndex(PtrValueHashTable* ptrTable, void* ptr)
{
  uint numProbes = 1;
  Index index = PointerHashFunc(ptr, ptrTable->capacity);
  if (ptrTable->fullEmptyFlags[index] == FULL)
    {
      if (ptrTable->table[index].key == ptr)
        return index;
      
      /* First lookup failed on non-empty slot: probe further */
      while (numProbes < MAX_NUM_PROBES)
        {
          index = PointerRehashFunc(index, ptrTable->capacity);
          if (ptrTable->fullEmptyFlags[index] == EMPTY)
            break;
          else if (ptrTable->table[index].key == ptr)
            return index;
          ++numProbes;
        }
    }
  
  /* Lookup failed: item is not in the table. */
  /* If last slot is empty, use that slot. */
  /* Otherwise, table must have been reallocated, so search again. */
  
  if (numProbes == MAX_NUM_PROBES)
    { /* table is too full: reallocate and search again */
      if (CurrentSizeEntry >= NUMPRIMES-1) {
        fprintf(stderr, "Out of PRIME Numbers!!!");
        abort();
      }
      ReallocTable(ptrTable, PRIMES[++CurrentSizeEntry]);
      return FindIndex(ptrTable, ptr);
    }
  else
    {
      assert(ptrTable->fullEmptyFlags[index] == EMPTY &&
             "Stopped before finding an empty slot and before MAX probes!");
      return index;
    }
}

/* Look up hash table using 'ptr' as the key.  If an entry exists, return
 * the value mapped to 'ptr'.  If not, and if action==ENTER is specified,
 * create a new entry with value 'value', but return 0 in any case.
 */
Generic
LookupOrInsertPtr(PtrValueHashTable* ptrTable, void* ptr, ACTION action,
                  Generic value)
{
  Index index = FindIndex(ptrTable, ptr);
  if (ptrTable->fullEmptyFlags[index] == FULL &&
      ptrTable->table[index].key == ptr)
    return ptrTable->table[index].value;
  
  /* Lookup failed: item is not in the table */
  /* If action is ENTER, insert item into the table.  Return 0 in any case. */ 
  if (action == ENTER)
    InsertAtIndex(ptrTable, ptr, value, index);

  return (Generic) 0;
}

void
Insert(PtrValueHashTable* ptrTable, void* ptr, Generic value)
{
  Index index = FindIndex(ptrTable, ptr);
  assert(ptrTable->fullEmptyFlags[index] == EMPTY &&
         "ptr is already in the table: delete it first!");
  InsertAtIndex(ptrTable, ptr, value, index);
}

void
Delete(PtrValueHashTable* ptrTable, void* ptr)
{
  Index index = FindIndex(ptrTable, ptr);
  if (ptrTable->fullEmptyFlags[index] == FULL &&
      ptrTable->table[index].key == ptr)
    {
      DeleteAtIndex(ptrTable, index);
    }
}

/*===---------------------------------------------------------------------=====
 * RUNTIME ROUTINES TO MAP POINTERS TO SEQUENCE NUMBERS
 *===---------------------------------------------------------------------===*/

PtrValueHashTable* SequenceNumberTable = NULL;
#define INITIAL_SIZE (PRIMES[FIRSTENTRY])

#define MAX_NUM_SAVED 1024

typedef struct PointerSet_struct {
  char* savedPointers[MAX_NUM_SAVED];   /* 1024 alloca'd ptrs shd suffice */
  unsigned int numSaved;
  struct PointerSet_struct* nextOnStack;     /* implement a cheap stack */ 
} PointerSet;

PointerSet* topOfStack = NULL;

SequenceNumber
HashPointerToSeqNum(char* ptr)
{
  static SequenceNumber count = 0;
  SequenceNumber seqnum;
  if (SequenceNumberTable == NULL) {
    assert(MAX_NUM_PROBES < INITIAL_SIZE+1 && "Initial size too small");
    SequenceNumberTable = CreateTable(INITIAL_SIZE);
  }
  seqnum = (SequenceNumber)
    LookupOrInsertPtr(SequenceNumberTable, ptr, ENTER, count+1);

  if (seqnum == 0)    /* new entry was created with value count+1 */
    seqnum = ++count;    /* so increment counter */

  assert(seqnum <= count && "Invalid sequence number in table!");
  return seqnum;
}

void
ReleasePointerSeqNum(char* ptr)
{ /* if a sequence number was assigned to this ptr, release it */
  if (SequenceNumberTable != NULL)
    Delete(SequenceNumberTable, ptr);
}

void
PushPointerSet()
{
  PointerSet* newSet = (PointerSet*) malloc(sizeof(PointerSet));
  newSet->numSaved = 0;
  newSet->nextOnStack = topOfStack;
  topOfStack = newSet;
}

void
PopPointerSet()
{
  PointerSet* oldSet;
  assert(topOfStack != NULL && "popping from empty stack!");
  oldSet = topOfStack; 
  topOfStack = oldSet->nextOnStack;
  assert(oldSet->numSaved == 0);
  free(oldSet);
}

/* free the pointers! */
static void
ReleaseRecordedPointers(char* savedPointers[MAX_NUM_SAVED],
                        unsigned int numSaved)
{
  unsigned int i;
  for (i=0; i < topOfStack->numSaved; ++i)
    ReleasePointerSeqNum(topOfStack->savedPointers[i]);  
}

void
ReleasePointersPopSet()
{
  ReleaseRecordedPointers(topOfStack->savedPointers, topOfStack->numSaved);
  topOfStack->numSaved = 0;
  PopPointerSet();
}

void
RecordPointer(char* ptr)
{ /* record pointers for release later */
  if (topOfStack->numSaved == MAX_NUM_SAVED) {
    printf("***\n*** WARNING: OUT OF ROOM FOR SAVED POINTERS."
           " ALL POINTERS ARE BEING FREED.\n"
           "*** THE SEQUENCE NUMBERS OF SAVED POINTERS WILL CHANGE!\n*** \n");
    ReleaseRecordedPointers(topOfStack->savedPointers, topOfStack->numSaved);
    topOfStack->numSaved = 0;
  }
  topOfStack->savedPointers[topOfStack->numSaved++] = ptr;
}

/*===---------------------------------------------------------------------=====
 * TEST DRIVER FOR INSTRUMENTATION LIBRARY
 *===---------------------------------------------------------------------===*/

#ifndef TEST_INSTRLIB
#undef  TEST_INSTRLIB /* #define this to turn on by default */
#endif

#ifdef TEST_INSTRLIB
int
main(int argc, char** argv)
{
  int i, j;
  int doRelease = 0;

  INITIAL_SIZE = 5; /* start with small table to test realloc's*/
  
  if (argc > 1 && ! strcmp(argv[1], "-r"))
    {
      PushPointerSet();
      doRelease = 1;
    }
  
  for (i=0; i < argc; ++i)
    for (j=0; argv[i][j]; ++j)
      {
        printf("Sequence number for argc[%d][%d] (%c) = Hash(%p) = %d\n",
               i, j, argv[i][j], argv[i]+j, HashPointerToSeqNum(argv[i]+j));
        
        if (doRelease)
          RecordPointer(argv[i]+j);
      }
  
  if (doRelease)
    ReleasePointersPopSet();     
  
  /* print sequence numbers out again to compare with (-r) and w/o release */
  for (i=argc-1; i >= 0; --i)
    for (j=0; argv[i][j]; ++j)
      printf("Sequence number for argc[%d][%d] (%c) = %d\n",
             i, j, argv[i][j], argv[i]+j, HashPointerToSeqNum(argv[i]+j));
  
  return 0;
}
#endif
