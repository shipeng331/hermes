/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_GCBASE_H
#define HERMES_VM_GCBASE_H

#include "hermes/Platform/Logging.h"
#include "hermes/Public/CrashManager.h"
#include "hermes/Public/GCConfig.h"
#include "hermes/Public/GCTripwireContext.h"
#include "hermes/Public/MemoryEventTracker.h"
#include "hermes/Support/CheckedMalloc.h"
#include "hermes/Support/OSCompat.h"
#include "hermes/Support/StatsAccumulator.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/CellKind.h"
#include "hermes/VM/GCDecl.h"
#include "hermes/VM/GCPointer.h"
#include "hermes/VM/HasFinalizer.h"
#include "hermes/VM/HeapAlign.h"
#include "hermes/VM/HeapSnapshot.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/SerializeHeader.h"
#include "hermes/VM/SlotAcceptor.h"
#include "hermes/VM/SlotVisitor.h"
#include "hermes/VM/StorageProvider.h"
#include "hermes/VM/StringRefUtils.h"
#include "hermes/VM/VTable.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/ErrorHandling.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <system_error>
#include <vector>

namespace hermes {
namespace vm {

class GCCell;
#ifdef HERMESVM_SERIALIZE
class Serializer;
class Deserializer;
#endif

// A specific GC class extend GCBase, and override its virtual functions.
// In addition, it must implement the following methods:

/// Allocate a new cell of the specified type \p vt and size \p size. If
/// necessary perform a GC cycle, which may potentially move allocated
/// objects. \p fixedSize should indicate whether the allocation is
/// for a fixed-size, small object; some GCs may allow optimizations
/// on this bases.  \p hasFinalizer must be \p HasFinalizer::Yes if
/// cells of the given type require a finalizer to be called.
///
///   template <bool fixedSize = true,
///             HasFinalizer hasFinalizer = HasFinalizer::No>
///   CallResult<GCCell *> alloc(const VTable *vt, uint32_t size);
///
/// Like the above, but if the GC makes a distinction between short- and
/// long-lived objects, allocates an object that is expected to be
/// long-lived.  Does not allow specification of fixed-sizeness.
///
///   template <HasFinalizer hasFinalizer = HasFinalizer::No>
///   CallResult<GCCell *> allocLongLived(const VTable *vt, uint32_t size);
///
/// In some GCs, objects can have associated memory allocated outside the heap,
/// and this memory can influence GC initiation and heap sizing heuristics.
/// This method tests whether an external memory allocation is too large (e.g.,
/// larger than the max size of the heap):
///
///   bool canAllocExternalMemory(uint32_t size);
///
/// These APIs inform the GC of this external memory.
///
///   void creditExternalMemory(GCCell *alloc, uint32_t size);
///   void debitExternalMemory(GCCell *alloc, uint32_t size);
///
/// Force a garbage collection cycle.
///   void collect();
///
/// The maximum size of any one allocation allowable by the GC in any state.
///   static constexpr uint32_t maxAllocationSize();
///
/// Mark a pointer to a GCCell.
///   template <class T> void mark(T *&ptr);
///
/// Returns true if \p p points into the heap.
///   bool contains(const void *p) const;
///
/// Returns true iff \p cell is the most-recently allocated finalizable object.
///   bool isMostRecentFinalizableObj(const GCCell* cell) const;
///
/// Return the lower bound of the heap's virtual address range (inclusive).
///   char *lowLim() const;
///
/// Return the upper bound of the heap's virtual address range (exclusive).
///   char *hiLim() const;
///
/// In the "mark" functions below, Name is one of const char*, int,
/// unsigned, or const StringPrimitive*:
///
/// Mark a HermesValue which may or may not be a pointer.
///   void mark(HermesValue &hv, Name name);
///   void mark(HermesValue &hv);
///
/// Mark a T* location. This location must be outside the heap.
///   void mark(T *&ptr, Name name);
///   void mark(T *&ptr);
///
/// Mark a GCPointer<T>, which must be within the heap.
///   void mark(GCPointer<T> &ptr, Name name);
///
/// \return true if a GC cycle is currently in progress.
/// \post If false, all objects in the heap have a valid VTable.
///   bool inGC() const;
///
/// Various forms of write barriers: these can have empty implementations
/// for GCs that don't require them:
///
///   The given value is being written at the given loc (required to
///   be in the heap).  If value is a pointer, execute a write barrier.
///      void writeBarrier(void *loc, HermesValue value);
///
///   The given pointer value is being written at the given loc (required to
///   be in the heap).  The value is may be null.  Execute a write barrier.
///      void writeBarrier(void *loc, void *value);
///
///   We copied HermesValues into the given region.  Note that \p numHVs is
///   the number of HermesValues in the the range, not the char length.
///   Do any necessary barriers.
///      void writeBarrierRange(HermesValue* start, uint32_t numHVs);
///
///   We filled numHVs slots starting at start with the given value.
///   Do any necessary barriers.
///      void writeBarrierRangeFill(HermesValue* start, uint32_t numHVs,
///                                 HermesValue value);
///
///   In debug builds: is a write barrier necessary for a write of the given
///   GC pointer \p value to the given \p loc?
///      bool needsWriteBarrier(void *loc, void *value);
///
/// This is intended to be called only from within object or root mark
/// functions and indicates whether the \c mark() operation, called within
/// the current GC phase, is the first such call that guarantees that the
/// location passed to mark will contain the final, correct, pointer value
/// after the mark call.
///   bool isUpdatingPointers() const;
///
/// It must also have the inner type:
///   class Size;
/// Which provides at least these functions publicly:
///   Constructor from either a GCConfig or the min and max heap size.
///     explicit Size(const GCConfig &conf);
///     Size(gcheapsize_t min, gcheapsize_t max);
///   Return the minimum amount of bytes holdable by this heap.
///     gcheapsize_t min() const;
///   Return the maximum amount of bytes holdable by this heap.
///     gcheapsize_t max() const;
///   Return the total amount of bytes of storage this GC will require.
///   This will be a multiple of AlignedStorage::size().
///     gcheapsize_t storageFootprint() const;
///
class GCBase {
 public:
  /// An interface enabling the garbage collector to mark roots and free
  /// symbols.
  struct GCCallbacks {
    /// Virtual destructor to avoid warnings.
    virtual ~GCCallbacks() = 0;

