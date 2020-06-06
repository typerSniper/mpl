/**
 * @file hierarchical-heap-collection.c
 *
 * @author Jatin Arora
 *
 * @brief
 * Implementation of the concurrent collection interface
 */


#include "concurrent-collection.h"

#if (defined (MLTON_GC_INTERNAL_FUNCS))

#define casCC(F, O, N) ((__sync_val_compare_and_swap(F, O, N)))
bool isChunkInList(HM_chunkList chunkList, HM_chunk T);
bool forwardPtrChunk (GC_state s, objptr *opp, void* rawArgs);
#define ASSERT2 0

void CC_addToStack (ConcurrentPackage cp, pointer p) {
  if(cp->rootList==NULL) {
    // LOG(LM_HH_COLLECTION, LL_FORCE, "Concurrent Stack is not initialised\n");
    // assert(0);
    cp->rootList = (struct CC_stack*) malloc(sizeof(struct CC_stack));
    CC_stack_init(cp->rootList, 2);
  }
  printf("%s\n", "trying to add to stack");
  CC_stack_push(cp->rootList, (void*)p);
}

bool CC_isPointerMarked (pointer p) {
  return ((MARK_MASK & getHeader (p)) == MARK_MASK);
}

// JATIN_NOTE: this function should be called only for in scope objects.
// for out of scope objects the assertion and sanctity of *p is uncertain
// This is because of local collection. In general, CC assumes that the access to chunk level info
// about p is safe since it does not depend on (*p). Once it is established that p is in scope,
// we can call getTransitivePtr on it. Only after the transitive p is reached can we begin to inspect
// (*p). Otherwise, all bets are off because of races
pointer getTransitivePtr(GC_state s, pointer p) {
  objptr op;

  while (hasFwdPtr(p)) {
    op = getFwdPtr(p);
    p = objptrToPointer(op, NULL);
  }
  assert(sizeofObject(s, p) >=0);
  return p;
}


bool isInScope(HM_chunk chunk, ConcurrentCollectArgs* args) {
  // Check that this chunk belongs to this list.
  // I think we should path compress here because we will access the
  // levelHead fairly regularly during collection.
  // if(HM_HH_getChunkList(HM_getLevelHeadPathCompress(chunk)) == args->origList) {
  //   assert(isChunkInList(args->origList, chunk) || isChunkInList(args->repList, chunk));
  //   return true;
  // }
  // return false;
  return chunk->tmpHeap == args->fromHead;
}

bool isChunkSaved(HM_chunk chunk, ConcurrentCollectArgs* args) {
  // Alternative implementation that would require a bool in the chunks
  // check if it's already added in the toSpace/saved
  // return (HM_isChunkMarked(chunk));
  // return chunk->levelHead == args->toHead;
  return (chunk->tmpHeap==args->toHead);
}

// Mark the object uniquely identified by p
void markObj(pointer p) {
  GC_header* headerp = getHeaderp(p);
  GC_header header = *headerp;
  header ^= MARK_MASK;
  *headerp = header;
  /* GC_header header_old = *headerp;
  GC_header header_new = header_old ^ MARK_MASK;
  GC_header q = casCC(headerp, header_old, header_new);
  assert(q==header_old);*/ // commented out cas implementation
}

void linearUnmarkChunkList(GC_state s, ConcurrentCollectArgs* args) {

  HM_assertChunkListInvariants(args->repList);
  HM_chunk chunk = HM_getChunkListFirstChunk (args->repList);

  while(chunk!=NULL) {

    pointer p = HM_getChunkStart(chunk);
    chunk->tmpHeap = NULL;


    assert(chunk->frontier <= chunk->limit);

    while(p != chunk->frontier){
      assert(p < chunk->frontier);
      p = advanceToObjectData(s, p);

      if (CC_isPointerMarked(p)) {
        markObj(p); // mark/unmark is just xor
      }

      p += sizeofObjectNoMetaData(s, p);
    }

    assert(chunk->frontier <= chunk->limit);
    chunk = chunk->nextChunk;
  }
  HM_assertChunkListInvariants(args->repList);
}

#if ASSERT
bool isChunkInList(HM_chunkList chunkList, HM_chunk T) {
  HM_chunk chunk = chunkList->firstChunk;
  while (chunk != NULL) {
    if(chunk == T) {
      return true;
    }
    chunk = chunk->nextChunk;
  }
  return false;
}
#else
bool assertChunkInList(HM_chunkList chunkList, HM_chunk chunk) {
  return true;
}
#endif /* ASSERT */

