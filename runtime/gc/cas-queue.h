#ifndef CAS_QUEUE_H
#define CAS_QUEUE_H

// Invariants: firstChunk <= lastChunk, firstChunk is the head.
// size <= actual-size
// !!! prevChunk field of chunks in the queue is not maintained.
struct CQ {
	HM_chunk firstChunk, lastChunk;
	size_t size;
};


typedef struct CQ * CQ;

#if (defined (MLTON_GC_INTERNAL_TYPES))

void CQ_init(CQ q);
void CQ_enqueue(CQ q, HM_chunkList list);
bool CQ_deque	(CQ q, const size_t bytesRequested, HM_chunkList list);

#endif /* MLTON_GC_INTERNAL_FUNCS */

#endif /* HOARD_DATA_H_ */