    /// Callback that will be invoked by the GC to mark all roots in the
    /// beginning of every GC by calling "gc->mark()".
    /// The \p markLongLived argument indicates whether root data structures
    /// that contain only references to long-lived objects (allocated
    /// via allocLongLived) are required to be scanned.  A generational
    /// collector, for example, might take advantage of this.
    virtual void markRoots(
        RootAcceptor &acceptor,
        bool markLongLived = true) = 0;

    /// Callback that will be invoked by the GC to mark all weak roots in the
    /// beginning of every GC.
    virtual void markWeakRoots(WeakRootAcceptor &weakAcceptor) = 0;

    /// \return one higher than the largest symbol in the identifier table. This
    /// enables the GC to size its internal structures for symbol marking.
    /// Optionally invoked at the beginning of a garbage collection.
    virtual unsigned getSymbolsEnd() const = 0;

    /// Free all symbols which are not marked as \c true in \p markedSymbols.
    /// Optionally invoked at the end of a garbage collection.
    virtual void freeSymbols(const std::vector<bool> &markedSymbols) = 0;

    /// Prints any statistics maintained in the Runtime about GC to \p
    /// os.  At present, this means the breakdown of markRoots time by
    /// "phase" within markRoots.
    virtual void printRuntimeGCStats(llvm::raw_ostream &os) const = 0;

    /// \returns the approximate usage of memory external to the GC such as
    /// malloc by the roots of the object graph.
    virtual size_t mallocSize() const = 0;

    /// Visits every entry in the identifier table and calls acceptor with
    /// the entry as argument. This is intended to be used only for Snapshots,
    /// as it is slow. The function passed as acceptor shouldn't perform any
    /// heap operations.
    virtual void visitIdentifiers(
        const std::function<void(UTF16Ref, uint32_t id)> &acceptor) = 0;

    /// Convert the given symbol into its UTF-8 string representation.
    /// \post The implementation of this function must not perform any GC
    ///   operations, such as allocations, mutating values in the heap, or
    ///   making handles.
    virtual std::string convertSymbolToUTF8(SymbolID id) = 0;

    /// Returns the current stack as a string. This function will not cause
    /// any allocs in the GC.
    virtual std::string getCallStackNoAlloc() = 0;
  };

  /// Struct that keeps a reference to a GC.  Useful, for example, as a base
  /// class of Acceptors that need access to the GC.
  struct GCRef {
    GC &gc;
    GCRef(GC &gc) : gc(gc) {}
  };

  /// Stats for collections. Time unit, where applicable, is seconds.
  struct CumulativeHeapStats {
    unsigned numCollections{0};

    /// Summary statistics for GC wall times.
    StatsAccumulator<double> gcWallTime;

    /// Summary statistics for GC CPU times.
    StatsAccumulator<double> gcCPUTime;

    gcheapsize_t finalHeapSize{0};

    /// Bytes allocated just before a collection.
    StatsAccumulator<gcheapsize_t, uint64_t> usedBefore;

    /// Bytes alive after a collection.
    StatsAccumulator<gcheapsize_t, uint64_t> usedAfter;
  };

  struct HeapInfo {
    /// Number of garbage collections (of any kind) since creation.
    unsigned numCollections{0};
    /// Total (cumulative) bytes allocated within the JS heap since creation.
    uint64_t totalAllocatedBytes{0};
    /// Number of currently allocated bytes within the JS heap. Some may be
    /// in unreachable objects (unless a full collection just occurred).
    gcheapsize_t allocatedBytes{0};
    /// Current capacity of the JS heap, in bytes.
    gcheapsize_t heapSize{0};
    /// Estimate of amount of current malloc space used by the runtime and any
    /// auxiliary allocations owned by heap objects. (Calculated by querying
    /// each finalizable object to report its malloc usage.)
    unsigned mallocSizeEstimate{0};
    /// The total amount of Virtual Address space (VA) that the GC is using.
    uint64_t va{0};
    /// Stats for full collections (zeroes if non-generational GC).
    CumulativeHeapStats fullStats;
    /// Stats for collections in the young generation (zeroes if
    /// non-generational GC).
    CumulativeHeapStats youngGenStats;
  };

#ifndef NDEBUG
  struct DebugHeapInfo {
    /// Number of currently allocated objects present in the heap. Some may be
    /// unreachable.
    unsigned numAllocatedObjects{0};
    /// Number of reachable objects in the last collection.
    unsigned numReachableObjects{0};
    /// Number of collected objects in the last collection.
    unsigned numCollectedObjects{0};
    /// Number of finalized objects in the last collection.
    unsigned numFinalizedObjects{0};
    /// Number of marked symbols.
    unsigned numMarkedSymbols{0};
    /// Number of hidden classes alive after the last collection.
    unsigned numHiddenClasses{0};
    /// Number of "leaf" hidden classes alive after the last collection.
    unsigned numLeafHiddenClasses{0};