// This function is exactly the same as in chunk.c.
// The only difference is, it doesn't NULL the levelHead of the unlinking chunk.
void CC_HM_unlinkChunk(HM_chunkList list, HM_chunk chunk) {

  #if ASSERT
    assert(isChunkInList(list, chunk));
  #endif

  if (NULL == chunk->prevChunk) {
    assert(list->firstChunk == chunk);
    list->firstChunk = chunk->nextChunk;
  } else {
    assert(list->firstChunk != chunk);
    chunk->prevChunk->nextChunk = chunk->nextChunk;
  }

  if (NULL == chunk->nextChunk) {
    assert(list->lastChunk == chunk);
    list->lastChunk = chunk->prevChunk;
  } else {
    assert(list->lastChunk != chunk);
    chunk->nextChunk->prevChunk = chunk->prevChunk;
  }

  list->size -= HM_getChunkSize(chunk);

  chunk->prevChunk = NULL;
  chunk->nextChunk = NULL;

#if ASSERT
  HM_assertChunkListInvariants(list);
    assert(!isChunkInList(list, chunk));
#endif

}



void saveChunk(HM_chunk chunk, ConcurrentCollectArgs* args) {
  CC_HM_unlinkChunk(args->origList, chunk);
  HM_appendChunk(args->repList, chunk);

  chunk->tmpHeap = args->toHead;

  HM_assertChunkListInvariants(args->origList);
  HM_assertChunkListInvariants(args->repList);
}


bool saveNoForward(GC_state s, pointer p, void* rawArgs) {
  ConcurrentCollectArgs* args = (ConcurrentCollectArgs*)rawArgs;

  HM_chunk cand_chunk = HM_getChunkOf(p);
  bool chunkSaved = isChunkSaved(cand_chunk, args);
  bool chunkOrig  = (chunkSaved)?true:isInScope(cand_chunk, args);

  if(chunkOrig && !chunkSaved) {
    assert(getTransitivePtr(s, p) == p);
    saveChunk(cand_chunk, args);
  }

  return (chunkSaved || chunkOrig);
}

void markAndScan(GC_state s, pointer p, void* rawArgs) {
  if(!CC_isPointerMarked(p)) {
    markObj(p);
    assert(CC_isPointerMarked(p));
    foreachObjptrInObject(s, p, false, trueObjptrPredicate, NULL,
            forwardPtrChunk, rawArgs);
  }
}

bool forwardPtrChunk (GC_state s, objptr *opp, void* rawArgs) {
  objptr op = *opp;
  assert(isObjptr(op));
  pointer p = objptrToPointer (op, NULL);
  if (isInScope(HM_getChunkOf(p), (ConcurrentCollectArgs*)rawArgs)) {
    p = getTransitivePtr(s, p);
  }
  // JATIN_NOTE: Optimisation, to be enabled later.
  // if(args->rootList->firstChunk == NULL) {
  //   assert(args->rootList->lastChunk == NULL);
  //   // The collection saved everything. No need to scan further.
  //   return true;
  // }

  bool saved = saveNoForward(s, p, rawArgs);

  // forward the object, if in repList and not forwarded already.
  if(saved) {
    #if ASSERT2
      // ConcurrentCollectArgs* args = (ConcurrentCollectArgs*)rawArgs;
      // if(!isChunkInList (args->repList, HM_getChunkOf(p))) {
      //   printf("%s\n", "this is failing\n");
      //   assert(0);
      // }
    #endif
    p = getTransitivePtr(s, p);
    markAndScan(s, p, rawArgs);
  }
  return saved;
}

void forwardDownPtrChunk(GC_state s, __attribute__((unused)) objptr dst,
                          __attribute__((unused)) objptr* field,
                          objptr src, void* rawArgs) {
  #if ASSERT2
  ConcurrentCollectArgs* args =  (ConcurrentCollectArgs*) rawArgs;
  HM_chunk T = HM_getChunkOf((pointer)p);
  // assert(isInScope(T, args->origList) || isChunkSaved(T, args));
    pointer p = objptrToPointer(src, NULL);
  #endif

  forwardPtrChunk(s, &src, rawArgs);
}

