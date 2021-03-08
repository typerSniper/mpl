#define BASE_SIZE (pow(2, 13))

static void printListStats(HM_chunkList list) {
  int countChunks =0;
  for(HM_chunk chunk = list->firstChunk; chunk!=NULL; chunk = chunk->nextChunk) {
    countChunks++;
  }
  printf("size of small list = %zu, numChunks = %d \n",
  HM_getChunkListSize(list), countChunks);
}

// TODO: Optimize this out by storing.
size_t getFreeListSize(FreeList f) {
  size_t size = 0;
  for (int i=0; i<f->numSegments; i++)
    size+=HM_getChunkListSize(&(f->segments[i]));
  return size;
}

static void getChunksFromList(FreeList f, const size_t bytesRequested,
                              HM_chunkList retList) {

  int idx = f->numSegments-1;
  size_t sizeAdded = 0;

  HM_chunk t, chunk;
  HM_chunkList list;

  while(idx >= 0 && sizeAdded < bytesRequested) {
    list = &(f->segments[idx]);
    chunk = list->firstChunk;

    while(sizeAdded < bytesRequested && chunk!=NULL) {
      t = chunk->nextChunk;

      HM_unlinkChunk(list, chunk);
      HM_appendChunk(retList, chunk);
      sizeAdded+=HM_getChunkSize(chunk);

      chunk = t;
    }
    idx--;
  }
}

static FreeList getFreeList(GC_state s) {return s->allocator->localFreeList;}

static SharedList getSharedFreeList(GC_state s) {
  return s->allocator->sharedfreeList;
}

static inline void updateAllocCounter(GC_state s, size_t change) {
  s->allocator->allocated+=change;
}

static inline size_t getAllocCounter(GC_state s) {
  return s->allocator->allocated;
}

static size_t getThresholdSize(GC_state s) {
  // return s->nextChunkAllocSize;
  FreeList f = getFreeList(s);
  return align ((f->sizes[f->numSegments - 2])*4,
   HM_BLOCK_SIZE);
}

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
  if (bytesRequested>HM_getChunkListSize(list)) return NULL;

  HM_chunk chunk = list->firstChunk;
  int remainingToCheck = 2;

  while(chunk!=NULL && remainingToCheck > 0) {
    chunk->startGap = 0;
    chunk->frontier = HM_getChunkStart(chunk);
    if (HM_chunkHasBytesFree(chunk, bytesRequested)) return chunk;
    chunk = chunk->nextChunk;
    remainingToCheck--;
  }
  return NULL;
}

static int getFIdx(size_t chunkSize, FreeList f) {
  if (chunkSize <=0) {assert(false); return 0;}

  int idx = (log2(chunkSize) - log2(BASE_SIZE));

  if (idx > f->numSegments-1) return (f->numSegments-1);
  else if (idx < 0) return 0;
  else {
    return idx;
  }
}

static bool isUnlinked(HM_chunk chunk) {
  return chunk->nextChunk == NULL && chunk->prevChunk == NULL;
}

void addChunkToFreeList(FreeList f, HM_chunk chunk) {
  assert(isUnlinked(chunk));
  HM_appendChunk(&(f->segments[getFIdx(HM_getChunkSize(chunk), f)]), chunk);
}

HM_chunk checkFreeList (FreeList f, const size_t bytesRequested) {
  int idx = getFIdx(bytesRequested, f);

  while(idx < f->numSegments) {
    HM_chunkList list = &(f->segments[idx]);

    HM_chunk chunk = getChunkFromList(list, bytesRequested);
    if (chunk!=NULL) {
      HM_chunk result =
          HM_splitChunkFront(list, chunk, bytesRequested);
      HM_unlinkChunk(list, chunk);

      if (result!=NULL) {
        HM_unlinkChunk(list, result);
        addChunkToFreeList(f, result);
      }

      return chunk;
    }
    idx++;
  }
  return false;
}

void addChunksToFreeList(FreeList f, HM_chunkList list) {

  struct HM_chunkList _deleteList;
  HM_chunkList deleteList = &(_deleteList);

  HM_chunk chunk, t;
  chunk = list->firstChunk;
  while(chunk!=NULL) {
    t = chunk->nextChunk;
    HM_unlinkChunk(list, chunk);

    int idx = getFIdx(HM_getChunkSize(chunk), f);
    HM_appendChunk(&(f->segments[idx]), chunk);

    chunk = t;
  }
}

