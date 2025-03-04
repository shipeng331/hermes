/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/SegmentedArray.h"

#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/HermesValue-inline.h"

#include "llvm/Support/Debug.h"
#define DEBUG_TYPE "serialize"

namespace hermes {
namespace vm {

VTable SegmentedArray::Segment::vt(
    CellKind::SegmentKind,
    cellSize<SegmentedArray::Segment>(),
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr, // externalMemorySize
    VTable::HeapSnapshotMetadata{HeapSnapshot::NodeType::Array,
                                 nullptr,
                                 nullptr,
                                 nullptr});

void SegmentBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const SegmentedArray::Segment *>(cell);
  mb.addArray<Metadata::ArrayData::ArrayType::HermesValue>(
      "data", &self->data_, &self->length_, sizeof(GCHermesValue));
}

#ifdef HERMESVM_SERIALIZE
SegmentedArray::Segment::Segment(Deserializer &d)
    : GCCell(&d.getRuntime()->getHeap(), &vt) {
  length_ = d.readInt<uint32_t>();
  for (uint32_t i = 0; i < length_; i++) {
    d.readHermesValue(&data_[i]);
  }
}

void SegmentSerialize(Serializer &s, const GCCell *cell) {
  auto *self = vmcast<const SegmentedArray::Segment>(cell);
  s.writeInt<uint32_t>(self->length_);
  for (uint32_t i = 0; i < self->length_; i++) {
    s.writeHermesValue(self->data_[i]);
  }
  s.endObject(cell);
}

void SegmentDeserialize(Deserializer &d, CellKind kind) {
  assert(kind == CellKind::SegmentKind && "Expected Segment");
  void *mem = d.getRuntime()->alloc(cellSize<SegmentedArray::Segment>());
  auto *cell = new (mem) SegmentedArray::Segment(d);
  d.endObject(cell);
}
#endif

PseudoHandle<SegmentedArray::Segment> SegmentedArray::Segment::create(
    Runtime *runtime) {
  // NOTE: This needs to live in the cpp file instead of the header because it
  // uses PseudoHandle, which requires a specialization of IsGCObject for the
  // type it constructs.
  return createPseudoHandle(new (runtime->alloc(cellSize<Segment>()))
                                Segment(runtime));
}

void SegmentedArray::Segment::setLength(uint32_t newLength) {
  if (newLength > length_) {
    // Length is increasing, fill with emptys.
    GCHermesValue::fill(
        data_ + length_, data_ + newLength, HermesValue::encodeEmptyValue());
  }
  // If length is decreasing nothing special needs to be done.
  setLengthWithoutFilling(newLength);
}

VTable SegmentedArray::vt(
    CellKind::SegmentedArrayKind,
    /*variableSize*/ 0,
    nullptr,
    nullptr,
    nullptr,
    // TODO(T43077289): if we rehabilitate SegmentedArray trimming, reenable
    // this code.
    /* _trimSizeCallback */ nullptr,
    /* _trimCallback */ nullptr,
    nullptr, // externalMemorySize
    VTable::HeapSnapshotMetadata{HeapSnapshot::NodeType::Array,
                                 nullptr,
                                 nullptr,
                                 nullptr});

void SegmentedArrayBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const SegmentedArray *>(cell);
  mb.addArray<Metadata::ArrayData::ArrayType::HermesValue>(
      "slots",
      self->inlineStorage(),
      &self->numSlotsUsed_,
      sizeof(GCHermesValue));
}

#ifdef HERMESVM_SERIALIZE
void SegmentedArraySerialize(Serializer &s, const GCCell *cell) {
  auto *self = vmcast<const SegmentedArray>(cell);
  s.writeInt<SegmentedArray::size_type>(self->slotCapacity_);
  s.writeInt<SegmentedArray::size_type>(self->numSlotsUsed_);

  for (uint32_t i = 0; i < self->numSlotsUsed_; i++) {
    s.writeHermesValue(self->at(i));
  }

  s.endObject(cell);
}

void SegmentedArrayDeserialize(Deserializer &d, CellKind kind) {
  assert(kind == CellKind::SegmentedArrayKind && "Expected SegmentedArray");
  SegmentedArray::size_type slotCapacity =
      d.readInt<SegmentedArray::size_type>();
  SegmentedArray::size_type numSlotsUsed =
      d.readInt<SegmentedArray::size_type>();
  void *mem = d.getRuntime()->alloc<false /*fixedSize*/>(
      SegmentedArray::allocationSizeForSlots(slotCapacity));
  auto *cell =
      new (mem) SegmentedArray(d.getRuntime(), slotCapacity, numSlotsUsed);
  for (auto it = cell->begin(); it != cell->end(); ++it) {
    d.readHermesValue(&*it);
  }
  d.endObject(cell);
}
#endif

