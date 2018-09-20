/*
 * Copyright (c) 2013, 2018, Red Hat, Inc. and/or its affiliates.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_HPP

#include "gc/shared/markBitMap.hpp"
#include "gc/shared/softRefPolicy.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahAllocRequest.hpp"
#include "gc/shenandoah/shenandoahHeapLock.hpp"
#include "gc/shenandoah/shenandoahEvacOOMHandler.hpp"
#include "gc/shenandoah/shenandoahSharedVariables.hpp"
#include "gc/shenandoah/shenandoahWorkGroup.hpp"
#include "services/memoryManager.hpp"

class ConcurrentGCTimer;
class PLABStats;
class ReferenceProcessor;
class ShenandoahAsserts;
class ShenandoahAllocTracker;
class ShenandoahCollectorPolicy;
class ShenandoahControlThread;
class ShenandoahGCSession;
class ShenandoahHeuristics;
class ShenandoahMarkingContext;
class ShenandoahPhaseTimings;
class ShenandoahHeap;
class ShenandoahHeapRegion;
class ShenandoahHeapRegionClosure;
class ShenandoahCollectionSet;
class ShenandoahFreeSet;
class ShenandoahConcurrentMark;
class ShenandoahMarkCompact;
class ShenandoahPacer;
class ShenandoahTraversalGC;
class ShenandoahVerifier;
class ShenandoahMonitoringSupport;

class ShenandoahRegionIterator : public StackObj {
private:
  volatile size_t _index;
  ShenandoahHeap* _heap;

  // No implicit copying: iterators should be passed by reference to capture the state
  ShenandoahRegionIterator(const ShenandoahRegionIterator& that);
  ShenandoahRegionIterator& operator=(const ShenandoahRegionIterator& o);

public:
  ShenandoahRegionIterator();
  ShenandoahRegionIterator(ShenandoahHeap* heap);

  // Reset iterator to default state
  void reset();

  // Returns next region, or NULL if there are no more regions.
  // This is multi-thread-safe.
  inline ShenandoahHeapRegion* next();

  // This is *not* MT safe. However, in the absence of multithreaded access, it
  // can be used to determine if there is more work to do.
  bool has_next() const;
};

class ShenandoahHeapRegionClosure : public StackObj {
public:
  // typically called on each region until it returns true;
  virtual bool heap_region_do(ShenandoahHeapRegion* r) = 0;
};

class ShenandoahUpdateRefsClosure: public OopClosure {
private:
  ShenandoahHeap* _heap;

  template <class T>
  inline void do_oop_work(T* p);

public:
  ShenandoahUpdateRefsClosure();
  inline void do_oop(oop* p);
  inline void do_oop(narrowOop* p);
};

#ifdef ASSERT
class ShenandoahAssertToSpaceClosure : public OopClosure {
private:
  template <class T>
  void do_oop_work(T* p);
public:
  void do_oop(narrowOop* p);
  void do_oop(oop* p);
};
#endif

class ShenandoahAlwaysTrueClosure : public BoolObjectClosure {
public:
  bool do_object_b(oop p) { return true; }
};

class ShenandoahForwardedIsAliveClosure: public BoolObjectClosure {
private:
  ShenandoahMarkingContext* const _mark_context;
public:
  ShenandoahForwardedIsAliveClosure();
  bool do_object_b(oop obj);
};

class ShenandoahIsAliveClosure: public BoolObjectClosure {
private:
  ShenandoahMarkingContext* const _mark_context;
public:
  ShenandoahIsAliveClosure();
  bool do_object_b(oop obj);
};

class VMStructs;

// // A "ShenandoahHeap" is an implementation of a java heap for HotSpot.
// // It uses a new pauseless GC algorithm based on Brooks pointers.
// // Derived from G1

// //
// // CollectedHeap
// //    SharedHeap
// //      ShenandoahHeap
class ShenandoahHeap : public CollectedHeap {
  friend class ShenandoahAsserts;
  friend class VMStructs;
  friend class ShenandoahGCSession;

  enum CancelState {

    // Normal state. GC has not been cancelled and is open for cancellation.
    // Worker threads can suspend for safepoint.
    CANCELLABLE,

    // GC has been cancelled. Worker threads can not suspend for
    // safepoint but must finish their work as soon as possible.
    CANCELLED,

    // GC has not been cancelled and must not be cancelled. At least
    // one worker thread checks for pending safepoint and may suspend
    // if a safepoint is pending.
    NOT_CANCELLED

  };

public:
  // GC state describes the important parts of collector state, that may be
  // used to make barrier selection decisions in the native and generated code.
  // Multiple bits can be set at once.
  //
  // Important invariant: when GC state is zero, the heap is stable, and no barriers
  // are required.
  enum GCStateBitPos {
    // Heap has forwarded objects: need RB, ACMP, CAS barriers.
    HAS_FORWARDED_BITPOS   = 0,

    // Heap is under marking: needs SATB barriers.
    MARKING_BITPOS    = 1,

    // Heap is under evacuation: needs WB barriers. (Set together with UNSTABLE)
    EVACUATION_BITPOS = 2,

    // Heap is under updating: needs SVRB/SVWB barriers.
    UPDATEREFS_BITPOS = 3,

    // Heap is under traversal collection
    TRAVERSAL_BITPOS  = 4,
  };

  enum GCState {
    STABLE        = 0,
    HAS_FORWARDED = 1 << HAS_FORWARDED_BITPOS,
    MARKING       = 1 << MARKING_BITPOS,
    EVACUATION    = 1 << EVACUATION_BITPOS,
    UPDATEREFS    = 1 << UPDATEREFS_BITPOS,
    TRAVERSAL     = 1 << TRAVERSAL_BITPOS,
  };

  enum ShenandoahDegenPoint {
    _degenerated_unset,
    _degenerated_traversal,
    _degenerated_outside_cycle,
    _degenerated_mark,
    _degenerated_evac,
    _degenerated_updaterefs,
    _DEGENERATED_LIMIT,
  };

  static const char* degen_point_to_string(ShenandoahDegenPoint point) {
    switch (point) {
      case _degenerated_unset:
        return "<UNSET>";
      case _degenerated_traversal:
        return "Traversal";
      case _degenerated_outside_cycle:
        return "Outside of Cycle";
      case _degenerated_mark:
        return "Mark";
      case _degenerated_evac:
        return "Evacuation";
      case _degenerated_updaterefs:
        return "Update Refs";
      default:
        ShouldNotReachHere();
        return "ERROR";
    }
  };

private:
  ShenandoahSharedBitmap _gc_state;
  ShenandoahHeapLock _lock;
  ShenandoahCollectorPolicy* _shenandoah_policy;
  ShenandoahHeuristics* _heuristics;
  SoftRefPolicy _soft_ref_policy;
  size_t _bitmap_size;
  size_t _bitmap_regions_per_slice;
  size_t _bitmap_bytes_per_slice;
  MemRegion _heap_region;
  MemRegion _bitmap_region;
  MemRegion _aux_bitmap_region;

  ShenandoahHeapRegion** _regions;
  ShenandoahFreeSet* _free_set;
  ShenandoahCollectionSet* _collection_set;

  ShenandoahRegionIterator _update_refs_iterator;

  ShenandoahConcurrentMark* _scm;
  ShenandoahMarkCompact* _full_gc;
  ShenandoahTraversalGC* _traversal_gc;
  ShenandoahVerifier*  _verifier;
  ShenandoahPacer*  _pacer;

  ShenandoahControlThread* _control_thread;

  ShenandoahMonitoringSupport* _monitoring_support;

  ShenandoahPhaseTimings*      _phase_timings;
  ShenandoahAllocTracker*      _alloc_tracker;

  size_t _num_regions;
  size_t _initial_size;

  uint _max_workers;
  ShenandoahWorkGang* _workers;
  ShenandoahWorkGang* _safepoint_workers;

  volatile size_t _used;
  volatile size_t _committed;

  MarkBitMap _verification_bit_map;
  MarkBitMap _aux_bit_map;

  ShenandoahMarkingContext* _marking_context;

  volatile size_t _bytes_allocated_since_gc_start;

  ShenandoahSharedFlag _progress_last_gc;

  ShenandoahSharedFlag _degenerated_gc_in_progress;
  ShenandoahSharedFlag _full_gc_in_progress;
  ShenandoahSharedFlag _full_gc_move_in_progress;

  ShenandoahSharedFlag _inject_alloc_failure;

  ShenandoahSharedFlag _process_references;
  ShenandoahSharedFlag _unload_classes;

  ShenandoahSharedEnumFlag<CancelState> _cancelled_gc;

  ReferenceProcessor* _ref_processor;

  AlwaysTrueClosure _subject_to_discovery;

  ConcurrentGCTimer* _gc_timer;

  GCMemoryManager _stw_memory_manager;
  GCMemoryManager _cycle_memory_manager;

  MemoryPool* _memory_pool;

  ShenandoahEvacOOMHandler _oom_evac_handler;

public:
  ShenandoahHeap(ShenandoahCollectorPolicy* policy);

  const char* name() const { return "Shenandoah"; }
  virtual HeapWord* allocate_new_tlab(size_t min_size,
                                      size_t requested_size,
                                      size_t* actual_size) /* override */;
  void print_on(outputStream* st) const /* override */;
  void print_extended_on(outputStream *st) const /* override */;

  ShenandoahHeap::Name kind() const  /* override */{
    return CollectedHeap::Shenandoah;
  }

  jint initialize() /* override */;
  void post_initialize() /* override */;
  size_t capacity() const /* override */;
  size_t used() const /* override */;
  size_t committed() const;
  bool is_maximal_no_gc() const shenandoah_not_implemented_return(false);
  size_t max_capacity() const /* override */;
  size_t initial_capacity() const /* override */;
  bool is_in(const void* p) const /* override */;
  bool is_scavengable(oop obj) { return true; }
  virtual oop obj_allocate(Klass* klass, int size, TRAPS);
  virtual oop array_allocate(Klass* klass, int size, int length, bool do_zero, TRAPS);
  virtual oop class_allocate(Klass* klass, int size, TRAPS);
  HeapWord* mem_allocate(size_t size, bool* what) /* override */;
  virtual void fill_with_dummy_object(HeapWord* start, HeapWord* end, bool zap);
  void collect(GCCause::Cause cause) /* override */;
  void do_full_collection(bool clear_all_soft_refs) /* override */;
  AdaptiveSizePolicy* size_policy() shenandoah_not_implemented_return(NULL);
  CollectorPolicy* collector_policy() const /* override */;
  SoftRefPolicy* soft_ref_policy() { return &_soft_ref_policy; }
  void ensure_parsability(bool retire_tlabs) /* override */;
  HeapWord* block_start(const void* addr) const /* override */;
  size_t block_size(const HeapWord* addr) const /* override */;
  bool block_is_obj(const HeapWord* addr) const /* override */;
  jlong millis_since_last_gc() /* override */;
  void prepare_for_verify() /* override */;
  void print_gc_threads_on(outputStream* st) const /* override */;
  void gc_threads_do(ThreadClosure* tcl) const /* override */;
  void print_tracing_info() const /* override */;
  void verify(VerifyOption vo) /* override */;
  bool supports_tlab_allocation() const { return true; }
  size_t tlab_capacity(Thread *thr) const /* override */;
  void object_iterate(ObjectClosure* cl) /* override */;
  void safe_object_iterate(ObjectClosure* cl) /* override */;
  size_t unsafe_max_tlab_alloc(Thread *thread) const /* override */;
  size_t max_tlab_size() const /* override */;
  uint oop_extra_words() /* override */;
  size_t tlab_used(Thread* ignored) const /* override */;
  void stop() /* override */;
  virtual void safepoint_synchronize_begin();
  virtual void safepoint_synchronize_end();

  WorkGang* get_safepoint_workers() { return _safepoint_workers; }

  void register_nmethod(nmethod* nm);
  void unregister_nmethod(nmethod* nm);

  /* override: object pinning support */
  bool supports_object_pinning() const { return true; }
  oop pin_object(JavaThread* thread, oop obj);
  void unpin_object(JavaThread* thread, oop obj);

  virtual void deduplicate_string(oop str);

  static ShenandoahHeap* heap();
  static ShenandoahHeap* heap_no_check();
  static address in_cset_fast_test_addr();
  static address cancelled_gc_addr();
  static address gc_state_addr();

  ShenandoahCollectorPolicy *shenandoahPolicy() const { return _shenandoah_policy; }
  ShenandoahHeuristics*     heuristics()        const { return _heuristics; }
  ShenandoahPhaseTimings*   phase_timings()     const { return _phase_timings; }
  ShenandoahAllocTracker*   alloc_tracker()     const { return _alloc_tracker; }

  void retire_and_reset_gclabs();

  inline ShenandoahHeapRegion* const heap_region_containing(const void* addr) const;
  inline size_t heap_region_index_containing(const void* addr) const;
  inline bool requires_marking(const void* entry) const;

  template <class T>
  inline oop evac_update_with_forwarded(T* p);

  template <class T>
  inline oop maybe_update_with_forwarded(T* p);

  template <class T>
  inline oop maybe_update_with_forwarded_not_null(T* p, oop obj);

  template <class T>
  inline oop update_with_forwarded_not_null(T* p, oop obj);

  void trash_cset_regions();

  void stop_concurrent_marking();

  void evacuate_and_update_roots();

  void update_heap_references(bool concurrent);

  void roots_iterate(OopClosure* cl);