void addFreedChunksToFreeList(GC_state s, FreeList f, HM_chunkList list) {

  struct HM_chunkList _deleteList;
  HM_chunkList deleteList = &(_deleteList);
  HM_initChunkList(deleteList);

  HM_chunk chunk, t;
  chunk = list->firstChunk;
  while(chunk!=NULL) {
    t = chunk->nextChunk;
    HM_unlinkChunk(list, chunk);


    if(HM_getChunkSize(chunk) < getThresholdSize(s)) {
      int idx = getFIdx(HM_getChunkSize(chunk), f);
      HM_appendChunk(&(f->segments[idx]), chunk);
    }
    else {
      HM_appendChunk(deleteList, chunk);
    }

    chunk = t;
  }

  Alloc_deleteChunkList(deleteList);
}

static size_t deleteBigChunks(GC_state s) {
  size_t thresh = getThresholdSize(s), deleted = 0;

  FreeList f = getFreeList(s);
  int idx = getFIdx(thresh, f);
  int numSegments = f->numSegments;

  for(int i=idx; i<numSegments; i++) {
    HM_chunkList list = &(f->segments[i]);
    HM_chunk chunk = list->firstChunk;

    while(chunk!=NULL) {
      HM_chunk t = chunk->nextChunk;
      if (HM_getChunkSize(chunk) > thresh){
        HM_unlinkChunk(list, chunk);
        deleted+=HM_getChunkSize(chunk);
        // printf("deleting %ld\n", HM_getChunkSize(chunk));
        GC_release (chunk, HM_getChunkSize(chunk));
      }
      chunk = t;
    }
  }




  // list = &(f->segments[0]);
  // chunk = list->firstChunk;
  // size_t freed = 0;
  // size_t target = HM_getChunkListSize(list)/2;

  // while(chunk!=NULL && freed<target) {
  //   t = chunk->nextChunk;
  //   if (HM_getChunkSize(chunk) > thresh){
  //     HM_unlinkChunk(list, chunk);
  //     freed+=HM_getChunkSize(chunk);
  //     GC_release (chunk, HM_getChunkSize(chunk));
  //   }
  //   chunk = t;
  // }


  return deleted;
}

static void enforceUsedFraction(GC_state s) {
  size_t free = getFreeListSize(getFreeList(s)),
         allocated = getAllocCounter(s),
         used = allocated - free;

  const float f = 0.5;

  if (f * allocated <= used) {
    return;
  }

  // try freeing the big chunks in freeList
  size_t deleted = 0;
  // size_t deleted = deleteBigChunks(s);

  free-=deleted;
  allocated-=deleted;
  if (f * allocated <= used) {
    return;
  }

  size_t freeSize = allocated - (used/f);

  struct HM_chunkList _upList;
  HM_chunkList upList = &(_upList);
  HM_initChunkList(upList);

  getChunksFromList(getFreeList(s), freeSize, upList);

  assert(upList->firstChunk!=NULL);
  assert(HM_getChunkListSize(upList) >= freeSize);
  // printf("size of first = %d & last = %d\n",
  //   HM_getChunkListSize(&(s->allocator->localFreeList->segments[0])),
  //   HM_getChunkListSize(&(s->allocator->localFreeList->segments[6])));
  HD_appendToSharedList(getSharedFreeList(s), upList);
  updateAllocCounter(s, -1*HM_getChunkListSize(upList));
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

  bool satisfied = HD_getSharedListChunks(getSharedFreeList(s),
                                    bytesRequested,
                                    sharedListChunks);

  size_t newAllocation = HM_getChunkListSize(sharedListChunks);

  updateAllocCounter(s, newAllocation);

  if (satisfied) {

    HM_chunk chunk = HM_getChunkListFirstChunk(sharedListChunks);
    while(chunk!=NULL) {
        assert(chunk->frontier == HM_getChunkStart(chunk));

        if (HM_chunkHasBytesFree(chunk, bytesRequested)) {

          // printf("found chunk from shared = %d, requested = %d, total = %d \n",
                  // HM_getChunkSize(chunk), bytesRequested, newAllocation);

          HM_chunk result =
            HM_splitChunkFront(sharedListChunks, chunk, bytesRequested);

          HM_unlinkChunk(sharedListChunks, chunk);

          if (result!=NULL) {
            HM_unlinkChunk(sharedListChunks, result);
            addChunkToFreeList(getFreeList(s), result);
          }

          addChunksToFreeList(getFreeList(s), sharedListChunks);
          return chunk;
        }
        else {
          HM_unlinkChunk(sharedListChunks, chunk);
          addChunkToFreeList(getFreeList(s), chunk);
        }
        chunk = HM_getChunkListFirstChunk(sharedListChunks);
      }
    }

  addChunksToFreeList(getFreeList(s), sharedListChunks);

  return NULL;
}

