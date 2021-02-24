// static for now

#define BASE_SIZE (pow(2, 13))

void Seg_init(SegQ q) {

	const int numSegments = 7;

	q->segments = malloc(sizeof(BQStruct)*numSegments);
	q->sizes = (size_t*) malloc(sizeof(size_t)*numSegments);
	for(int i = 0; i < numSegments - 1; i++) {
		BQ_init(&(q->segments[i]));
		q->sizes[i] = BASE_SIZE*pow(2, i);
	}

	// last segment isn't size bound
	BQ_init(&(q->segments[numSegments-1]));
	q->sizes[numSegments - 1] = 0;

	q->numSegments = numSegments;
}

int getQIdx(size_t chunkSize, SegQ q) {
	if (chunkSize <=0) {assert(false); return 0;}

	int idx = log2(chunkSize) - log2(BASE_SIZE);

	if (idx > q->numSegments-1) return (q->numSegments-1);
	else if (idx < 0) return 0;
	// TODO: check this idx again
	else return idx;
}

// thresholding -- free if too large
// **get rid of the free list large
// when you mmap --> have a global argument for why its needed.
// remainingToCheck?. ==> size_segmentation

void Seg_enqueueList(SegQ q, HM_chunkList list) {
	struct HM_chunkList size_lists[q->numSegments];

	for (int i=0; i < q->numSegments; i++) HM_initChunkList (&(size_lists[i]));

	HM_chunk chunk = list->firstChunk;
  while(chunk!=NULL) {
  	HM_chunk c = chunk->nextChunk;
  	HM_unlinkChunk(list, chunk);
  	HM_appendChunk(&(size_lists[getQIdx(HM_getChunkSize(chunk), q)]), chunk);
  	chunk = c;
  }

	for (int i=0; i < q->numSegments; i++)
		BQ_enqueueList(&(q->segments[i]), &size_lists[i]);
}

bool Seg_dequeList(SegQ q, const size_t bytesRequested, HM_chunkList list) {
	int idx = getQIdx(bytesRequested, q);

	while(idx < q->numSegments) {
		// BQ_dequeList populates the chunkList list
		if (BQ_dequeList(&(q->segments[idx]), bytesRequested, list)) {
			return true;
		}
		idx++;
	}
	return false;
}