    // Assert any invariants that should hold among the fields of the
    // DebugHeapInfo.
    void assertInvariants() const;
  };
#endif

  class IDTracker final {
   public:
    /// These are IDs that are reserved for special objects.
    enum class ReservedObjectID : HeapSnapshot::NodeID {
      // For any object where an ID cannot be found.
      NoID = 0,
      // The ID for the super root object.
      Root,
#define ROOT_SECTION(name) name,
#include "hermes/VM/RootSections.def"
      FirstNonReservedID,
    };

    explicit IDTracker() = default;

    /// Return true if IDs are being tracked.
    inline bool isTrackingIDs() const;

    /// Get the unique object id of the given object.
    /// If one does not yet exist, start tracking it.
    inline HeapSnapshot::NodeID getObjectID(const void *cell);
    /// Get the unique object id of the given native memory (non-JS-heap).
    /// If one does not yet exist, start tracking it.
    inline HeapSnapshot::NodeID getNativeID(const void *mem);

    /// Tell the tracker that an object has moved locations.
    /// This must be called in a safe order, if A moves to B, and C moves to A,
    /// the first move must be recorded before the second.
    inline void moveObject(const void *oldLocation, const void *newLocation);

    /// Remove the object from being tracked. This should be done to keep the
    /// tracking working set small.
    inline void untrackObject(const void *cell);

    /// Remove the native memory from being tracked. This should be done to keep
    /// the tracking working set small. It is also required to be done when
    /// malloc'ed memory is freed, since addresses can be re-used by future
    /// allocations.
    inline void untrackNative(const void *mem);

    /// Execute a callback on each pair of pointer and ID.
    /// \param callback A function whose signature should be
    ///   void(const void *, HeapSnapshot::NodeID).
    template <typename F>
    inline void forEachID(F callback);

#ifdef HERMESVM_SERIALIZE
    /// Serialize this IDTracker to the output stream.
    void serialize(Serializer &s) const;

    /// Deserialize IDTracker from the MemoryBuffer.
    void deserialize(Deserializer &d);
#endif

   private:
    /// Get the next unique object ID for a newly created object.
    inline HeapSnapshot::NodeID nextObjectID();
    /// Get the next unique native ID for a chunk of native memory.
    inline HeapSnapshot::NodeID nextNativeID();

    /// JS heap nodes are represented by even-numbered IDs, while native nodes
    /// are represented with odd-numbered IDs. This is not a guarantee of the
    /// system, but an implementation detail.
    static constexpr HeapSnapshot::NodeID kIDStep = 2;

    /// The next available ID to assign to an object. Object IDs are not
    /// recycled so that snapshots don't confuse two objects with each other.
    /// NOTE: Need to ensure that this starts on an even number, so check if
    /// the first non-reserved ID is even, if not add one.
    uint64_t nextID_{
        static_cast<uint64_t>(ReservedObjectID::FirstNonReservedID) +
        static_cast<uint64_t>(ReservedObjectID::FirstNonReservedID) % 2};
    /// The next available native ID to assign to a chunk of native memory.
    HeapSnapshot::NodeID nextNativeID_{nextID_ + 1};

    /// Map of object pointers to IDs. Only populated once the first heap
    /// snapshot is requested, or the first time the memory profiler is turned
    /// on.
    /// NOTE: The same map is used for both JS heap and native heap IDs.
    llvm::DenseMap<const void *, HeapSnapshot::NodeID> objectIDMap_;
  };

#ifndef NDEBUG
  /// Whether the last allocation was fixed size.  For long-lived
  /// allocations, we do not declare whether they are fixed size;
  /// Unknown is used in that case.
  enum class FixedSizeValue { Yes, No, Unknown };
#endif

  GCBase(
      MetadataTable metaTable,
      GCCallbacks *gcCallbacks,
      PointerBase *pointerBase,
      const GCConfig &gcConfig,
      std::shared_ptr<CrashManager> crashMgr,
      StorageProvider *provider);

  virtual ~GCBase() {}

  /// \return true if we should run handle sanitization and the coin flip with
  /// probability sanitizeRate_ has passed.
#ifdef HERMESVM_SANITIZE_HANDLES
  bool shouldSanitizeHandles();
#else
  static constexpr bool shouldSanitizeHandles() {
    return false;
  }
#endif

  /// \return true if the "target space" for allocations should be randomized
  /// (for GCs where that concept makes sense).
  bool shouldRandomizeAllocSpace() const {
    return randomizeAllocSpace_;
  }

#ifndef NDEBUG
  /// Returns whether the most-recently allocated object was specified as
  /// fixed-size in the the allocation.  (FixedSizeValue is a trinary type,
  /// defined above: Yes, No, or Unknown.)
  virtual FixedSizeValue lastAllocationWasFixedSize() const {
    // The default implementation returns Unknown.  This makes sense for GC
    // implementations that don't care about FixedSize.
    return FixedSizeValue::Unknown;
  }
#endif

  /// Name to indentify this heap in logs.
  const std::string &getName() const {
    return name_;
  }

  /// \return the base of pointers in the heap.
  /// NOTE: This normally should not be needed, Runtime provides it.
  /// However in some scenarios there is only a GC available, not a
  /// Runtime. In those cases use this function.
  PointerBase *getPointerBase() const {
    return pointerBase_;
  }

  GCCallbacks *getCallbacks() const {
    return gcCallbacks_;
  }

  /// Forwards to the GC callback \p convertSymbolToUTF8, see documentation
  /// for that function.
  std::string convertSymbolToUTF8(SymbolID id) {
    return gcCallbacks_->convertSymbolToUTF8(id);
  }

