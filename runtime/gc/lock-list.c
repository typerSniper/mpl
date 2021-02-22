void HD_initSharedList(LockList slist) {
  slist->list = (HM_chunkList)malloc(sizeof(struct HM_chunkList));
  HM_initChunkList(slist->list);

  slist->listLock = (bool*)malloc(sizeof(bool));
  *(slist->listLock) = false;
}

static bool tryLockSharedList(bool* lock) {
  if (!(*(lock)) && !(__sync_val_compare_and_swap(lock, false, true))){
    return true;
  }
  return false;
}
static void acquire(bool* lock) {
  while(true) {
    if (!(*(lock)) && !(__sync_val_compare_and_swap(lock, false, true))){
       return;
    }
  }
}

static void release (bool* lock) {
  assert(*(lock));
  *(lock) = false;
}

void HD_appendToSharedList(LockList slist, HM_chunkList list) {
  acquire(slist->listLock);
  HM_appendChunkList(slist->list, list);
  release(slist->listLock);
}

bool HD_getSharedListChunks(LockList slist,
                                  const size_t bytesRequested,
                                  HM_chunkList list){

  HM_chunkList sharedfreeList = slist->list;

  uint64_t bytesTotal = HM_getChunkListSize(sharedfreeList);
  if (bytesTotal < bytesRequested) {
    return NULL;
  }

  // for now I do not intend to have the proc waiting
  if (!tryLockSharedList(slist->listLock)) {
    return NULL;
  }

  int remainingToCheck = 2;
  HM_chunk chunk = HM_getChunkListFirstChunk(sharedfreeList);
  bool foundChunk = false;
  while (chunk != NULL && remainingToCheck > 0) {

    chunk->startGap = 0;
    chunk->frontier = HM_getChunkStart(chunk);

    if (HM_chunkHasBytesFree(chunk, bytesRequested)) {
      HM_unlinkChunk(sharedfreeList, chunk);
      foundChunk = true;
      break;
    }
    HM_unlinkChunk(sharedfreeList, chunk);
    HM_appendChunk(sharedfreeList, chunk);
    remainingToCheck--;
    chunk = sharedfreeList->firstChunk;
  }

  release(slist->listLock);

  if (foundChunk) HM_appendChunk(list, chunk);
  return foundChunk;

/*
  HM_chunk foundChunk = NULL;
  bool foundChunkB = false;
  bool traverseList = HM_getChunkListFirstChunk(getFreeListSmall(s)) == NULL;
  int count = 3;
  uint64_t largListThresh = s->nextChunkAllocSize;

  while((traverseList || !foundChunkB) && chunk!=NULL) {
    // HM_unlinkChunk(sharedfreeList, chunk);
    // HM_appendChunk(tempList, chunk);
    bytesVisited+=HM_getChunkSize(chunk);

    if(!foundChunkB && HM_chunkHasBytesFree(chunk, bytesRequested)) {
      HM_unlinkChunk(sharedfreeList, chunk);
      HM_appendChunk(tempList, chunk);
      foundChunk = chunk;
      foundChunkB = true;
      chunk = HM_getChunkListFirstChunk(sharedfreeList);
      bytesTotal = min(bytesTotal, HM_getChunkSize(foundChunk)*4);
    }
    else if (traverseList) {
      HM_unlinkChunk(sharedfreeList, chunk);
      if (HM_getChunkSize(chunk) >= largListThresh) {
        HM_prependChunk(tempListLarge, chunk);
      }
      else {
        HM_prependChunk(tempList, chunk);
      }
      chunk = HM_getChunkListFirstChunk(sharedfreeList);
      count --;
      traverseList = (count>0);
    }
    else {
      chunk = chunk->nextChunk;
    }
  }
  release(s->);
  HM_appendChunkList(getFreeListSmall(s), tempList);
  for (HM_chunk chunk = tempListLarge->firstChunk; chunk!=NULL; chunk = chunk->nextChunk) {
    chunk->frontier = HM_getChunkStart(chunk);
  }
  HM_appendChunkList(getFreeListLarge(s), tempListLarge);
  return foundChunk;
*/
}
