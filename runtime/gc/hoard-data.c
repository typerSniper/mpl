static inline void getChunksFromList (HM_chunkList list,
                              size_t freeSize,
                              HM_chunkList returnList) {

  HM_chunk chunk = list->firstChunk;
  while(chunk!=NULL && HM_getChunkListSize(returnList) < freeSize) {
    HM_chunk t = chunk->nextChunk;
    HM_unlinkChunk(list, chunk);
    HM_appendChunk(returnList, chunk);
    chunk = t;
  }
}

static void printListStats(HM_chunkList list) {
  int countChunks =0;
  for(HM_chunk chunk = list->firstChunk; chunk!=NULL; chunk = chunk->nextChunk) {
    countChunks++;
  }
  printf("size of small list = %zu, numChunks = %d \n",
  HM_getChunkListSize(list), countChunks);
}

static size_t getFreeSize(GC_state s) {
  return HM_getChunkListSize(getFreeListExtraSmall(s)) +
         HM_getChunkListSize(getFreeListSmall(s))+
         HM_getChunkListSize(getFreeListLarge(s));
}

static void enforceUsedFraction(GC_state s) {

  size_t free = getFreeSize(s),
         allocated = s->hoardData->allocated,
         used = allocated - free;

  const float f = 0.5;

  if (f * allocated <= used) {
    return;
  }

  size_t freeSize = allocated - (used/f);

  struct HM_chunkList _upList;
  HM_chunkList upList = &(_upList);
  HM_initChunkList(upList);

  FL_getList(getSegFreeList(s), freeSize, upList)

  assert(upList->firstChunk!=NULL);
  assert(HM_getChunkListSize(upList) >= freeSize);

  HD_appendToSharedList(s->hoardData->sharedfreeList, upList);
  s->hoardData->allocated -= HM_getChunkListSize(upList);
}


static FreeList getFreeList(GC_state s) {return ;}

static void initFreeList(FreeList f) {
  const int numSegments = 7;

  f->segments = malloc(sizeof(struct HM_chunkList)*numSegments);
  f->sizes = (size_t*) malloc(sizeof(size_t)*numSegments);

  for(int i = 0; i < numSegments - 1; i++) {
    HM_initChunkList(&(f->segments[i]));
    f->sizes[i] = BASE_SIZE*pow(2, i);
  }
  // last segment isn't size bound
  HM_initChunkList(&(f->segments[numSegments-1]));
  f->sizes[numSegments - 1] = 0;
  f->numSegments = numSegments;
}

static HM_chunk getChunkFromList(HM_chunkList list, size_t bytesRequested) {
  HM_chunk chunk = list->firstChunk;
  int remainingToCheck = 2;

  while(chunk!=NULL && remainingToCheck > 0) {
    chunk->startGap = 0;
    chunk->frontier = HM_getChunkStart(chunk);
    if (HM_chunkHasBytesFree(chunk, bytesRequested)) return chunk;
  }
  return NULL;
}

static int getFIdx(size_t chunkSize, FreeList f) {
  if (chunkSize <=0) {assert(false); return 0;}

  int idx = log2(chunkSize) - log2(BASE_SIZE);

  if (idx > f->numSegments-1) return (f->numSegments-1);
  else if (idx < 0) return 0;
  // TODO: check this idx again
  else return idx;
}

static bool isUnlinked(HM_chunk chunk) {
  return chunk->nextChunk == NULL && chunk->prevChunk == NULL;
}

void addChunkToFreeList(FreeList f, HM_chunk chunk) {
  assert(isUnlinked(chunk));
  HM_appendChunk(&(f->segments[getQIdx(HM_getChunkSize(chunk), f)]), chunk);
}

