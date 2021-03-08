
#define casQ(F, O, N) ((__sync_val_compare_and_swap(F, O, N)))


void CQ_init (CQ q) {

	q->firstChunk = NULL;
	q->lastChunk = NULL;
	q->size = 0;
}

void CQ_enqueueList(CQ q, HM_chunkList list) {
	if (list == NULL || list->firstChunk == NULL || list->lastChunk == NULL)
	{return;}


	HM_chunk fchunk = list->firstChunk;
	HM_chunk lchunk = list->lastChunk;

	HM_chunk tail = q->lastChunk;

	if (tail == NULL &&
		 	casQ(&(q->lastChunk), NULL, list->lastChunk) == NULL) {
			q->firstChunk = fchunk;
	}
	else {
		HM_chunk next;
		while(true) {
			tail = q->lastChunk;
			next = tail->nextChunk;

			if (next == NULL &&
					NULL == casQ(&(q->lastChunk->nextChunk), NULL, fchunk)) {
				break;
			}
			else if (next != NULL) {
				tail = q->lastChunk;
				casQ (&(q->lastChunk), tail, next);
			}
		}

		tail = q->lastChunk;
		casQ(&(q->lastChunk), tail, lchunk);
	}
	// its okay if the sizes are a bit under-approximate
	q->size += HM_getChunkListSize(list);

}

HM_chunk HD_popTop(CQ q) {
	if (q->firstChunk == NULL) return NULL;

	HM_chunk head, next, tail;

	while (true) {
		head = q->firstChunk;
		if (head == NULL) return NULL;
		tail = q->lastChunk;

		if (head == tail) {
			next = tail->nextChunk;
			if (next == NULL) return NULL;
			else casQ(&(q->lastChunk), tail, next);
		}
		else {
			next = head->nextChunk;
			if (casQ(&(q->firstChunk), head, next) == head){
				head->nextChunk = NULL;
				break;
			}
		}
	}

	return head;
}


bool CQ_dequeList(CQ q, const size_t bytesRequested, HM_chunkList list) {

  uint64_t bytesTotal = q->size;
  if (bytesTotal < bytesRequested) {
    return NULL;
  }

  int remainingToCheck = 5;
  bool foundChunk = false;

  HM_chunk chunk = HD_popTop(q);
  while (chunk != NULL) {
    chunk->startGap = 0;
    chunk->frontier = HM_getChunkStart(chunk);
    assert(chunk->nextChunk == NULL);

    HM_appendChunk(list, chunk);

    if (HM_chunkHasBytesFree(chunk, bytesRequested)) {
      foundChunk = true;
      break;
    }
    remainingToCheck--;

    chunk = HD_popTop(q);
  }

  return foundChunk;

}

