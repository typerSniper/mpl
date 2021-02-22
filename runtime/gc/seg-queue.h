#ifndef SEG_QUEUE_H
#define SEG_QUEUE_H


// declare the type and interface of the base queue.
#define BQ CQ
#define BQStruct struct CQ
#define BQ_init(S) CQ_init(S)
#define BQ_enqueueList(S, L) CQ_enqueueList(S, L)
#define BQ_dequeList(S, B, L) CQ_dequeList(S, B, L)

struct SegQ {
  BQ segments;
	size_t * sizes;
	int numSegments;
};

typedef struct SegQ * SegQ;

#if (defined (MLTON_GC_INTERNAL_TYPES))

void Seg_init(SegQ q);
void Seg_enqueueList(SegQ q, HM_chunkList list);
bool Seg_dequeList(SegQ q, const size_t bytesRequested, HM_chunkList list);

#endif
#endif