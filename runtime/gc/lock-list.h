#ifndef LOCK_LIST_H
#define LOCK_LIST_H

struct LockList {
	HM_chunkList list;
  bool* listLock;
};
typedef struct LockList * LockList;

#if (defined (MLTON_GC_INTERNAL_TYPES))

void HD_initSharedList(LockList slist);
HM_chunk HD_checkSharedListForChunk(LockList slist, const size_t bytesRequested);
bool HD_getSharedListChunks(LockList slist,
																	const size_t bytesRequested,
																	HM_chunkList list);

#endif /* MLTON_GC_INTERNAL_FUNCS */

#endif /* HOARD_DATA_H_ */