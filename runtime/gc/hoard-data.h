#ifndef HOARD_DATA_H_
#define HOARD_DATA_H_



// so we can switch different implementations of shared list
#define SharedList SegQ
#define SharedListStruct struct SegQ
#define initSharedList(S) Seg_init(S);
#define HD_appendToSharedList(S, L) Seg_enqueueList(S, L);
#define HD_getSharedListChunks(S, B, L) Seg_dequeList(S, B, L);




struct FreeList {
  struct HM_chunkList segments;
	size_t * sizes;
	int numSegments;
};

struct AllocatorData {
	uint64_t allocated;
	FreeList localFreeList;
	SharedList sharedfreeList;
}


typedef struct FreeList * FreeList;
typedef struct AllocatorData * AllocatorData;

#if (defined (MLTON_GC_INTERNAL_TYPES))

void FL_init(FreeList q);
void FL_enqueue(FreeList q, HM_chunk chunk);
HM_chunk FL_deque(FreeList q, const size_t bytesRequested);

void FL_getList(FreeList q, const size_t bytesRequested, HM_chunkList retList);

void Alloc_init(AllocatorData h);

void Alloc_dup(AllocatorData h, AllocatorData g);

// void HD_allocate(HD_data h, size_t size);

HM_chunk Alloc_getFreeChunk(GC_state s, size_t bytesRequested);

// Name misrepresents what this function really does
// Its essentially HD_getFreeChunk with the chunk added to the input list
HM_chunk Alloc_allocateChunk(HM_chunkList list, size_t bytesRequested);


void Alloc_freeChunk(GC_state s, HM_chunk chunk) ;
void Alloc_freeChunkList(GC_state s, HM_chunkList chunkList) ;
void Alloc_deleteChunkList(GC_state s, HM_chunkList chunk);

#endif /* MLTON_GC_INTERNAL_FUNCS */

#endif /* HOARD_DATA_H_ */
