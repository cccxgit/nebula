/* Copyright (c) 2021 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "kvstore/wal/AtomicLogBuffer.h"

#include <chrono>

DEFINE_int32(max_log_buffer_size,
             16 * 1024 * 1024,
             "Max atomic log buffer size, if the size exceeds it, will trigger gc");
DEFINE_bool(wal_memory_observe, false, "Enable startup-only AtomicLogBuffer memory observations");
DEFINE_uint32(wal_memory_observe_interval_secs,
              10,
              "Minimum seconds between WAL memory observations");

namespace nebula {
namespace wal {

namespace {

// Process-wide observations, not a heap profiler. Individual relaxed loads do not form a
// simultaneous snapshot. Enabling is latched on first use so node lifetimes cannot be half-counted
// by changing the flag after buffers have already been populated.
struct WalMemoryObservations {
  const bool enabled{FLAGS_wal_memory_observe};
  std::atomic<int64_t> liveNodes{0};
  std::atomic<int64_t> created{0};
  std::atomic<int64_t> freed{0};
  std::atomic<int64_t> pushes{0};
  std::atomic<int64_t> emptyPushes{0};
  std::atomic<int64_t> liveLogicalBytes{0};
  std::atomic<int64_t> releases{0};
  std::atomic<int64_t> gcRuns{0};
  std::atomic<int64_t> gcFreed{0};
  std::atomic<int64_t> resets{0};
  std::atomic<int64_t> lastLogSecond{0};
};

WalMemoryObservations& memoryObservations() {
  static WalMemoryObservations observations;
  return observations;
}

// Called only at existing ownership-safe deletion sites; never walks a concurrently modified list.
void observeNodeDeletion(const Node* node, bool byGc) {
  auto& observation = memoryObservations();
  if (!observation.enabled) {
    return;
  }
  observation.liveLogicalBytes.fetch_sub(node->size_, std::memory_order_relaxed);
  observation.liveNodes.fetch_sub(1, std::memory_order_relaxed);
  observation.freed.fetch_add(1, std::memory_order_relaxed);
  if (byGc) {
    observation.gcFreed.fetch_add(1, std::memory_order_relaxed);
  }
}

void observeMemory(const void* buffer,
                   const char* event,
                   int32_t size,
                   int32_t capacity,
                   int32_t dirtyNodes,
                   int32_t refs) {
  auto& observation = memoryObservations();
  auto now = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
                 .count();
  auto previous = observation.lastLogSecond.load(std::memory_order_relaxed);
  auto interval = std::max<uint32_t>(1, FLAGS_wal_memory_observe_interval_secs);
  if (now - previous < interval || !observation.lastLogSecond.compare_exchange_strong(
                                       previous, now, std::memory_order_relaxed)) {
    return;
  }
  auto liveNodes = observation.liveNodes.load(std::memory_order_relaxed);
  LOG(INFO) << "WAL_MEMORY_OBSERVE event=" << event << " buffer=" << buffer
            << " live_nodes=" << liveNodes
            << " created=" << observation.created.load(std::memory_order_relaxed)
            << " freed=" << observation.freed.load(std::memory_order_relaxed)
            << " pushes=" << observation.pushes.load(std::memory_order_relaxed)
            << " empty_pushes=" << observation.emptyPushes.load(std::memory_order_relaxed)
            << " live_logical_bytes="
            << observation.liveLogicalBytes.load(std::memory_order_relaxed)
            << " releases=" << observation.releases.load(std::memory_order_relaxed)
            << " gc_runs=" << observation.gcRuns.load(std::memory_order_relaxed)
            << " gc_freed=" << observation.gcFreed.load(std::memory_order_relaxed)
            << " resets=" << observation.resets.load(std::memory_order_relaxed)
            << " sizeof_record=" << sizeof(Record) << " sizeof_node=" << sizeof(Node)
            << " node_object_bytes=" << liveNodes * static_cast<int64_t>(sizeof(Node))
            << " buffer_size=" << size << " buffer_capacity=" << capacity
            << " buffer_dirty_nodes=" << dirtyNodes << " buffer_refs=" << refs;
}

}  // namespace

LogIterator& AtomicLogBuffer::Iterator::operator++() {
  currIndex_++;
  currLogId_++;
  if (currLogId_ > end_) {
    valid_ = false;
    currRec_ = nullptr;
    return *this;
  }
  // Operations after load SHOULD NOT reorder before it.
  auto pos = currNode_->pos_.load(std::memory_order_acquire);
  VLOG(5) << "currNode firstLogId = " << currNode_->firstLogId_ << ", currIndex = " << currIndex_
          << ", currNode pos " << pos;
  if (currIndex_ >= pos) {
    currNode_ = currNode_->prev_.load(std::memory_order_relaxed);
    if (currNode_ == nullptr) {
      valid_ = false;
      currRec_ = nullptr;
      return *this;
    } else {
      currIndex_ = 0;
    }
  }
  DCHECK_LT(currIndex_, kMaxLength);
  currRec_ = DCHECK_NOTNULL(currNode_)->rec(currIndex_);
  return *this;
}

void AtomicLogBuffer::Iterator::seek(LogID logId) {
  currNode_ = logBuffer_->seek(logId);
  if (currNode_ != nullptr) {
    currIndex_ = logId - currNode_->firstLogId_;
    // Since reader is only a snapshot, a possible case is that logId > currNode->firstLogId_,
    // however, the logId we search may not in currNode. (e.g. currNode_ is the latest node,
    // but currIndex_ >= kMaxLength). In this case, currRec_ will be an invalid one.
    currRec_ = currNode_->rec(currIndex_);
    valid_ = (currRec_ != nullptr);
  } else {
    valid_ = false;
  }
}

AtomicLogBuffer::~AtomicLogBuffer() {
  auto refs = refs_.load(std::memory_order_acquire);
  CHECK_EQ(0, refs);
  auto* curr = head_.load(std::memory_order_relaxed);
  auto* prev = curr;
  while (curr != nullptr) {
    curr = curr->next_;
    observeNodeDeletion(prev, false);
    delete prev;
    prev = curr;
  }
  if (prev != nullptr) {
    observeNodeDeletion(prev, false);
    delete prev;
  }
}

void AtomicLogBuffer::push(LogID logId, Record&& record) {
  auto& observation = memoryObservations();
  if (observation.enabled) {
    observation.pushes.fetch_add(1, std::memory_order_relaxed);
    if (record.msg_.empty()) {
      observation.emptyPushes.fetch_add(1, std::memory_order_relaxed);
    }
  }
  auto* head = head_.load(std::memory_order_relaxed);
  auto recSize = record.size();
  if (head == nullptr || head->isFull() || head->markDeleted_.load(std::memory_order_relaxed)) {
    auto* newNode = new Node();
    if (observation.enabled) {
      observation.created.fetch_add(1, std::memory_order_relaxed);
      observation.liveNodes.fetch_add(1, std::memory_order_relaxed);
    }
    newNode->firstLogId_ = logId;
    newNode->next_ = head;
    newNode->push_back(std::move(record));
    if (observation.enabled) {
      observation.liveLogicalBytes.fetch_add(recSize, std::memory_order_relaxed);
    }
    if (head == nullptr || head->markDeleted_.load(std::memory_order_relaxed)) {
      // It is the first Node in current list, or head has been marked as
      // deleted
      firstLogId_.store(logId, std::memory_order_relaxed);
      tail_.store(newNode, std::memory_order_relaxed);
    } else if (head != nullptr) {
      head->prev_.store(newNode, std::memory_order_release);
    }
    size_.fetch_add(recSize, std::memory_order_relaxed);
    head_.store(newNode, std::memory_order_relaxed);
    if (observation.enabled) {
      observeMemory(this, "push_new_node", size_, capacity_, dirtyNodes_, refs_);
    }
    return;
  }
  if (size_ + recSize > capacity_) {
    auto* tail = tail_.load(std::memory_order_relaxed);
    // todo(doodle): there is a potential problem is that: since Node::isFull
    // is judged by log count, we can only add new node when previous node
    // has enough logs. So when tail is equal to head, we need to wait tail is
    // full, after head moves forward, at then tail can be marked as deleted.
    // So the log buffer would takes up more memory than its capacity. Since
    // it does not affect correctness, we could fix it later if necessary.
    if (tail != head) {
      // We have more than one nodes in current list.
      // So we mark the tail to be deleted.
      bool expected = false;
      VLOG(5) << "Mark node " << tail->firstLogId_ << " to be deleted!";
      auto marked =
          tail->markDeleted_.compare_exchange_strong(expected, true, std::memory_order_relaxed);
      auto* prev = tail->prev_.load(std::memory_order_relaxed);
      firstLogId_.store(prev->firstLogId_, std::memory_order_relaxed);
      // All operations above SHOULD NOT be reordered.
      tail_.store(tail->prev_, std::memory_order_release);
      if (marked) {
        size_.fetch_sub(tail->size_, std::memory_order_relaxed);
        // dirtyNodes_ changes SHOULD after the tail move.
        dirtyNodes_.fetch_add(1, std::memory_order_release);
      }
    }
  }
  size_.fetch_add(recSize, std::memory_order_relaxed);
  head->push_back(std::move(record));
  if (observation.enabled) {
    observation.liveLogicalBytes.fetch_add(recSize, std::memory_order_relaxed);
    observeMemory(this, "push", size_, capacity_, dirtyNodes_, refs_);
  }
}

void AtomicLogBuffer::reset() {
  auto* p = head_.load(std::memory_order_relaxed);
  int32_t count = 0;
  while (p != nullptr) {
    bool expected = false;
    if (!p->markDeleted_.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
      // The rest nodes has been mark deleted.
      break;
    }
    p = p->next_;
    ++count;
  }
  size_.store(0, std::memory_order_relaxed);
  firstLogId_.store(0, std::memory_order_relaxed);
  dirtyNodes_.fetch_add(count, std::memory_order_release);
  auto& observation = memoryObservations();
  if (observation.enabled) {
    observation.resets.fetch_add(1, std::memory_order_relaxed);
    observeMemory(this, "reset", size_, capacity_, dirtyNodes_, refs_);
  }
}

Node* AtomicLogBuffer::seek(LogID logId) {
  auto* head = head_.load(std::memory_order_relaxed);
  if (head != nullptr && logId > head->lastLogId()) {
    VLOG(5) << "Bad seek, the seeking logId " << logId
            << " is greater than the latest logId in buffer " << head->lastLogId();
    return nullptr;
  }
  auto* p = head;
  if (p == nullptr) {
    return nullptr;
  }
  auto* tail = tail_.load(std::memory_order_relaxed);
  CHECK_NOTNULL(tail);
  // The scan range is [head, tail]
  // And we should ensure the nodes inside the range SHOULD NOT be deleted.
  // We could ensure the tail during gc is older than current one.
  while (p != tail->next_ && !p->markDeleted_) {
    VLOG(5) << "current node firstLogId = " << p->firstLogId_ << ", the seeking logId = " << logId;
    if (logId >= p->firstLogId_) {
      break;
    }
    p = p->next_;
  }
  if (p == nullptr) {
    return nullptr;
  }
  return p->markDeleted_ ? nullptr : p;
}

void AtomicLogBuffer::releaseRef() {
  // All operations following SHOULD NOT reordered before tail.load()
  // so we could ensure the tail used in GC is older than new coming readers.
  auto* tail = tail_.load(std::memory_order_acquire);
  auto readers = refs_.fetch_sub(1, std::memory_order_relaxed);
  auto& observation = memoryObservations();
  if (observation.enabled) {
    observation.releases.fetch_add(1, std::memory_order_relaxed);
  }
  VLOG(5) << "Release ref, readers = " << readers << ", size = " << size_
          << ", dirty node = " << dirtyNodes_;
  // todo(doodle): https://github.com/vesoft-inc/nebula-storage/issues/390
  if (readers > 1) {
    if (observation.enabled) {
      observeMemory(this, "release_shared", size_, capacity_, dirtyNodes_, refs_);
    }
    return;
  }
  // In this position, maybe there are some new readers coming in
  // So we should load tail before refs count down to ensure the tail current
  // thread got is older than the new readers see.

  CHECK_EQ(1, readers);

  auto dirtyNodes = dirtyNodes_.load(std::memory_order_relaxed);
  bool gcRunning = false;

  if (dirtyNodes > dirtyNodesLimit_ || size_ > FLAGS_max_log_buffer_size) {
    if (gcOnGoing_.compare_exchange_strong(gcRunning, true, std::memory_order_acquire)) {
      if (observation.enabled) {
        observation.gcRuns.fetch_add(1, std::memory_order_relaxed);
      }
      VLOG(4) << "GC begins!";
      // It means no readers on the deleted nodes.
      // Cut-off the list.
      CHECK_NOTNULL(tail);
      auto* dirtyHead = tail->next_;
      tail->next_ = nullptr;

      // Now we begin to delete the nodes.
      auto* curr = dirtyHead;
      while (curr != nullptr) {
        CHECK(curr->markDeleted_.load(std::memory_order_relaxed));
        VLOG(5) << "Delete node " << curr->firstLogId_;
        auto* del = curr;
        curr = curr->next_;
        observeNodeDeletion(del, true);
        delete del;
        dirtyNodes_.fetch_sub(1, std::memory_order_release);
        CHECK_GE(dirtyNodes_, 0);
      }

      gcOnGoing_.store(false, std::memory_order_release);
      VLOG(4) << "GC finished!";
    } else {
      VLOG(5) << "Current list is in gc now!";
    }
  }
  if (observation.enabled) {
    observeMemory(this, "release_last", size_, capacity_, dirtyNodes_, refs_);
  }
}

}  // namespace wal
}  // namespace nebula