  /// Called by the Runtime to inform the GC that it is about to execute JS for
  /// the first time.
  void runtimeWillExecute();

  /// Inform the GC that TTI has been reached.  (In case, for example,
  /// behavior should change at that point.  Default behavior is to do
  /// nothing.)
  void ttiReached() {}

  /// Do anything necessary to record the current number of allocated
  /// objects in numAllocatedObjects_.  Default is to do nothing.
  virtual void recordNumAllocatedObjects() {}

  /// Print any and all collected statistics to the give output stream, \p os.
  void printAllCollectedStats(llvm::raw_ostream &os);

  /// Total number of collections of any kind.
  unsigned getNumGCs() const {
    return cumStats_.numCollections;
  }

  /// Total wall time in seconds of all pauses due to collections so far.
  double getGCTime() const {
    return cumStats_.gcWallTime.sum();
  }

  /// Total CPU time in seconds of all pauses due to collections so far.
  double getGCCPUTime() const {
    return cumStats_.gcCPUTime.sum();
  }

  /// Cumulative stats over time so far.
  virtual size_t getPeakAllocatedBytes() const {
    return cumStats_.usedBefore.max();
  }
  virtual size_t getPeakLiveAfterGC() const {
    return cumStats_.usedAfter.max();
  }

  /// Populate \p info with information about the heap.
  virtual void getHeapInfo(HeapInfo &info);
  /// Same as \c getHeapInfo, and it adds the amount of malloc memory in use.
  virtual void getHeapInfoWithMallocSize(HeapInfo &info) = 0;

  /// Populate \p info with crash manager information about the heap
  virtual void getCrashManagerHeapInfo(CrashManager::HeapInformation &info) = 0;

#ifndef NDEBUG
  /// Populate \p info with more detailed information about the heap that is
  /// too expensive to know during production builds.
  virtual void getDebugHeapInfo(DebugHeapInfo &info);
#endif

  /// Dump detailed heap contents to the given output stream, \p os.
  virtual void dump(llvm::raw_ostream &os, bool verbose = false);

  /// Do any logging of info about the heap that is useful, then dies with a
  /// fatal out-of-memory error.
  LLVM_ATTRIBUTE_NORETURN void oom(std::error_code reason);

  /// Creates a snapshot of the heap and writes it to the given \p fileName.
  /// \p compact whether to write a compact version or a pretty human-readable
  ///   version.
  /// \return true on success, false on failure.
  bool createSnapshotToFile(const std::string &fileName);
  /// Creates a snapshot of the heap, which includes information about what
  /// objects exist, their sizes, and what they point to.
  virtual void createSnapshot(llvm::raw_ostream &os) = 0;

#ifdef HERMESVM_SERIALIZE
  /// Serialize WeakRefs.
  virtual void serializeWeakRefs(Serializer &s) = 0;

  /// Deserialize WeakRefs
  virtual void deserializeWeakRefs(Deserializer &d) = 0;

  /// Serialze all heap objects to a stream.
  virtual void serializeHeap(Serializer &s) = 0;

  /// Deserialize heap objects.
  virtual void deserializeHeap(Deserializer &d) = 0;

  /// Signal GC we are deserializing. Switch to oldgen if necessary for GenGC
  /// Otherwise do nothing.
  virtual void deserializeStart() = 0;

  /// Signal GC we are serializing. Switch to youngGen if necessary
  virtual void deserializeEnd() = 0;
#endif

  /// Default implementations for the external memory credit/debit APIs: do
  /// nothing.
  void creditExternalMemory(GCCell *alloc, uint32_t size) {}
  void debitExternalMemory(GCCell *alloc, uint32_t size) {}

  /// Default implementations for write barriers: do nothing.
  inline void writeBarrier(void *loc, HermesValue value) {}
  inline void writeBarrier(void *loc, void *value) {}
  inline void writeBarrierRange(HermesValue *start, uint32_t numHVs) {}
  inline void writeBarrierRangeFill(
      HermesValue *start,
      uint32_t numHVs,
      HermesValue value) {}
#ifndef NDEBUG
  bool needsWriteBarrier(void *loc, void *value) {
    return false;
  }
#endif

  /// @name Marking APIs
  /// @{

  /// Marks a cell by its metadata.
  /// \p cell The heap object to mark.
  /// \p gc The GC that owns the cell.
  /// \p acceptor The action to perform on each slot in the cell.
  template <typename Acceptor>
  static inline void markCell(GCCell *cell, GC *gc, Acceptor &acceptor);

  /// Same as the normal \c markCell, but for cells that don't have a valid
  /// vtable pointer.
  template <typename Acceptor>
  static inline void
  markCell(GCCell *cell, const VTable *vt, GC *gc, Acceptor &acceptor);

  /// Same as the normal \c markCell, but takes a visitor instead.
  template <typename Acceptor>
  static inline void markCell(
      SlotVisitor<Acceptor> &visitor,
      GCCell *cell,
      const VTable *vt,
      GC *gc);

  /// Marks a cell by its metadata, but only for the slots that point between
  /// [begin, end).
  template <typename Acceptor>
  static inline void markCellWithinRange(
      SlotVisitor<Acceptor> &visitor,
      GCCell *cell,
      const VTable *vt,
      GC *gc,
      const char *begin,
      const char *end);

  /// Marks a cell by its metadata, and outputs the names of the slots.
  /// Meant to be used by heap snapshots.
  template <typename Acceptor>
  static inline void markCellWithNames(
      SlotVisitorWithNames<Acceptor> &visitor,
      GCCell *cell,
      GC *gc);

  /// @}

  bool inGC() const {
    return inGC_;
  }