void unmarkPtrChunk(GC_state s, objptr* opp, void* rawArgs) {
  objptr op = *opp;
  assert(isObjptr(op));

  pointer p = objptrToPointer (op, NULL);
  HM_chunk chunk = HM_getChunkOf(p);

  // check that getTransitivePtr can be called.
  if (!isChunkSaved(chunk, rawArgs)) {
    return;
  }
  p = getTransitivePtr(s, p);

  if(CC_isPointerMarked (p)) {
    assert(chunk->tmpHeap == ((ConcurrentCollectArgs*)rawArgs)->toHead);
    #if ASSERT2
    GC_header  header = getHeader (p);
    uint16_t bytesNonObjptrs;
    uint16_t numObjptrs;
    GC_objectTypeTag tag;
    printf("%p %p\n", header, p);
    splitHeader(s, header, &tag, NULL, &bytesNonObjptrs, &numObjptrs);
    #endif
    markObj(p);

    assert(!CC_isPointerMarked(p));
    foreachObjptrInObject(s, p, false, trueObjptrPredicate, NULL,
                  unmarkPtrChunk, rawArgs);
  }
}

void unmarkDownPtrChunk (GC_state s, objptr dst, objptr* field, objptr src, void* rawArgs) {
  unmarkPtrChunk(s, &src, rawArgs);
}


void printObjPtrFunction(GC_state s, objptr* opp, void* rawArgs) {
  // printf("\t %p", opp);

}

// this function does more than forwardPtrChunk and is safer. It forwards even if not in scope.
// Recursively however it only calls forwardPtrChunk and not itself
void forceForward(GC_state s, objptr *opp, void* rawArgs) {
  pointer p = objptrToPointer(*opp, NULL);

  // forceForward is called manually. So those things should not have forwarding pointers installed
  assert(getTransitivePtr(s, p) == p);
  bool saved = saveNoForward(s, p, rawArgs);

  // Inlined markAndScan here since we don't want to mark if not in scope
  if(saved && !CC_isPointerMarked(p)) {
    markObj(p);
  }
  foreachObjptrInObject(s, p, false, trueObjptrPredicate, NULL,
          forwardPtrChunk, rawArgs);
}

// Dual unmarking function -- can merge codes by adding another param to args
void unmarkObjects(GC_state s, objptr *opp, void * rawArgs) {
  objptr op = *opp;
  pointer p = objptrToPointer (op, NULL);
  ConcurrentCollectArgs* args = (ConcurrentCollectArgs*)rawArgs;

  HM_chunk cand_chunk = HM_getChunkOf(p);

  if(isInScope(cand_chunk, args)) {
    if(CC_isPointerMarked(p)) {
      // marking is xor. taking xor twice is unmarking
      markObj(p);
      foreachObjptrInObject(s, p, false, trueObjptrPredicate, NULL,
              unmarkObjects, &args);
    }
  }
}


void forceUnmark (GC_state s, objptr* opp, void* rawArgs) {
  pointer p = objptrToPointer(*opp, NULL);
  assert(getTransitivePtr(s, p) == p);
  if(CC_isPointerMarked(p)){
    markObj(p);
  }
  foreachObjptrInObject(s, p, false, trueObjptrPredicate, NULL,
                unmarkPtrChunk, rawArgs);
}


// TODO: This function will develop as the policy is added.
// It is to document all integrity concerns and assert them.
void ensureCallSanity(__attribute__((unused)) GC_state s,
                      HM_HierarchicalHeap targetHH,
                      ConcurrentPackage args) {
  assert(targetHH!=NULL);
  assert(args!=NULL);

  assert(args->snapLeft!=BOGUS_OBJPTR);
  assert(args->snapRight!=BOGUS_OBJPTR);

}

