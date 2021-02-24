#ifndef HOARD_DATA_H_
#define HOARD_DATA_H_



// so we can switch different implementations of shared list
#define SharedList SegQ
#define SharedListStruct struct SegQ
#define initSharedList(S) Seg_init(S);
#define HD_appendToSharedList(S, L) Seg_enqueueList(S, L);
#define HD_getSharedListChunks(S, B, L) Seg_dequeList(S, B, L);




struct FreeList {
  struct HM_chunkList * segments;
	size_t * sizes;
	int numSegments;
};

typedef struct FreeList * FreeList;


struct GeneralAllocator {
	uint64_t allocated;
	FreeList localFreeList;
	SharedList sharedfreeList;
};

typedef struct GeneralAllocator * GeneralAllocator;

#if (defined (MLTON_GC_INTERNAL_TYPES))

void Alloc_init(GeneralAllocator g);

void Alloc_dup(GeneralAllocator h, GeneralAllocator g);


HM_chunk Alloc_getFreeChunk(GC_state s, size_t bytesRequested);
// Name misrepresents what this function really does
// Its essentially Alloc_getFreeChunk with the chunk added to the input list
HM_chunk Alloc_allocateChunk(HM_chunkList list, size_t bytesRequested);

void Alloc_freeChunk(GC_state s, HM_chunk chunk);
void Alloc_freeChunkList(GC_state s, HM_chunkList chunkList);
void Alloc_deleteChunkList(HM_chunkList chunk);

#endif /* MLTON_GC_INTERNAL_FUNCS */

#endif /* HOARD_DATA_H_ */