  IDTracker &getIDTracker() {
    return idTracker_;
  }

  inline HeapSnapshot::NodeID getObjectID(const void *cell);
  inline HeapSnapshot::NodeID getObjectID(const GCPointerBase &cell);
  inline HeapSnapshot::NodeID getNativeID(const void *mem);

#ifndef NDEBUG
  /// \return The next debug allocation ID for embedding directly into a GCCell.
  /// NOTE: This is not the same ID as is used for stable lifetime tracking, use
  /// \p getObjectID for that.
  inline uint64_t nextObjectID();
#endif

  /// Get the instance of the memory event tracker. If memory
  /// profiling is not enabled this should return nullptr.
  inline MemoryEventTracker *memEventTracker() {
#ifdef HERMESVM_MEMORY_PROFILER
    return memEventTracker_.get();
#else
    return nullptr;
#endif
  }

  using TimePoint = std::chrono::steady_clock::time_point;
  /// Return the difference between the two time points (end - start)
  /// as a double representing the number of seconds in the duration.
  static double clockDiffSeconds(TimePoint start, TimePoint end);

  /// Return the difference between the two durations (end - start) given in
  /// microseconds as a double representing the number of seconds in the
  /// difference.
  static double clockDiffSeconds(
      std::chrono::microseconds start,
      std::chrono::microseconds end);

// Mangling scheme used by MSVC encode public/private into the name.
// As a result, vanilla "ifdef public" trick leads to link errors.
#if defined(UNIT_TEST) || defined(_MSC_VER)
 public:
#else
 protected:
#endif

  /// dataSize is the live data in bytes, now is the current time point. The
  /// function checks these parameters against the limits set at initialisation.
  /// If the conditions are met, the tripwire is triggered and tripwireCallback_
  /// is called.
  void checkTripwire(
      size_t dataSize,
      std::chrono::time_point<std::chrono::steady_clock> now);

  // Visibility here is public for unit_tests and protected otherwise

 protected:
  /// An RAII-style object used to denote regions when a GC cycle is considered
  /// active.
  class GCCycle final {
   public:
    GCCycle(GCBase *gc);
    ~GCCycle();

   private:
    GCBase *const gc_;
  };

  /// Returns the number of bytes allocated allocated since the last GC.
  /// TODO: Implement this for heaps other than GenGC
  /// (at which point this can become an abstract function).
  virtual gcheapsize_t bytesAllocatedSinceLastGC() const {
    return 0;
  }

  /// Convenience method to invoke the mark roots function provided at
  /// initialization, using the context provided then (on this heap).
  /// The \p markLongLived argument indicates whether root data structures
  /// containing only pointers to objects allocated via allocLongLived
  /// are required to be marked.  In this collector, such objects will
  /// be allocated in the old gen, and references to them need not be
  /// marked during young-gen collection.
  void markRoots(RootAcceptor &acceptor, bool markLongLived) {
    gcCallbacks_->markRoots(acceptor, markLongLived);
  }

  /// Convenience method to invoke the mark weak roots function provided at
  /// initialization, using the context provided then (on this heap).
  void markWeakRoots(WeakRootAcceptor &acceptor) {
    gcCallbacks_->markWeakRoots(acceptor);
  }

  /// Print the cumulative statistics.
  /// \p os The output stream to print the stats to.
  /// \p trailingComma true if the end of the JSON string should have a trailing
  /// comma (anticipating more objects added after it).
  virtual void printStats(llvm::raw_ostream &os, bool trailingComma);

  /// Record statistics from a single GC, which took \p wallTime seconds wall
  /// time and \p cpuTime seconds CPU time to run the gc and left the heap size
  /// at the given \p finalHeapSize, in the given cumulative stats struct.
  void recordGCStats(
      double wallTime,
      double cpuTime,
      gcheapsize_t finalHeapSize,
      gcheapsize_t usedBefore,
      gcheapsize_t usedAfter,
      CumulativeHeapStats *stats);

  /// Record statistics from a single GC, which took \p wallTime seconds wall
  /// time and \p cpuTime seconds CPU time to run the gc and left the heap size
  /// at the given \p finalHeapSize, in the overall cumulative stats struct.
  void recordGCStats(
      double wallTime,
      double cpuTime,
      gcheapsize_t finalHeapSize,
      gcheapsize_t usedBefore,
      gcheapsize_t usedAfter);

  /// Do any additional GC-specific logging that is useful before dying with
  /// out-of-memory.
  virtual void oomDetail(std::error_code reason);

#ifndef NDEBUG
  // Returns true iff \p finalizables is non-empty, and \p cell is the
  // last element in the vector.  Useful in code checking that
  // objects with finalizers are allocated correctly.
  static bool isMostRecentCellInFinalizerVector(
      const std::vector<GCCell *> &finalizables,
      const GCCell *cell);
#endif

  /// If a cell has any weak references to mark, and the acceptor supports
  /// marking them, mark those weak references.
  template <typename Acceptor>
  static void
  markWeakRefsIfNecessary(GCCell *cell, const VTable *vt, Acceptor &acceptor);

  /// Overload of \p markWeakRefsIfNecessary for acceptors that support marking
  /// weak references.
  /// Don't call this directly, use the three-argument variant instead.
  template <typename Acceptor>
  static void markWeakRefsIfNecessary(
      GCCell *cell,
      const VTable *vt,
      Acceptor &acceptor,
      std::true_type) {
    // In C++17, we could implement this via "constexpr if" rather than
    // overloads with std::true_type.
    // Once C++17 is available, switch to using that.
    vt->markWeakIfExists(cell, acceptor);
  }