void CC_collectAtPublicLevel(GC_state s, GC_thread thread, uint32_t depth) {
  // 1) ensure the depth is indeed a public depth
  // 2) it is not being collected somewhere already
  // 3) construct arguments to the function call
  // 4) ensure the children can't join back until the collector is done -- not done
  // 5) Races with fork -- The heap could be in process of forking.
  bool gdbArg = false;
  if (gdbArg) {
    depth = (depth>0? depth -1: 0);
  }
  // depth = (depth==1)?(depth+1):depth;

  HM_HierarchicalHeap currentHeap = thread->hierarchicalHeap;
  // LOG(LM_HH_COLLECTION, LL_Log, "called func");

  // Copied the ifs from HM_HHC_collectLocal.
  if (NONE == s->controls->collectionType) {
    return;
  }

  if (s->wsQueueTop == BOGUS_OBJPTR || s->wsQueueBot == BOGUS_OBJPTR) {
    return;
  }
  if (thread->currentDepth == 0 || depth <= 0) {
    return;
  }
  // printf("%s %d\n", "depth = ", depth);

  if(HM_HH_getDepth(currentHeap) < thread->currentDepth) {
    return;
  }

  // uint32_t originalLocalScope = pollCurrentLocalScope(s);

  // if(originalLocalScope > thread->currentDepth) {
  //   return;
  // }

  // assert(originalLocalScope == thread->currentDepth);
  // uint32_t maxDepth = (originalLocalScope>0)?originalLocalScope - 1 : 0;
  uint64_t topval = *(uint64_t*)objptrToPointer(s->wsQueueTop, NULL);
  uint32_t shallowestPrivateLevel = UNPACK_IDX(topval);
  uint32_t maxDepth = (shallowestPrivateLevel>0)?(shallowestPrivateLevel-1):0;

  if(depth > maxDepth) {
    // LOG(LM_HH_COLLECTION, LL_Log, "Level is not public, skipping");
    return;
    // if(depth == thread->currentDepth)
      // return;
  }
  else if (depth >= thread->currentDepth) {
    // printf("%s\n", "disconnect between top and thread");
    return;
  }
  #if ASSERT
    HM_HierarchicalHeap oldStart = currentHeap;
  #endif

  while(HM_HH_getDepth(currentHeap) > depth) {
    currentHeap = currentHeap->nextAncestor;
  }

  if(HM_HH_getDepth(currentHeap) < depth)
    return;

  ConcurrentPackage cp = currentHeap->concurrentPack;

  assert(cp!=NULL); // it has to be the case that the currentHeap is well-formed from (HM_HH_new)
  if(cp->isCollecting
    || cp->snapLeft == BOGUS_OBJPTR
    || cp->snapRight == BOGUS_OBJPTR
    || cp->shouldCollect == false) {
    return;
  }
  else if(casCC(&(cp->isCollecting), false, true)) {
    printf("\t %s\n", "returning because someone else claimed collection");
    assert(0);
    return;
  }
  else{
    LOG(LM_HH_COLLECTION, LL_FORCE, "\t collection turned on isCollect = & depth = ");
    printf("%d %d\n", cp->isCollecting, depth);

  #if ASSERT
    while(oldStart!=currentHeap) {
      printf(" %p => ", (void*) oldStart);
      oldStart = oldStart->nextAncestor;
    }
    printf("FIN: %p\n", currentHeap);
  #endif

  }


  // This point is reachable only after the fork is completed.
  assert(HM_HH_getDepth(currentHeap) == depth);
  assert(s->currentThread == thread);
  // return ;
  CC_collectWithRoots(s, currentHeap, currentHeap->concurrentPack);
  cp->isCollecting = false;
  cp->shouldCollect= false;
  // assertInvariants(thread);

}

void CC_deferredPromote(HM_chunkList x, HM_HierarchicalHeap hh){
  // TODO: Don't know if pointers should be promoted while concurrent collection.
  HM_chunkList y = HM_HH_getRemSet(hh);
  x->firstChunk = y->firstChunk;
  x->lastChunk = y->lastChunk;
  x->size = y->size;
}