static HM_chunk getChunk(GC_state s, size_t bytesRequested) {


  bool bigAlloc = bytesRequested > getThresholdSize(s);

  if (!bigAlloc) {

    HM_chunk chunk = checkFreeList(getFreeList(s), bytesRequested);

    if (chunk == NULL) {chunk = getChunksFromSharedList(s, bytesRequested);}

    if(chunk!=NULL) {
      chunk->startGap = 0;
      assert (chunk->frontier == HM_getChunkStart(chunk));
      assert(HM_chunkHasBytesFree(chunk, bytesRequested));

      return chunk;
    }
  }
  size_t bytesNeeded = align(bytesRequested +
                                sizeof(struct HM_chunk), HM_BLOCK_SIZE);
  size_t allocSize = bytesNeeded;
  size_t nAllocSize = *(s->nextChunkAllocSize);

  if(!bigAlloc){
     allocSize = max(bytesNeeded, nAllocSize);
  }

  HM_chunk chunk = mmapNewChunk(s, allocSize);
  if (NULL != chunk) {
    /* success; on next mmap, get even more. */
    if (!bigAlloc && *(s->nextChunkAllocSize) < SIZE_MAX/2) {

      *(s->nextChunkAllocSize) = nAllocSize*2;
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
    if (nAllocSize > 2 * s->controls->allocChunkSize) {
      *(s->nextChunkAllocSize) = nAllocSize/2;
    }
  }
  updateAllocCounter(s, HM_getChunkSize(chunk));

  // TODO:: hack, fix it later
  HM_chunk result =
        HM_splitChunkFrontWithoutList(chunk, bytesRequested);

  if (result!=NULL) {
    addChunkToFreeList(getFreeList(s), result);
  }

  return chunk;
}


void Alloc_init(GeneralAllocator g) {
  g->allocated = 0;
  g->sharedfreeList = (SharedList)malloc(sizeof(SharedListStruct));
  initSharedList(g->sharedfreeList);
  g->localFreeList = (FreeList)malloc(sizeof (struct FreeList));
  initFreeList(g->localFreeList);
}

void Alloc_dup(GeneralAllocator h, GeneralAllocator g) {
  h->allocated = 0;
  h->sharedfreeList = g->sharedfreeList;
  h->localFreeList = (FreeList)malloc(sizeof (struct FreeList));
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
  HM_chunk chunk = Alloc_getFreeChunk(s, bytesRequested);

  HM_appendChunk(list, chunk);
  return chunk;
}

void Alloc_freeChunk(GC_state s, HM_chunk chunk) {
  addChunkToFreeList(getFreeList(s), chunk);
  enforceUsedFraction(s);
}

void Alloc_freeChunkList(GC_state s, HM_chunkList chunkList) {
  addFreedChunksToFreeList (s, getFreeList(s), chunkList);
  enforceUsedFraction(s);
}

void Alloc_deleteChunkList(HM_chunkList deleteList) {
  GC_state s = pthread_getspecific(gcstate_key);

  updateAllocCounter(s, -1*HM_getChunkListSize(deleteList));

  HM_chunk chunk = deleteList->firstChunk;
  while (chunk!=NULL) {
    HM_chunk c = chunk;
    chunk = chunk->nextChunk;
    HM_unlinkChunk(deleteList, c);
    GC_release (c, HM_getChunkSize(c));
  }
}