  /// Overload of \p markWeakRefsIfNecessary for acceptors that do not support
  /// marking weak references.
  /// Don't call this directly, use the three-argument variant instead.
  template <typename Acceptor>
  static void markWeakRefsIfNecessary(
      GCCell *,
      const VTable *,
      Acceptor &,
      std::false_type) {}

  /// Number of finalized objects in the last collection.
  unsigned numFinalizedObjects_{0};

  /// The total number of bytes allocated in the execution.
  uint64_t totalAllocatedBytes_{0};

/// These fields are not available in optimized builds.
#ifndef NDEBUG
  /// Number of currently allocated objects present in the heap before the start
  /// of the last collection.  Some may be unreachable.
  unsigned numAllocatedObjects_{0};
  /// Number of reachable objects in the last collection.  (More properly, this
  /// is the number not known to be unreachable: if a GC does not consider
  /// determine the reachability of some subset of objects, for example, an old
  /// generation in a generational collection, those objects should be included
  /// in this count.)
  unsigned numReachableObjects_{0};
  /// Number of collected objects in the last collection.  Equal to
  /// numAllocatedObjects_ (at the start of the last collection),
  /// minus numReachableObjects_ found in that collection.
  unsigned numCollectedObjects_{0};
  /// Number of marked symbols.
  unsigned numMarkedSymbols_{0};
  /// Number of hidden classes alive after the last collection.
  unsigned numHiddenClasses_{0};
  /// Number of "leaf" hidden classes alive after the last collection.
  unsigned numLeafHiddenClasses_{0};

  /// Associate a semi-unique (until it overflows) id with every allocation
  /// for easier identification when debugging.
  uint64_t debugAllocationCounter_{0};
#endif

  /// The table to retrieve metadata about each cell kind.
  const MetadataTable metaTable_;

  /// User-supplied callbacks invoked by the GC to query information or perform
  /// tasks.
  GCCallbacks *const gcCallbacks_;

  /// Base of all pointers in compressed pointers implementation.
  PointerBase *const pointerBase_;

  /// A place to log crash data if a crash is about to occur.
  std::shared_ptr<CrashManager> crashMgr_;

  /// Whether to output GC statistics at the end of execution.
  bool recordGcStats_{false};

  /// Whether or not a GC cycle is currently occurring.
  bool inGC_;

  /// Time at which execution of the Hermes VM began.
  std::chrono::time_point<std::chrono::steady_clock> execStartTime_;
  std::chrono::microseconds execStartCPUTime_;
  /// Number of context switches before execution of the Hermes VM began.
  long startNumVoluntaryContextSwitches_{0};
  long startNumInvoluntaryContextSwitches_{0};
  // The cumulative GC stats.
  CumulativeHeapStats cumStats_;

  /// Name to indentify this heap in logs.
  std::string name_;

  /// Tracks what objects need a stable identity for features such as heap
  /// snapshots and the memory profiler.
  IDTracker idTracker_;

#ifndef NDEBUG
  /// The number of reasons why no allocation is allowed in this heap right now.
  uint32_t noAllocLevel_{0};

  friend class NoAllocScope;
#endif

 private:
#ifdef HERMESVM_MEMORY_PROFILER
  /// Memory event tracker for the memory profiler
  std::shared_ptr<MemoryEventTracker> memEventTracker_;
#endif

  /// Callback called if it's not null when the Live Data Tripwire is triggered
  std::function<void(GCTripwireContext &)> tripwireCallback_;

  /// Maximum size limit before the heap size tripwire will trigger
  gcheapsize_t tripwireLimit_;

  /// Time in hours before the tripwire can trigger again after it is
  /// triggered
  std::chrono::hours tripwireCooldown_;

  /// Time when the tripwire can be activated again
  std::chrono::time_point<std::chrono::steady_clock> nextTripwireMinTime_{
      std::chrono::steady_clock::now()};

  /// Variable that saves whether the callback for the live data tripwire is
  /// already running
  bool liveDataTripwireCallbackRunning_{false};

#ifdef HERMESVM_SANITIZE_HANDLES
  /// Whether to keep moving the heap around to detect unsanitary GC handles.
  double sanitizeRate_{1.0};

  /// PRNG for sanitizing at a less than 1.0 rate.
  std::minstd_rand randomEngine_;
#else
  /// Sanitize handles is completely disabled (and ignored at runtime) without
  /// a special build mode.
  static constexpr double sanitizeRate_{0.0};
#endif

/// Whether to randomize the "target space" for allocations, for GC's in which
/// this concept makes sense. Only available in debug builds.
#ifndef NDEBUG
  bool randomizeAllocSpace_{false};
#else
  static const bool randomizeAllocSpace_{false};
#endif
};

#ifdef HERMESVM_EXCEPTION_ON_OOM
/// A std::runtime_error class for out-of-memory.
class JSOutOfMemoryError : public std::runtime_error {
 public:
  JSOutOfMemoryError(const std::string &what_arg)
      : std::runtime_error(what_arg) {}
  JSOutOfMemoryError(const char *what_arg) : std::runtime_error(what_arg) {}
};
#endif

// Utilities for formatting time durations and memory sizes.

/// An object that, written to an ostream, formats the given # of
/// secs in appropriate units (down to microseconds).
struct DurationFormatObj {
  double secs;
};
llvm::raw_ostream &operator<<(
    llvm::raw_ostream &os,
    const DurationFormatObj &dfo);
inline DurationFormatObj formatSecs(double secs) {
  return {secs};
}

/// An object that, written to an ostream, formats the given # of
/// bytes in appropriate units (bytes to GiB).
struct SizeFormatObj {
  gcheapsize_t bytes;
};
llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const SizeFormatObj &sfo);
inline SizeFormatObj formatSize(gcheapsize_t size) {
  return {size};
}

/// This is a single slot in the weak reference table. It contains a pointer to
/// a GC managed object. The GC will make sure it is updated when the object is
/// moved; if the object is garbage-collected, the pointer will be cleared.
class WeakRefSlot {
 public:
  /// State of this slot for the purpose of reusing slots.
  enum State {
    Unmarked = 0, /// Unknown whether this slot is in use by the mutator.
    Marked, /// Proven to be in use by the mutator.
    Free /// Proven to NOT be in use by the mutator.
  };

