
#include <pthread.h>
#include <time.h>
#include "platform.h"

/* num of holding thread or -1 if no one*/
volatile int32_t *Parallel_mutexes;

void Parallel_init (void) {
  GC_state s = pthread_getspecific (gcstate_key);

  if (!Proc_isInitialized (s)) {
    Parallel_mutexes = (int32_t *) malloc (s->numberOfProcs * sizeof (int32_t));

    /* Set up call-back state in each worker thread */
    /*
     * SPOONHOWER_NOTE: hack copy the call-from-c-handler into the worker
     * threads assumes this is called by the primary thread
     */
    for (uint32_t proc = 0; proc < s->numberOfProcs; proc++) {
      s->procStates[proc].callFromCHandlerThread = pointerToObjptr(
        GC_copyThread (s, objptrToPointer(s->callFromCHandlerThread,
                                          s->heap->start)),
        s->heap->start);

      Parallel_mutexes[proc] = -1;
    }
    /* Now wake them up! */
    Proc_signalInitialization (s);
  }
}

void Parallel_yield (void) {
  if (Proc_threadInSection ()) {
    GC_state s = pthread_getspecific (gcstate_key);
    ENTER0 (s);
    LEAVE0 (s);
  }
}

/* lock = int* >= 0 if held or -1 if no one */

Pointer Parallel_lockInit (Pointer p_) {
  Int32 *p = (Int32 *)p_;
  *p = -1;
  return (Pointer)p;
}

void Parallel_lockTake (Pointer p_) {
  Int32 *p = (Int32 *)p_;
  //GC_state s = pthread_getspecific (gcstate_key);
  //int32_t myNumber = Proc_processorNumber (s);
  int32_t myNumber = 1;
  /*
  struct timeval tv_lock;
  if (needGCTime (s))
    startWallTiming (&tv_lock);
  */
  //fprintf (stdout, "%d trying for %llX\n", myNumber, (unsigned long long)p);
  do {
    if (Proc_threadInSection ()) {
      //fprintf (stderr, "waiting for gc [%d]\n", Proc_processorNumber (s));
      GC_state s = pthread_getspecific (gcstate_key);
      /*
      if (needGCTime (s))
        stopWallTiming (&tv_lock, &s->cumulativeStatistics->tv_lock);
      */

      ENTER1 (s, p_);
      LEAVE1 (s, p_);
      p = (Int32 *)p_;
      /*
      if (needGCTime (s))
        startWallTiming (&tv_lock);
      */
    }
    //if (*p == myNumber) break;
  } while (*p >= 0 or
           (not __sync_bool_compare_and_swap (p, -1, myNumber)));
  //fprintf (stdout, "%d got %llX\n", myNumber, (unsigned long long)p);
  //*p = myNumber;
  /*
  if (needGCTime (s))
    stopWallTiming (&tv_lock, &s->cumulativeStatistics->tv_lock);
  ++s->cumulativeStatistics->numLocks;
  */
}

void Parallel_lockRelease (Pointer p_) {
  Int32 *p = (Int32 *)p_;
  //GC_state s = pthread_getspecific (gcstate_key);
  //int32_t myNumber = Proc_processorNumber (s);
  int32_t myNumber = 1;

  //fprintf (stdout, "%d releasing' %llX\n", myNumber, (unsigned long long)p);

  /* paranoid mode: */
  if (*p != myNumber) {
    fprintf (stdout, "can't unlock if you don't hold the lock; me: %d, lock: %d\n", myNumber, *p);
    return;
  }
  __sync_bool_compare_and_swap (p, myNumber, -1);
  //*p = -1;
}

void Parallel_dekkerTake (Bool amLeft, Pointer left, Pointer right, Pointer leftsTurn_)
{
  Bool *mine, *other, *leftsTurn;
  if (amLeft) {
    mine = (Bool *)left;
    other = (Bool *)right;
  }
  else {
    mine = (Bool *)right;
    other = (Bool *)left;
  }
  leftsTurn = (Bool *)leftsTurn_;

  //__sync_synchronize ();
  //*mine = 1;
  ////__sync_synchronize ();
  //if (__sync_lock_test_and_set (mine, 1)) {
  if (!__sync_bool_compare_and_swap (mine, 0, 1)) {
    fprintf (stderr, "failed lock!\n");
  }
  while (*other) {
    //__sync_synchronize ();
    if (amLeft != *leftsTurn) {
      GC_state s = pthread_getspecific (gcstate_key);
      //__sync_synchronize ();
      //*mine = 0;
      ////__sync_synchronize ();
      //__sync_lock_release (mine);
      __sync_bool_compare_and_swap (mine, 1, 0);
      while (amLeft != *leftsTurn) {
        //__sync_synchronize ();
        if (Proc_threadInSection ()) {
          Pointer mine_ = (Pointer)mine,
            other_ = (Pointer)other;
          leftsTurn_ = (Pointer)leftsTurn;
          ENTER3 (s, mine_, other_, leftsTurn_);
          LEAVE3 (s, mine_, other_, leftsTurn_);
          mine = (Bool *)mine_;
          other = (Bool *)other_;
          leftsTurn = (Bool *)leftsTurn_;
        }
      }
      //*mine = 1;
      //if (__sync_lock_test_and_set (mine, 1)) {
      if (!__sync_bool_compare_and_swap (mine, 0, 1)) {
        fprintf (stderr, "failed lock!\n");
      }
    }
    //__sync_synchronize ();
  }
}

void Parallel_dekkerRelease (Bool amLeft, Pointer left, Pointer right, Pointer leftsTurn_)
{
  Bool *mine, *leftsTurn;
  if (amLeft) {
    mine = (Bool *)left;
  }
  else {
    mine = (Bool *)right;
  }
  leftsTurn = (Bool *)leftsTurn_;

  //__sync_synchronize ();
  *leftsTurn = amLeft ? 0 : 1;
  //__sync_synchronize ();
  //*mine = 0;
  ////__sync_synchronize ();
  //__sync_lock_release (mine);
  __sync_bool_compare_and_swap (mine, 1, 0);
}

Word32 Parallel_processorNumber (void) {
  GC_state s = pthread_getspecific (gcstate_key);
  return Proc_processorNumber (s);
}

Word32 Parallel_numberOfProcessors (void) {
  GC_state s = pthread_getspecific (gcstate_key);
  return s->numberOfProcs;
}


Word64 Parallel_maxBytesLive (void) {
  GC_state s = pthread_getspecific (gcstate_key);
  return (uint64_t)s->cumulativeStatistics->maxBytesLiveSinceReset;
}

void Parallel_resetBytesLive (void) {
  GC_state s = pthread_getspecific (gcstate_key);
  s->cumulativeStatistics->maxBytesLiveSinceReset = 0;
}

uint64_t Parallel_getTimeInGC (void) {
  GC_state s = pthread_getspecific (gcstate_key);
  uint64_t gcTime = rusageTime (&s->cumulativeStatistics->ru_gc);
  return gcTime;
}

Int32 Parallel_fetchAndAdd (pointer p, Int32 v) {
  return __sync_fetch_and_add ((Int32 *)p, v);
}

bool Parallel_compareAndSwap (pointer p, Int32 old, Int32 new) {
  return __sync_bool_compare_and_swap ((Int32 *)p, old, new);
}