void CC_collectWithRoots(GC_state s, HM_HierarchicalHeap targetHH,
              ConcurrentPackage cp) {
  ensureCallSanity(s, targetHH, cp);

  // At the end of collection, repList will contain all the chunks that have
  // some object that is reachable from the roots. origList will contain the
  // chunks in which all objects are garbage. Before exiting, chunks in
  // origList are added to the free list and origList is made equal to repList
  struct HM_chunkList _repList;
  HM_chunkList repList = &(_repList);
  HM_initChunkList(repList);
  HM_chunkList origList = HM_HH_getChunkList(targetHH);
  HM_assertChunkListInvariants(origList);

#if ASSERT
  int lenOrig = 0;
  HM_chunk T = origList->firstChunk;
  while(T!=NULL) {
    lenOrig++;
    T = T->nextChunk;
  }
#endif

  ConcurrentCollectArgs lists = {
    .origList = origList,
    .repList  = repList,
    .toHead = (void*)repList,
    .fromHead = (void*) &(origList)
  };

  // clearing extraneous additions from previous collection : TODO
  /*if(workStack!=NULL) {
    CC_stack_clear(workStack);
  }*/
  for(HM_chunk T = HM_getChunkListFirstChunk(origList); T!=NULL; T = T->nextChunk) {
    #if ASSERT2
      if (T!=stackChunk && HM_getLevelHead(T) != targetHH) {
        printf("%s\n", "this is failing");
        assert(0);
      }
    #endif
    T->tmpHeap = lists.fromHead;
    T->levelHead = targetHH;
  }

  //TODO: Find the right pointer type here
  HM_chunk baseChunk = HM_getChunkOf(targetHH);
  if(isInScope(baseChunk, &lists)) {
    saveChunk(baseChunk, &lists);
  }
  else {
    assert(0);
  }

  struct HM_chunkList downPtrs;
  CC_deferredPromote(&downPtrs, targetHH);
  HM_foreachRemembered(s, &downPtrs, forwardDownPtrChunk, &lists);


  // bool q = forwardPtrChunk(s, &(cp->snapLeft), &lists);
  // assert(q);

  // q = forwardPtrChunk(s, &(cp->snapRight), &lists);
  // assert(q);
  forceForward(s, &(cp->snapLeft), &lists);
  forceForward(s, &(cp->snapRight), &lists);

  objptr stackp;
// The stack and thread are root sets.
// The stack itself might not be in scope and might not get scanned.
  // stackp = cp->stack;
  // assert(stackp != BOGUS_OBJPTR);
  // printf("stack scanned = %p\n", stackp);
  // forceScan(s, &(stackp), &lists);
  stackp = getStackCurrentObjptr(s);
  forceForward(s, &(stackp), &lists);
  forceForward(s, &(s->currentThread), &lists);

  forEachObjptrinStack(s, cp->rootList, forwardPtrChunk, &lists);

  HM_foreachRemembered(s, &downPtrs, unmarkDownPtrChunk, &lists);
  forceUnmark(s, &(cp->snapLeft), &lists);
  forceUnmark(s, &(cp->snapRight), &lists);
  forceUnmark(s, &(stackp), &lists);
  forEachObjptrinStack(s, cp->rootList, unmarkPtrChunk, &lists);


  /*forwardPtrChunk(s, &(s->currentThread), &lists);
  if(workStack!=NULL){
    size_t size = CC_stack_size(workStack);
    while(size!=0){
      void* q = CC_stack_pop(workStack);
      forwardPtrChunk(s, q, &lists);
      // callIfIsObjptr(s, forwardPtrChunk, ((objptr*)(q)), &args);
      q = CC_stack_pop(workStack);
      if(HM_getChunkListFirstChunk(origList)==NULL)
        break;
    }
  }

  HM_chunk chunk= origList->firstChunk;
  while ( chunk!=NULL ) {
  	if(chunk->startGap!=0) { // save all chunks
  		HM_chunk currNextChunk = chunk->nextChunk;
  		saveChunk(chunk, &lists);
  		chunk = currNextChunk;
  	}
  	else
  		chunk=chunk->nextChunk;
  }*/

#if ASSERT
  HM_assertChunkListInvariants(origList);
  HM_assertChunkListInvariants(repList);

  int lenFree = 0;
  int lenRep = 0;
  HM_chunk Q = origList->firstChunk;
  // assert(Q==NULL);
  while(Q!=NULL) {
    lenFree++;
    Q = Q->nextChunk;
  }

  Q = repList->firstChunk;
  while(Q!=NULL){
    lenRep++;
    assert(Q->levelHead == targetHH);
    Q = Q->nextChunk;
  }
  assert(lenRep+lenFree == lenOrig);
  printf("%s %d \n", "Chunks Collected = ", lenFree);

	for(HM_chunk chunk = repList->firstChunk;
		chunk!=NULL;
		chunk=chunk->nextChunk) {
		assert(chunk->levelHead == targetHH);
	}
#endif

  // for(HM_chunk chunk = origList->firstChunk; chunk!=NULL; chunk = chunk->nextChunk){
  //   saveChunk(chunk, &lists);
  // }
  printf("sizes released  = ");
  for(HM_chunk chunk = origList->firstChunk; chunk!=NULL; chunk = chunk->nextChunk) {
    printf("%d ", HM_getChunkSize(chunk));
  }
  printf("\n");
  HM_appendChunkList(getFreeListSmall(s), origList);
  // linearUnmarkChunkList(s, &lists);
  origList->firstChunk = HM_getChunkListFirstChunk(repList);
  origList->lastChunk =  HM_getChunkListLastChunk(repList);
  origList->size = HM_getChunkListSize(repList);
  HM_assertChunkListInvariants(origList);

}
#endif