private:
  void set_gc_state_all_threads(char state);
  void set_gc_state_mask(uint mask, bool value);

public:
  void set_concurrent_mark_in_progress(bool in_progress);
  void set_evacuation_in_progress(bool in_progress);
  void set_update_refs_in_progress(bool in_progress);
  void set_degenerated_gc_in_progress(bool in_progress);
  void set_full_gc_in_progress(bool in_progress);
  void set_full_gc_move_in_progress(bool in_progress);
  void set_concurrent_traversal_in_progress(bool in_progress);
  void set_has_forwarded_objects(bool cond);

  void set_process_references(bool pr);
  void set_unload_classes(bool uc);

  inline bool is_stable() const;
  inline bool is_idle() const;
  inline bool is_concurrent_mark_in_progress() const;
  inline bool is_update_refs_in_progress() const;
  inline bool is_evacuation_in_progress() const;
  inline bool is_degenerated_gc_in_progress() const;
  inline bool is_full_gc_in_progress() const;
  inline bool is_full_gc_move_in_progress() const;
  inline bool is_concurrent_traversal_in_progress() const;
  inline bool has_forwarded_objects() const;
  inline bool is_gc_in_progress_mask(uint mask) const;

  char gc_state() const;

  bool process_references() const;
  bool unload_classes() const;

  void force_satb_flush_all_threads();

  template <class T>
  inline bool in_collection_set(T obj) const;

  // Avoid accidentally calling the method above with ShenandoahHeapRegion*, which would be *wrong*.
  inline bool in_collection_set(ShenandoahHeapRegion* r) shenandoah_not_implemented_return(false);

  // Evacuates object src. Returns the evacuated object if this thread
  // succeeded, otherwise rolls back the evacuation and returns the
  // evacuated object by the competing thread.
  inline oop  evacuate_object(oop src, Thread* thread);
  inline bool cancelled_gc() const;
  inline bool check_cancelled_gc_and_yield(bool sts_active = true);
  inline bool try_cancel_gc();
  inline void clear_cancelled_gc();

  inline ShenandoahHeapRegion* const get_region(size_t region_idx) const;

  ShenandoahFreeSet* free_set()             const { return _free_set; }
  ShenandoahCollectionSet* collection_set() const { return _collection_set; }

  void increase_used(size_t bytes);
  void decrease_used(size_t bytes);

  void set_used(size_t bytes);

  void increase_committed(size_t bytes);
  void decrease_committed(size_t bytes);

  void increase_allocated(size_t bytes);

  void notify_mutator_alloc_words(size_t words, bool waste);

  void reset_mark_bitmap();

  inline void mark_complete_marking_context();
  inline void mark_incomplete_marking_context();
  inline ShenandoahMarkingContext* complete_marking_context() const;
  inline ShenandoahMarkingContext* marking_context() const;

  bool commit_bitmap_slice(ShenandoahHeapRegion *r);
  bool uncommit_bitmap_slice(ShenandoahHeapRegion *r);

  // Hint that the bitmap slice is not needed
  bool idle_bitmap_slice(ShenandoahHeapRegion* r);
  void activate_bitmap_slice(ShenandoahHeapRegion* r);

  bool is_bitmap_slice_committed(ShenandoahHeapRegion* r, bool skip_self = false);

  void print_heap_regions_on(outputStream* st) const;

  size_t bytes_allocated_since_gc_start();
  void reset_bytes_allocated_since_gc_start();

  void trash_humongous_region_at(ShenandoahHeapRegion *r);

  virtual GrowableArray<GCMemoryManager*> memory_managers();
  virtual GrowableArray<MemoryPool*> memory_pools();

  ShenandoahMonitoringSupport* monitoring_support();
  ShenandoahConcurrentMark* concurrent_mark() { return _scm; }
  ShenandoahMarkCompact* full_gc() { return _full_gc; }
  ShenandoahTraversalGC* traversal_gc() { return _traversal_gc; }
  ShenandoahVerifier* verifier();
  ShenandoahPacer* pacer() const { return _pacer; }

  ReferenceProcessor* ref_processor() { return _ref_processor;}

  WorkGang* workers() const { return _workers;}

  uint max_workers();

  void assert_gc_workers(uint nworker) PRODUCT_RETURN;

  void heap_region_iterate(ShenandoahHeapRegionClosure* blk, bool skip_cset_regions = false, bool skip_humongous_continuation = false) const;

  // Delete entries for dead interned string and clean up unreferenced symbols
  // in symbol table, possibly in parallel.
  void unload_classes_and_cleanup_tables(bool full_gc);

  inline size_t num_regions() const { return _num_regions; }

  // Call before starting evacuation.
  void enter_evacuation();
  // Call after finished with evacuation.
  void leave_evacuation();