HM_chunk checkFreeList (FreeList f, const size_t bytesRequested) {
  int idx = getFIdx(bytesRequested, f);

  while(idx < f->numSegments) {
    HM_chunkList list = &(f->segments[idx]);

    HM_chunk chunk = getChunkFromList(list, bytesRequested);
    if (chunk!=NULL) {
      HM_chunk result =
          HM_splitChunkFront(getFreeListSmall(s), chunk, bytesRequested);
      HM_unlinkChunk(list, chunk);
      HM_unlinkChunk(list, result);
      addChunkToFreeList(f, result);
      return chunk;
    }
    idx++;
  }
  return false;
}

void addChunksToFreeList(FreeList f, HM_chunkList chunkList,
                          HM_chunkList deleteList) {
  HM_chunk chunk, t;
  chunk = chunkList->firstChunk;
  while(chunk!=NULL) {
    t = chunk->nextChunk;
    HM_unlinkChunk(list, chunk);

    int idx = getQIdx(HM_getChunkSize(chunk), f);
    if (idx == f->numSegments - 1) {
      HM_appendChunk(deleteList, chunk);
    }
    else {
      HM_appendChunk(&(f->segments[idx]), chunk);
    }
  }
}


HM_chunk mmapNewChunk(__attribute__((unused)) GC_state s, size_t chunkWidth) {
  assert(isAligned(chunkWidth, HM_BLOCK_SIZE));
  size_t bs = HM_BLOCK_SIZE;
  pointer start = (pointer)GC_mmapAnon(NULL, chunkWidth + bs);
  if (MAP_FAILED == start) {
    return NULL;
  }
  start = (pointer)(uintptr_t)align((uintptr_t)start, bs);
  HM_chunk result = HM_initializeChunk(start, start + chunkWidth);

  LOG(LM_CHUNK, LL_INFO,
    "Mapped a new region of size %zu",
    chunkWidth + bs);

  return result;
}

static HM_chunk getChunksFromSharedList(GC_state s, size_t bytesRequested) {

  struct HM_chunkList _sharedListChunks;
  HM_chunkList sharedListChunks = &(_sharedListChunks);
  HM_initChunkList(sharedListChunks);

  bool satisfied = HD_getSharedListChunks(s->hoardData->sharedfreeList,
                                    bytesRequested,
                                    sharedListChunks);

  s->hoardData->allocated += HM_getChunkListSize(sharedListChunks);

  if (satisfied) {


    HM_chunk chunk = HM_getChunkListFirstChunk(sharedListChunks);
    while(chunk!=NULL) {
        HM_unlinkChunk(sharedListChunks, chunk);
        assert(chunk->frontier == HM_getChunkStart(chunk));

        if (HM_chunkHasBytesFree(chunk, bytesRequested)) {

          // append the chunk to the large list and split it
          HM_appendChunk(getFreeListLarge(s), chunk);
          HM_splitChunkFront(getFreeListLarge(s), chunk, bytesRequested);
          HM_unlinkChunk(getFreeListLarge(s), chunk);

          // TODO: add code to append selectively
          // add remaining chunks to the free list.
          printf("found chunk = %d, request = %d\n",
                HM_getChunkSize(chunk), bytesRequested);
          HM_appendChunkList(getFreeListSmall(s), sharedListChunks);
          return chunk;
        }
        else HM_appendChunk(getFreeListSmall(s), chunk);

        chunk = HM_getChunkListFirstChunk(sharedListChunks);
      }
    }
  else HM_appendChunkList(getFreeListSmall(s), sharedListChunks);

  return NULL;
}