CallResult<HermesValue> SegmentedArray::create(
    Runtime *runtime,
    size_type capacity) {
  if (LLVM_UNLIKELY(capacity > maxElements())) {
    return throwExcessiveCapacityError(runtime, capacity);
  }
  // Leave the segments as null. Whenever the size is changed, the segments will
  // be allocated.
  return HermesValue::encodeObjectValue(new (
      runtime->alloc<false /*fixedSize*/>(allocationSizeForCapacity(capacity)))
                                            SegmentedArray(runtime, capacity));
}

CallResult<HermesValue> SegmentedArray::createLongLived(
    Runtime *runtime,
    size_type capacity) {
  if (LLVM_UNLIKELY(capacity > maxElements())) {
    return throwExcessiveCapacityError(runtime, capacity);
  }
  // Leave the segments as null. Whenever the size is changed, the segments will
  // be allocated.
  return HermesValue::encodeObjectValue(new (runtime->allocLongLived(
      allocationSizeForCapacity(capacity))) SegmentedArray(runtime, capacity));
}

CallResult<HermesValue>
SegmentedArray::create(Runtime *runtime, size_type capacity, size_type size) {
  auto arrRes = create(runtime, capacity);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = createPseudoHandle(vmcast<SegmentedArray>(*arrRes));
  // TODO T25663446: This is potentially optimizable to iterate over the inline
  // storage and the segments separately.
  self = increaseSize</*Fill*/ true>(runtime, std::move(self), size);
  return self.getHermesValue();
}