  // Mutator methods.

  WeakRefSlot(HermesValue v) {
    reset(v);
  }

#if 1
  /// Tagged pointer implementation. Only supports HermesValues with object tag.

  bool hasValue() const {
    return hasPointer();
  }

  /// Return the object as a HermesValue.
  const HermesValue value() const {
    assert(state() == Unmarked && "unclean GC mark state");
    assert(hasPointer() && "tried to access collected referent");
    static_assert(Unmarked == 0, "unmarked state should not need untagging");
    return HermesValue::encodeObjectValue(tagged_);
  }

  // GC methods to update slot when referent moves/dies.

  /// Return the pointer to a GCCell, whether or not this slot is marked.
  void *getPointer() const {
    assert(state() != Free && "use nextFree instead");
    return tagged_ - state();
  }

  /// Update the stored pointer (because the object moved).
  void setPointer(void *newPtr) {
    assert(state() != Free && "tried to update unallocated slot");
    tagged_ = (char *)newPtr + (ptrdiff_t)state();
  }

  /// Clear the pointer (because the object died).
  void clearPointer() {
    tagged_ = (char *)state();
  }

  // GC methods to recycle slots.

  /// Return true if this slot stores a non-null pointer to something. For any
  /// slot reachable by the mutator, that something is a GCCell.
  bool hasPointer() const {
    return reinterpret_cast<uintptr_t>(tagged_) > Free;
  }

  State state() const {
    return static_cast<State>((reinterpret_cast<uintptr_t>(tagged_) & 3));
  }

  void mark() {
    assert(state() == Unmarked && "already marked");
    tagged_ += Marked;
  }

  void unmark() {
    assert(state() == Marked && "not yet marked");
    tagged_ -= Marked;
  }

  void free(WeakRefSlot *nextFree) {
    assert(state() == Unmarked && "cannot free a reachable slot");
    tagged_ = (char *)nextFree;
    tagged_ += Free;
    assert(state() == Free);
  }

  WeakRefSlot *nextFree() const {
    assert(state() == Free);
    return (WeakRefSlot *)(tagged_ - Free);
  }

  /// Re-initialize a freed slot.
  void reset(HermesValue v) {
    assert(v.isObject() && "Weak ref must be to object");
    static_assert(Unmarked == 0, "unmarked state should not need tagging");
    tagged_ = (char *)v.getObject();
    assert(state() == Unmarked && "initial state should be unmarked");
  }

#ifdef HERMESVM_SERIALIZE
  // Deserialization methods.
  WeakRefSlot() : tagged_{nullptr} {}
  // RelocationKind::NativePointer is kind of a misnomer: it really refers
  // to the kind of pointer - a raw pointer, as opposed to HermesValue or
  // GCPointer - not the type of the pointee (in this case, a GCCell).
  static constexpr RelocationKind kRelocKind = RelocationKind::NativePointer;
  void *deserializeAddr() {
    return &tagged_;
  }
#endif // HERMESVM_SERIALIZE

 private:
  /// Tagged pointer to either a GCCell or another WeakRefSlot (if the slot has
  /// been freed for reuse). Typed as char* to simplify tagging/untagging.
  /// The low two bits encode the integer value of the state.
  char *tagged_;

#else
  /// HermesValue implementation. Supports any value as referent.

  bool hasValue() const {
    return !value_.isEmpty();
  }

  /// Return the object as a HermesValue.
  const HermesValue value() const {
    assert(state() == Unmarked && "unclean GC mark state");
    assert(hasValue() && "tried to access collected referent");
    return value_;
  }

  // GC methods to update slot when referent moves/dies.

  /// Return true if this slot stores a non-null pointer to something. For any
  /// slot reachable by the mutator, that something is a GCCell.
  bool hasPointer() const {
    return value_.isPointer();
  }

  /// Return the pointer to a GCCell, whether or not this slot is marked.
  void *getPointer() const {
    assert(state() != Free && "use nextFree instead");
    return value_.getPointer();
  }

  /// Update the stored pointer (because the object moved).
  void setPointer(void *newPtr) {
    assert(state() != Free && "tried to update unallocated slot");
    value_ = value_.updatePointer(newPtr);
  }

  /// Clear the pointer (because the object died).
  void clearPointer() {
    value_ = HermesValue::encodeEmptyValue();
  }

  // GC methods to recycle slots.

  State state() const {
    return state_;
  }

  void mark() {
    assert(state() == Unmarked && "already marked");
    state_ = Marked;
  }

  void unmark() {
    assert(state() == Marked && "not yet marked");
    state_ = Unmarked;
  }

  void free(WeakRefSlot *nextFree) {
    assert(state() == Unmarked && "cannot free a reachable slot");
    state_ = Free;
    value_ = HermesValue::encodeNativePointer(nextFree);
    assert(state() == Free);
  }

  WeakRefSlot *nextFree() const {
    assert(state() == Free);
    return value_.getNativePointer<WeakRefSlot>();
  }