static HM_chunk getChunk(GC_state s, size_t bytesRequested) {

  HM_chunk chunk = checkFreeList(getFreeList(s), bytesRequested);

  if (chunk == NULL) {chunk = getChunksFromSharedList(s, bytesRequested);}

  if(chunk!=NULL) {
    chunk->startGap = 0;
    assert (chunk->frontier == HM_getChunkStart(chunk));
    assert(HM_chunkHasBytesFree(chunk, bytesRequested));

    return chunk;
  }

  size_t bytesNeeded = align(bytesRequested + sizeof(struct HM_chunk), HM_BLOCK_SIZE);
  size_t allocSize = max(bytesNeeded, s->nextChunkAllocSize);
  chunk = mmapNewChunk(s, allocSize);
  if (NULL != chunk) {
    /* success; on next mmap, get even more. */
    if (s->nextChunkAllocSize < (SIZE_MAX / 2)) {
      s->nextChunkAllocSize *= 2;
    }
  } else {
    /* the mmap failed. try again where we only request exactly what we need,
     * and if this still fails, then we're really out of memory and need to
     * abort. */
    LOG(LM_ALLOCATION, LL_INFO,
        "mmap of size %zu failed; trying again for %zu bytes",
        allocSize,
        bytesNeeded);

    chunk = mmapNewChunk(s, bytesNeeded);
    if (NULL == chunk) {
      DIE("Out of memory. Unable to allocate new chunk of size %zu.", bytesNeeded);
    }
    /* also, on next mmap, don't try to allocate so much. */
    if (s->nextChunkAllocSize > 2 * s->controls->allocChunkSize) {
      s->nextChunkAllocSize /= 2;
    }
  }
  s->hoardData->allocated += HM_getChunkSize(chunk);

  // TODO:: hack, fix it later
  HM_prependChunk(getFreeListExtraSmall(s), chunk);
  HM_chunk result =
          HM_splitChunkFront(getFreeListExtraSmall(s), chunk, bytesRequested);

  HM_unlinkChunk(getFreeListExtraSmall(s), chunk);
  HM_unlinkChunk(getFreeListExtraSmall(s), result);
  addChunkToFreeList(getFreeSize(s), result);
  return chunk;
}


void Alloc_init(HD_data h) {
  h->allocated = 0;
  h->sharedfreeList = (SharedList)malloc(sizeof(SharedListStruct));
  initSharedList(h->sharedfreeList);
  h->localFreeList = (FreeList)malloc(sizeof (struct FreeList))
  initFreeList(h->localFreeList);
}

void Alloc_dup(HD_data h, HD_data g) {
  h->allocated = 0;
  h->sharedfreeList = g->sharedfreeList;
  h->localFreeList = (FreeList)malloc(sizeof (struct FreeList))
  initFreeList(h->localFreeList);
}

HM_chunk Alloc_getFreeChunk(GC_state s, size_t bytesRequested) {
  HM_chunk chunk = getChunk(s, bytesRequested);

  assert(chunk->frontier == HM_getChunkStart(chunk));
  assert(HM_chunkHasBytesFree(chunk, bytesRequested));
  HM_reinitializeChunk(chunk);

  return chunk;
}

HM_chunk Alloc_allocateChunk(HM_chunkList list, size_t bytesRequested) {
  GC_state s = pthread_getspecific(gcstate_key);
  HM_chunk chunk = HD_getFreeChunk(s, bytesRequested);

  HM_appendChunk(list, chunk);
  return chunk;
}

void Alloc_freeChunk(GC_state s, HM_chunk chunk) {
  addChunkToFreeList(s->allocator-)
  enforceUsedFraction(s);
}

void Alloc_freeChunkList(GC_state s, HM_chunkList chunkList) {
  struct HM_chunkList _deleteList;
  HM_chunkList deleteList = &(_deleteList);

  addChunksToFreeList (getFreeList(s), chunkList, deleteList);
  Alloc_deleteChunkList(s, deleteList);

  enforceUsedFraction(s);
}

void Alloc_deleteChunkList(GC_state s, HM_chunkList deleteList) {

  s->hoardData->allocated -= HM_getChunkListSize(deleteList);

  HM_chunk chunk = deleteList->firstChunk;
  while (chunk!=NULL) {
    HM_chunk c = chunk;
    chunk = chunk->nextChunk;
    HM_unlinkChunk(deleteList, c);
    GC_release (c, HM_getChunkSize(c));
  }
}