public:
  template<class T>
  inline void marked_object_iterate(ShenandoahHeapRegion* region, T* cl);

  template<class T>
  inline void marked_object_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* limit);

  template<class T>
  inline void marked_object_oop_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* limit);

  GCTimer* gc_timer() const;

  void cancel_gc(GCCause::Cause cause);

  ShenandoahHeapLock* lock() { return &_lock; }
  void assert_heaplock_owned_by_current_thread() PRODUCT_RETURN;
  void assert_heaplock_not_owned_by_current_thread() PRODUCT_RETURN;
  void assert_heaplock_or_safepoint() PRODUCT_RETURN;

private:
  HeapWord* tlab_post_allocation_setup(HeapWord* obj);

  void initialize_heuristics();
  virtual void initialize_serviceability();

  HeapWord* allocate_memory_under_lock(ShenandoahAllocRequest& request, bool& in_new_region);
  HeapWord* allocate_memory(ShenandoahAllocRequest& request);
  // Shenandoah functionality.
  inline HeapWord* allocate_from_gclab(Thread* thread, size_t size);
  HeapWord* allocate_from_gclab_slow(Thread* thread, size_t size);
  HeapWord* allocate_new_gclab(size_t min_size, size_t word_size, size_t* actual_size);

  ShenandoahControlThread* control_thread() { return _control_thread; }