  /// Re-initialize a freed slot.
  void reset(HermesValue v) {
    static_assert(Unmarked == 0, "unmarked state should not need tagging");
    state_ = Unmarked;
    value_ = v;
    assert(state() == Unmarked && "initial state should be unmarked");
  }

#ifdef HERMESVM_SERIALIZE
  // Deserialization methods.
  WeakRefSlot() : value_{HermesValue::encodeEmptyValue()}, state_{Unmarked} {}
  static constexpr RelocationKind kRelocKind = RelocationKind::HermesValue;
  void *deserializeAddr() {
    return &value_;
  }
#endif // HERMESVM_SERIALIZE
 private:
  PinnedHermesValue value_;
  State state_;
#endif
  // End of split between tagged pointer/HermesValue implementations.
};
using WeakSlotState = WeakRefSlot::State;

/// This is a concrete base of \c WeakRef<T> that can be passed to concrete
/// functions in GC.
class WeakRefBase {
 protected:
  WeakRefSlot *slot_;
  WeakRefBase(WeakRefSlot *slot) : slot_(slot) {}

 public:
  /// \return true if the referenced object hasn't been freed.
  bool isValid() const {
    return slot_->hasValue();
  }

  /// \return true if the given slot stores a non-empty value.
  static bool isSlotValid(const WeakRefSlot *slot) {
    assert(slot && "slot must not be null");
    return slot->hasValue();
  }

  /// \return a pointer to the slot used by this WeakRef.
  /// Used primarily when populating a DenseMap with WeakRef keys.
  WeakRefSlot *unsafeGetSlot() {
    return slot_;
  }
  const WeakRefSlot *unsafeGetSlot() const {
    return slot_;
  }

  /// \return the stored HermesValue.
  /// The weak ref may be invalid, in which case an "empty" value is returned.
  /// This is an unsafe function since the referenced object may be freed any
  /// time that GC occurs.
  HermesValue unsafeGetHermesValue() const {
    return slot_->value();
  }
};

inline HeapSnapshot::NodeID GCBase::getObjectID(const void *cell) {
  assert(cell && "Called getObjectID on a null pointer");
  return idTracker_.getObjectID(cell);
}

inline HeapSnapshot::NodeID GCBase::getObjectID(const GCPointerBase &cell) {
  assert(cell && "Called getObjectID on a null pointer");
  return getObjectID(cell.get(pointerBase_));
}

inline HeapSnapshot::NodeID GCBase::getNativeID(const void *mem) {
  assert(mem && "Called getNativeID on a null pointer");
  return idTracker_.getNativeID(mem);
}

#ifndef NDEBUG
inline uint64_t GCBase::nextObjectID() {
  return debugAllocationCounter_++;
}
#endif

inline bool GCBase::IDTracker::isTrackingIDs() const {
  return !objectIDMap_.empty();
}

inline HeapSnapshot::NodeID GCBase::IDTracker::getObjectID(const void *cell) {
  auto iter = objectIDMap_.find(cell);
  if (iter != objectIDMap_.end()) {
    return iter->second;
  }
  // Else, assume it is an object that needs to be tracked and give it a new ID.
  const auto objID = nextObjectID();
  objectIDMap_[cell] = objID;
  return objID;
}

inline HeapSnapshot::NodeID GCBase::IDTracker::getNativeID(const void *mem) {
  auto iter = objectIDMap_.find(mem);
  if (iter != objectIDMap_.end()) {
    return iter->second;
  }
  // Else, assume it is a piece of native memory that needs to be tracked and
  // give it a new ID.
  const auto objID = nextNativeID();
  objectIDMap_[mem] = objID;
  return objID;
}

inline void GCBase::IDTracker::moveObject(
    const void *oldLocation,
    const void *newLocation) {
  if (oldLocation == newLocation) {
    // Don't need to do anything if the object isn't moving anywhere. This can
    // happen in old generations where it is compacted to the same location.
    return;
  }
  auto old = objectIDMap_.find(oldLocation);
  if (old == objectIDMap_.end()) {
    // Avoid making new keys for objects that don't need to be tracked.
    return;
  }
  const auto oldID = old->second;
  assert(
      objectIDMap_.count(newLocation) == 0 &&
      "Moving to a location that is already tracked");
  // Have to erase first, because any other access can invalidate the iterator.
  objectIDMap_.erase(old);
  objectIDMap_[newLocation] = oldID;
}

inline void GCBase::IDTracker::untrackObject(const void *cell) {
  objectIDMap_.erase(cell);
}

inline void GCBase::IDTracker::untrackNative(const void *mem) {
  // Since native memory and heap memory share the same map, this is the same
  // as untracking an object.
  untrackObject(mem);
}

template <typename F>
inline void GCBase::IDTracker::forEachID(F callback) {
  for (auto &p : objectIDMap_) {
    callback(p.first, p.second);
  }
}

inline HeapSnapshot::NodeID GCBase::IDTracker::nextObjectID() {
  // This must be unique for most features that rely on it, check for overflow.
  if (LLVM_UNLIKELY(
          nextID_ >=
          std::numeric_limits<HeapSnapshot::NodeID>::max() - kIDStep)) {
    hermes_fatal("Ran out of object IDs");
  }
  return nextID_ += kIDStep;
}

inline HeapSnapshot::NodeID GCBase::IDTracker::nextNativeID() {
  // This must be unique for most features that rely on it, check for overflow.
  if (LLVM_UNLIKELY(
          nextNativeID_ >=
          std::numeric_limits<HeapSnapshot::NodeID>::max() - kIDStep)) {
    hermes_fatal("Ran out of native IDs");
  }
  return nextNativeID_ += kIDStep;
}

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_GCBASE_H