ExecutionStatus SegmentedArray::push_back(
    MutableHandle<SegmentedArray> &self,
    Runtime *runtime,
    Handle<> value) {
  auto oldSize = self->size();
  if (growRight(self, runtime, 1) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto &elm = self->at(oldSize);
  elm.set(*value, &runtime->getHeap());
  return ExecutionStatus::RETURNED;
}

ExecutionStatus SegmentedArray::resize(
    MutableHandle<SegmentedArray> &self,
    Runtime *runtime,
    size_type newSize) {
  if (newSize > self->size()) {
    return growRight(self, runtime, newSize - self->size());
  } else if (newSize < self->size()) {
    self->shrinkRight(self->size() - newSize);
  }
  return ExecutionStatus::RETURNED;
}

ExecutionStatus SegmentedArray::resizeLeft(
    MutableHandle<SegmentedArray> &self,
    Runtime *runtime,
    size_type newSize) {
  if (newSize == self->size()) {
    return ExecutionStatus::RETURNED;
  } else if (newSize > self->size()) {
    return growLeft(self, runtime, newSize - self->size());
  } else {
    self->shrinkLeft(runtime, self->size() - newSize);
    return ExecutionStatus::RETURNED;
  }
}

void SegmentedArray::resizeWithinCapacity(
    PseudoHandle<SegmentedArray> self,
    Runtime *runtime,
    size_type newSize) {
  const size_type currSize = self->size();
  assert(
      newSize <= self->capacity() &&
      "Cannot resizeWithinCapacity to a size not within capacity");
  if (newSize > currSize) {
    growRightWithinCapacity(runtime, std::move(self), newSize - currSize);
  } else if (newSize < currSize) {
    self->shrinkRight(currSize - newSize);
  }
}

ExecutionStatus SegmentedArray::throwExcessiveCapacityError(
    Runtime *runtime,
    size_type capacity) {
  assert(
      capacity > maxElements() &&
      "Shouldn't call this without first checking that capacity is big");
  return runtime->raiseRangeError(
      TwineChar16(
          "Requested an array size larger than the max allowable: Requested elements = ") +
      capacity + ", max elements = " + maxElements());
}

void SegmentedArray::allocateSegment(
    Runtime *runtime,
    Handle<SegmentedArray> self,
    SegmentNumber segment) {
  assert(
      self->segmentAtPossiblyUnallocated(segment)->isEmpty() &&
      "Allocating into a non-empty segment");
  PseudoHandle<Segment> c = Segment::create(runtime);
  self->segmentAtPossiblyUnallocated(segment)->set(
      c.getHermesValue(), &runtime->getHeap());
}

ExecutionStatus SegmentedArray::growRight(
    MutableHandle<SegmentedArray> &self,
    Runtime *runtime,
    size_type amount) {
  if (self->size() + amount <= self->capacity()) {
    SegmentedArray::growRightWithinCapacity(runtime, self, amount);
    return ExecutionStatus::RETURNED;
  }
  const auto newSize = self->size() + amount;
  // Allocate a new SegmentedArray according to the resize policy.
  auto arrRes = create(runtime, calculateNewCapacity(self->size(), newSize));
  if (arrRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto newSegmentedArray = createPseudoHandle(vmcast<SegmentedArray>(*arrRes));
  // Copy inline storage and segments over.
  // Do this with raw pointers so that the range write barrier occurs.
  GCHermesValue::copy(
      self->inlineStorage(),
      self->inlineStorage() + self->numSlotsUsed_,
      newSegmentedArray->inlineStorage(),
      &runtime->getHeap());
  // Set the size of the new array to be the same as the old array's size.
  newSegmentedArray->numSlotsUsed_ = self->numSlotsUsed_;
  newSegmentedArray = increaseSize</*Fill*/ true>(
      runtime, std::move(newSegmentedArray), amount);
  // Assign back to self.
  self = newSegmentedArray.get();
  return ExecutionStatus::RETURNED;
}

ExecutionStatus SegmentedArray::growLeft(
    MutableHandle<SegmentedArray> &self,
    Runtime *runtime,
    size_type amount) {
  if (self->size() + amount < self->capacity()) {
    growLeftWithinCapacity(runtime, self, amount);
    return ExecutionStatus::RETURNED;
  }
  const auto newSize = self->size() + amount;
  auto arrRes = create(runtime, calculateNewCapacity(self->size(), newSize));
  if (arrRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto newSegmentedArray = createPseudoHandle(vmcast<SegmentedArray>(*arrRes));
  // Don't fill with empty values, most will be copied in.
  newSegmentedArray = increaseSize</*Fill*/ false>(
      runtime, std::move(newSegmentedArray), newSize);
  // Fill the beginning of the new array with empty values.
  GCHermesValue::fill(
      newSegmentedArray->begin(),
      newSegmentedArray->begin() + amount,
      HermesValue::encodeEmptyValue());
  // Copy element-by-element, since a shift would need to happen anyway.
  // Since self and newSegmentedArray are distinct, don't need to worry about
  // order.
  GCHermesValue::copy(
      self->begin(),
      self->end(),
      newSegmentedArray->begin() + amount,
      &runtime->getHeap());
  // Assign back to self.
  self = newSegmentedArray.get();
  return ExecutionStatus::RETURNED;
}

void SegmentedArray::growRightWithinCapacity(
    Runtime *runtime,
    PseudoHandle<SegmentedArray> self,
    size_type amount) {
  assert(
      self->size() + amount <= self->capacity() &&
      "Cannot grow higher than capacity");
  increaseSize</*Fill*/ true>(runtime, std::move(self), amount);
}

void SegmentedArray::growLeftWithinCapacity(
    Runtime *runtime,
    PseudoHandle<SegmentedArray> self,
    size_type amount) {
  assert(
      self->size() + amount <= self->capacity() &&
      "Cannot grow higher than capacity");
  // Don't fill with empty values since we will overwrite the end anyway.
  self = increaseSize</*Fill*/ false>(runtime, std::move(self), amount);
  // Copy the range from the beginning to the end.
  GCHermesValue::copy_backward(
      self->begin(), self->end() - amount, self->end(), &runtime->getHeap());
  // Fill the beginning with empty values.
  GCHermesValue::fill(
      self->begin(), self->begin() + amount, HermesValue::encodeEmptyValue());
}

void SegmentedArray::shrinkRight(size_type amount) {
  decreaseSize(amount);
}

void SegmentedArray::shrinkLeft(Runtime *runtime, size_type amount) {
  // Copy the end values leftwards to the beginning.
  GCHermesValue::copy(begin() + amount, end(), begin(), &runtime->getHeap());
  // Now that all the values are moved down, fill the end with empty values.
  decreaseSize(amount);
}

template <bool Fill>
PseudoHandle<SegmentedArray> SegmentedArray::increaseSize(
    Runtime *runtime,
    PseudoHandle<SegmentedArray> self,
    size_type amount) {
  const auto empty = HermesValue::encodeEmptyValue();
  const auto currSize = self->size();
  const auto finalSize = currSize + amount;

  if (currSize <= kValueToSegmentThreshold &&
      finalSize <= kValueToSegmentThreshold) {
    // currSize and finalSize are inside inline storage, bump and fill.
    if (Fill) {
      GCHermesValue::fill(
          self->inlineStorage() + currSize,
          self->inlineStorage() + finalSize,
          empty);
    }
    // Set the final size.
    self->numSlotsUsed_ = finalSize;
    return self;
  }

  // currSize might be in inline storage, but finalSize is definitely in
  // segments.
  // Allocate missing segments after filling inline storage.
  if (currSize <= kValueToSegmentThreshold) {
    // Segments will need to be allocated, if the old size didn't have the
    // inline storage filled up, fill it up now.
    GCHermesValue::fill(
        self->inlineStorage() + currSize,
        self->inlineStorage() + kValueToSegmentThreshold,
        empty);
    // Set the size to the inline storage threshold.
    self->numSlotsUsed_ = kValueToSegmentThreshold;
  }

  // NOTE: during this function, allocations can happen.
  // If one of these allocations triggers a full compacting GC, then the array
  // currently being increased might have its capacity shrunk to match its
  // numSlotsUsed. So, increase numSlotsUsed immediately to its final value
  // before the allocations happen so it isn't shrunk, and also fill with empty
  // values so that any mark passes don't fail.
  // The segments should all have length 0 until allocations are finished, so
  // that uninitialized memory is not scanned inside the segments. Once
  // allocations are finished, go back and fixup the lengths.
  const SegmentNumber startSegment =
      currSize <= kValueToSegmentThreshold ? 0 : toSegment(currSize - 1);
  const SegmentNumber lastSegment = toSegment(finalSize - 1);
  const auto newNumSlotsUsed = numSlotsForCapacity(finalSize);
  // Put empty values into all of the added slots so that the memory is not
  // uninitialized during marking.
  GCHermesValue::fill(
      self->inlineStorage() + self->numSlotsUsed_,
      self->inlineStorage() + newNumSlotsUsed,
      empty);
  self->numSlotsUsed_ = newNumSlotsUsed;

  // Allocate a handle to track the current array.
  auto selfHandle = toHandle(runtime, std::move(self));
  // Allocate each segment.
  if (startSegment <= lastSegment &&
      selfHandle->segmentAtPossiblyUnallocated(startSegment)->isEmpty()) {
    // The start segment might already be allocated if it was half full when we
    // increase the size.
    allocateSegment(runtime, selfHandle, startSegment);
  }
  for (auto i = startSegment + 1; i <= lastSegment; ++i) {
    // All segments except the start need to become allocated.
    allocateSegment(runtime, selfHandle, i);
  }

  // Now that all allocations have occurred, set the lengths inside each
  // segment, and optionally fill.
  for (auto i = startSegment; i <= lastSegment; ++i) {
    // If its the last chunk, set to the length required by any leftover
    // elements.
    const auto segmentLength =
        i == lastSegment ? toInterior(finalSize - 1) + 1 : Segment::kMaxLength;
    if (Fill) {
      selfHandle->segmentAt(i)->setLength(segmentLength);
    } else {
      selfHandle->segmentAt(i)->setLengthWithoutFilling(segmentLength);
    }
  }
  self = selfHandle;
  return self;
}

void SegmentedArray::decreaseSize(size_type amount) {
  assert(amount <= size() && "Cannot decrease size past zero");
  const auto finalSize = size() - amount;
  if (finalSize <= kValueToSegmentThreshold) {
    // Just adjust the field and exit, no segments to compress.
    numSlotsUsed_ = finalSize;
    return;
  }
  // Set the new last used segment's length to be the leftover.
  segmentAt(toSegment(finalSize - 1))->setLength(toInterior(finalSize - 1) + 1);
  numSlotsUsed_ = numSlotsForCapacity(finalSize);
}

gcheapsize_t SegmentedArray::_trimSizeCallback(const GCCell *cell) {
  const auto *self = reinterpret_cast<const SegmentedArray *>(cell);
  // This array will shrink so that it has the same slot capacity as the slot
  // size.
  return allocationSizeForSlots(self->numSlotsUsed_);
}

void SegmentedArray::_trimCallback(GCCell *cell) {
  auto *self = reinterpret_cast<SegmentedArray *>(cell);
  // Shrink so that the capacity is equal to the size.
  self->slotCapacity_ = self->numSlotsUsed_;
}

// Forward instantiations of increaseSize for use outside this file.
template PseudoHandle<SegmentedArray> SegmentedArray::increaseSize<true>(
    Runtime *runtime,
    PseudoHandle<SegmentedArray> self,
    size_type amount);
template PseudoHandle<SegmentedArray> SegmentedArray::increaseSize<false>(
    Runtime *runtime,
    PseudoHandle<SegmentedArray> self,
    size_type amount);

} // namespace vm
} // namespace hermes