public:
  inline oop atomic_compare_exchange_oop(oop n, narrowOop* addr, oop c);
  inline oop atomic_compare_exchange_oop(oop n, oop* addr, oop c);

private:
  void ref_processing_init();

public:
  void make_parsable(bool retire_tlabs);
  void accumulate_statistics_tlabs();
  void resize_tlabs();

  GCMemoryManager* cycle_memory_manager() { return &_cycle_memory_manager; }
  GCMemoryManager* stw_memory_manager()   { return &_stw_memory_manager; }

  GCTracer* tracer();
public:
  // Entry points to STW GC operations, these cause a related safepoint, that then
  // call the entry method below
  void vmop_entry_init_mark();
  void vmop_entry_final_mark();
  void vmop_entry_final_evac();
  void vmop_entry_init_updaterefs();
  void vmop_entry_final_updaterefs();
  void vmop_entry_init_traversal();
  void vmop_entry_final_traversal();
  void vmop_entry_full(GCCause::Cause cause);
  void vmop_degenerated(ShenandoahDegenPoint point);

  // Entry methods to normally STW GC operations. These set up logging, monitoring
  // and workers for net VM operation
  void entry_init_mark();
  void entry_final_mark();
  void entry_final_evac();
  void entry_init_updaterefs();
  void entry_final_updaterefs();
  void entry_init_traversal();
  void entry_final_traversal();
  void entry_full(GCCause::Cause cause);
  void entry_degenerated(int point);

  // Entry methods to normally concurrent GC operations. These set up logging, monitoring
  // for concurrent operation.
  void entry_reset();
  void entry_mark();
  void entry_preclean();
  void entry_cleanup();
  void entry_evac();
  void entry_updaterefs();
  void entry_traversal();
  void entry_uncommit(double shrink_before);

private:
  // Actual work for the phases
  void op_init_mark();
  void op_final_mark();
  void op_final_evac();
  void op_init_updaterefs();
  void op_final_updaterefs();
  void op_init_traversal();
  void op_final_traversal();
  void op_full(GCCause::Cause cause);
  void op_degenerated(ShenandoahDegenPoint point);
  void op_degenerated_fail();
  void op_degenerated_futile();

  void op_reset();
  void op_mark();
  void op_preclean();
  void op_cleanup();
  void op_evac();
  void op_updaterefs();
  void op_traversal();
  void op_uncommit(double shrink_before);

  // Messages for GC trace event, they have to be immortal for
  // passing around the logging/tracing systems
  const char* init_mark_event_message() const;
  const char* final_mark_event_message() const;
  const char* conc_mark_event_message() const;
  const char* degen_event_message(ShenandoahDegenPoint point) const;

private:
  void try_inject_alloc_failure();
  bool should_inject_alloc_failure();
};

class ShenandoahIsAliveSelector : public StackObj {
private:
  ShenandoahIsAliveClosure _alive_cl;
  ShenandoahForwardedIsAliveClosure _fwd_alive_cl;
public:
  BoolObjectClosure* is_alive_closure();
};


#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_HPP
