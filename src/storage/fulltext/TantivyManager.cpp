/* Copyright (c) 2026. Licensed under Apache 2.0. */
#include "storage/fulltext/TantivyManager.h"

#include <boost/filesystem.hpp>
#include <chrono>

#include "codec/RowReaderWrapper.h"
#include "common/utils/NebulaKeyUtils.h"
#include "kvstore/Part.h"

DEFINE_bool(enable_tantivy, true, "Enable embedded Tantivy full-text indexes");
DEFINE_uint64(tantivy_queue_bytes, 64 * 1024 * 1024, "Bounded committed mutation queue bytes");
DEFINE_uint64(tantivy_writer_bytes, 16 * 1024 * 1024, "Writer memory budget per full-text index");
DEFINE_uint32(tantivy_query_wait_seconds, 30, "Maximum wait for searchable committed mutations");

namespace nebula::storage {
using Code = nebula::cpp2::ErrorCode;
using Mutation = kvstore::CommittedMutation;

TantivyManager::TantivyManager(meta::MetaClient* meta,
                               meta::SchemaManager* schemas,
                               std::string root)
    : meta_(meta), schemas_(schemas), root_(std::move(root)) {}
TantivyManager::~TantivyManager() {
  stop();
}
void TantivyManager::start(kvstore::KVStore* store) {
  store_ = store;
  worker_ = std::thread([this] { run(); });
}
void TantivyManager::stop() {
  stopping_ = true;
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void TantivyManager::enqueue(GraphSpaceID space,
                             PartitionID part,
                             std::vector<Mutation>&& mutations) {
  if (!interested_ || stopping_) return;
  Event event;
  event.space = space;
  event.part = part;
  event.mutations = std::move(mutations);
  for (const auto& m : event.mutations) event.bytes += m.key.size() + m.value.size() + sizeof(m);
  const bool cleanup =
      event.mutations.size() == 1 && event.mutations.front().type == Mutation::Type::REMOVE_RANGE;
  std::unique_lock<std::mutex> guard(queueMutex_);
  // Allow one oversized Raft batch, but never accumulate unbounded batches.
  wake_.wait(guard, [&] {
    // Cleanup may hold a lock needed by the backfill; never wait to enqueue it.
    return stopping_ || cleanup || queuedBytes_ == 0 ||
           queuedBytes_ + event.bytes <= FLAGS_tantivy_queue_bytes;
  });
  if (stopping_) return;
  event.sequence = ++submitted_;
  queuedBytes_ += event.bytes;
  queue_.push_back(std::move(event));
  wake_.notify_all();
}

bool TantivyManager::matches(const Entry& entry, folly::StringPiece key, size_t vidLen) const {
  const auto& schema = entry.definition.get_depend_schema();
  if (schema.getType() == nebula::cpp2::SchemaID::Type::edge_type) {
    return NebulaKeyUtils::isEdge(vidLen, key) &&
           NebulaKeyUtils::getEdgeType(vidLen, key) == schema.get_edge_type();
  }
  return NebulaKeyUtils::isTag(vidLen, key) &&
         NebulaKeyUtils::getTagId(vidLen, key) == schema.get_tag_id();
}
void TantivyManager::fail(const std::shared_ptr<Entry>& entry, const std::string& reason) {
  entry->ready = false;
  entry->error = reason;
  LOG(ERROR) << "Tantivy index unavailable: " << reason;
}

bool TantivyManager::apply(const std::shared_ptr<Entry>& entry, const Mutation& mutation) {
  auto space = entry->definition.get_space_id();
  auto len = schemas_->getSpaceVidLen(space);
  if (!len.ok()) return false;
  if (!matches(*entry, mutation.key, len.value())) return true;
  if (mutation.type == Mutation::Type::REMOVE) return entry->index->remove(mutation.key);
  if (mutation.type != Mutation::Type::PUT) return true;
  const auto& schema = entry->definition.get_depend_schema();
  auto reader = schema.getType() == nebula::cpp2::SchemaID::Type::edge_type
                    ? RowReaderWrapper::getEdgePropReader(
                          schemas_, space, schema.get_edge_type(), mutation.value)
                    : RowReaderWrapper::getTagPropReader(
                          schemas_, space, schema.get_tag_id(), mutation.value);
  if (!reader) return false;
  std::vector<std::string> fields;
  for (const auto& field : entry->definition.get_fields()) {
    auto value = reader->getValueByName(field);
    // Null is a candidate only for an empty query; the original predicate
    // evaluated against RocksDB is authoritative and removes that candidate.
    fields.emplace_back(value.isStr() ? value.getStr() : "");
  }
  return entry->index->upsert(mutation.key, fields);
}

bool TantivyManager::build(const std::string& name, const std::shared_ptr<Entry>& entry) {
  std::lock_guard<std::mutex> lifecycle(entry->lifecycleMutex);
  entry->ready = false;
  entry->documents = 0;
  auto space = entry->definition.get_space_id();
  if (!entry->error.empty()) {
    // Explicit rebuild also recovers a failed open/writer. Replace only derived
    // index files; authoritative RocksDB remains untouched.
    auto path = root_ + "/" + std::to_string(space) + "/" + name;
    entry->index.reset();
    boost::filesystem::remove_all(path);
    boost::filesystem::create_directories(path);
    entry->index = std::make_unique<plugin::TantivyIndex>();
    auto analyzer = entry->definition.get_analyzer();
    if (analyzer.empty()) analyzer = "standard";
    if (!entry->index->open(
            path, entry->definition.get_fields(), true, FLAGS_tantivy_writer_bytes, analyzer)) {
      fail(entry, entry->index->error());
      return false;
    }
  }
  auto count = schemas_->getPartsNum(space);
  auto len = schemas_->getSpaceVidLen(space);
  if (!count.ok() || !len.ok()) {
    fail(entry, "Space schema unavailable");
    return false;
  }
  if (!entry->index->clear()) {
    fail(entry, entry->index->error());
    return false;
  }
  bool edge =
      entry->definition.get_depend_schema().getType() == nebula::cpp2::SchemaID::Type::edge_type;
  for (PartitionID partId = 1; partId <= count.value(); ++partId) {
    if (stopping_) return false;
    auto part = store_->part(space, partId);
    if (!ok(part)) continue;
    std::unique_ptr<kvstore::KVIterator> iter;
    auto prefix = edge ? NebulaKeyUtils::edgePrefix(partId) : NebulaKeyUtils::tagPrefix(partId);
    auto code = value(part)->engine()->prefix(prefix, &iter);
    if (code != Code::SUCCEEDED) {
      fail(entry, "Primary data scan failed");
      return false;
    }
    while (iter->valid()) {
      if (stopping_) return false;
      if (matches(*entry, iter->key(), len.value())) {
        Mutation m{Mutation::Type::PUT, iter->key().str(), iter->val().str()};
        if (!apply(entry, m)) {
          fail(entry, entry->index->error());
          return false;
        }
        ++entry->documents;
        // Bound pending delete terms and segment state during large backfills.
        // Queries still see ready=false until the complete backfill finishes.
        if (entry->documents % 100000 == 0) {
          if (!entry->index->commit()) {
            fail(entry, entry->index->error());
            return false;
          }
          LOG(INFO) << "Tantivy backfill name=" << name << " documents=" << entry->documents;
        }
      }
      iter->next();
    }
  }
  if (!entry->index->commit()) {
    fail(entry, entry->index->error());
    return false;
  }
  entry->error.clear();
  entry->ready = true;
  LOG(INFO) << "Tantivy index ready name=" << name << " space=" << space
            << " documents=" << entry->documents;
  wake_.notify_all();
  return true;
}

void TantivyManager::refreshDefinitions() {
  auto definitions = meta_->getFTIndexesFromCache();
  if (!definitions.ok()) return;
  interested_ = !definitions.value().empty();
  for (const auto& item : definitions.value()) {
    std::shared_ptr<Entry> entry;
    {
      std::lock_guard<std::mutex> guard(entriesMutex_);
      auto it = entries_.find(item.first);
      if (it != entries_.end()) {
        if (it->second->definition == item.second) continue;
        auto previous = it->second;
        std::lock_guard<std::mutex> lifecycle(previous->lifecycleMutex);
        previous->ready = false;
        auto oldPath =
            root_ + "/" + std::to_string(previous->definition.get_space_id()) + "/" + item.first;
        previous->index.reset();
        entries_.erase(it);
        boost::filesystem::remove_all(oldPath);
      }
      entry = std::make_shared<Entry>();
      entry->definition = item.second;
      entry->index = std::make_unique<plugin::TantivyIndex>();
      entries_[item.first] = entry;
    }
    auto path = root_ + "/" + std::to_string(item.second.get_space_id()) + "/" + item.first;
    boost::filesystem::create_directories(path);
    auto analyzer = item.second.get_analyzer();
    if (analyzer.empty()) analyzer = "standard";
    if (!entry->index->open(
            path, item.second.get_fields(), true, FLAGS_tantivy_writer_bytes, analyzer)) {
      fail(entry, entry->index->error());
      continue;
    }
    // Prototype recovery deliberately rebuilds from authoritative RocksDB.
    // No persisted index is declared ready without validating its contents.
    build(item.first, entry);
  }
  std::lock_guard<std::mutex> guard(entriesMutex_);
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (definitions.value().count(it->first) == 0) {
      auto previous = it->second;
      std::lock_guard<std::mutex> lifecycle(previous->lifecycleMutex);
      previous->ready = false;
      auto path =
          root_ + "/" + std::to_string(previous->definition.get_space_id()) + "/" + it->first;
      // Close the old writer before reusing its directory, even if a query still
      // owns an Entry reference. That query observes ready=false under this lock.
      previous->index.reset();
      it = entries_.erase(it);
      boost::filesystem::remove_all(path);
    } else {
      ++it;
    }
  }
}

void TantivyManager::run() {
  while (!stopping_) {
    try {
      refreshDefinitions();
      std::deque<Event> batch;
      {
        std::unique_lock<std::mutex> guard(queueMutex_);
        wake_.wait_for(
            guard, std::chrono::milliseconds(200), [&] { return stopping_ || !queue_.empty(); });
        batch.swap(queue_);
        queuedBytes_ = 0;
        wake_.notify_all();
      }
      std::unordered_map<std::string, std::shared_ptr<Entry>> entries;
      {
        std::lock_guard<std::mutex> guard(entriesMutex_);
        entries = entries_;
      }
      std::unordered_set<std::string> touched;
      std::unordered_set<std::string> rebuilds;
      for (const auto& event : batch) {
        for (const auto& pair : entries) {
          const auto& entry = pair.second;
          if (entry->definition.get_space_id() != event.space) continue;
          bool rescan = event.rebuild;
          for (const auto& mutation : event.mutations) {
            if (mutation.type == Mutation::Type::REMOVE_RANGE) rescan = true;
          }
          if (rescan) {
            rebuilds.insert(pair.first);
            continue;
          }
          if (rebuilds.count(pair.first)) continue;
          if (!entry->ready) continue;
          for (const auto& mutation : event.mutations) {
            auto len = schemas_->getSpaceVidLen(event.space);
            if (!len.ok() || !matches(*entry, mutation.key, len.value())) continue;
            // A rebuild may have scanned a newer primary value than this queued
            // event. Read current RocksDB state so replay cannot resurrect an
            // older value or erase a later reinsertion.
            auto current = mutation;
            auto part = store_->part(event.space, event.part);
            if (!ok(part)) {
              fail(entry, "Partition unavailable during index update");
              break;
            }
            auto code = value(part)->engine()->get(mutation.key, &current.value);
            if (code == Code::SUCCEEDED) {
              current.type = Mutation::Type::PUT;
            } else if (code == Code::E_KEY_NOT_FOUND) {
              current.type = Mutation::Type::REMOVE;
            } else {
              fail(entry, "Primary data read failed during index update");
              break;
            }
            if (!apply(entry, current)) {
              fail(entry, entry->index->error());
              break;
            }
          }
          touched.insert(pair.first);
        }
      }
      for (const auto& name : touched) {
        auto entry = entries.at(name);
        if (!rebuilds.count(name) && entry->ready && !entry->index->commit())
          fail(entry, entry->index->error());
      }
      for (const auto& name : rebuilds) build(name, entries.at(name));
      if (!batch.empty()) {
        std::lock_guard<std::mutex> guard(queueMutex_);
        committed_ = batch.back().sequence;
        wake_.notify_all();
      }
    } catch (const std::exception& e) {
      LOG(ERROR) << "Tantivy worker exception: " << e.what();
      {
        std::lock_guard<std::mutex> guard(entriesMutex_);
        for (auto& pair : entries_) fail(pair.second, e.what());
      }
      // An exception may have interrupted a batch removed from the queue. Do not
      // publish a false committed watermark or leave its waiters blocked.
      stopping_ = true;
      wake_.notify_all();
      LOG(ERROR) << "Tantivy worker stopped after an unrecoverable exception; "
                    "restart this storaged to rebuild indexes from RocksDB";
    }
  }
}

bool TantivyManager::waitFor(uint64_t sequence, std::chrono::seconds timeout) {
  std::unique_lock<std::mutex> guard(queueMutex_);
  return wake_.wait_for(guard, timeout, [&] { return stopping_ || committed_ >= sequence; }) &&
         !stopping_;
}
Code TantivyManager::search(GraphSpaceID space,
                            PartitionID part,
                            const std::string& name,
                            const std::string& field,
                            const std::string& text,
                            int32_t mode,
                            std::vector<plugin::TantivyIndex::Hit>& hits,
                            size_t limit) {
  if (mode < 0 || mode > 3) return Code::E_INVALID_PARM;
  uint64_t sequence;
  {
    std::lock_guard<std::mutex> guard(queueMutex_);
    sequence = submitted_;
  }
  if (!waitFor(sequence, std::chrono::seconds(FLAGS_tantivy_query_wait_seconds)))
    return Code::E_REBUILD_INDEX_FAILED;
  std::shared_ptr<Entry> entry;
  {
    std::lock_guard<std::mutex> guard(entriesMutex_);
    auto it = entries_.find(name);
    if (it == entries_.end() || it->second->definition.get_space_id() != space)
      return Code::E_INDEX_NOT_FOUND;
    entry = it->second;
  }
  std::lock_guard<std::mutex> lifecycle(entry->lifecycleMutex);
  if (!entry->ready) return Code::E_REBUILD_INDEX_FAILED;
  bool edge =
      entry->definition.get_depend_schema().getType() == nebula::cpp2::SchemaID::Type::edge_type;
  auto prefix = edge ? NebulaKeyUtils::edgePrefix(part) : NebulaKeyUtils::tagPrefix(part);
  if (!entry->index->search(
          field, text, static_cast<plugin::TantivyIndex::SearchMode>(mode), hits, limit, prefix)) {
    LOG(ERROR) << "Tantivy search failed: " << entry->index->error();
    return Code::E_INVALID_PARM;
  }
  return Code::SUCCEEDED;
}
Code TantivyManager::rebuild(GraphSpaceID space) {
  using Clock = std::chrono::steady_clock;
  using Definitions = std::unordered_map<std::string, meta::cpp2::FTIndex>;
  const auto deadline = Clock::now() + std::chrono::seconds(3600);
  auto pause = [&] {
    std::unique_lock<std::mutex> guard(queueMutex_);
    wake_.wait_until(guard,
                     std::min(deadline, Clock::now() + std::chrono::milliseconds(200)),
                     [&] { return stopping_.load(); });
  };
  auto definitions = [&]() -> StatusOr<Definitions> {
    try {
      if (stopping_ || Clock::now() >= deadline) {
        return Status::Error("Stopped or timed out reading fulltext index definitions");
      }
      auto future = meta_->listFTIndexes();
      while (!future.isReady()) {
        if (stopping_ || Clock::now() >= deadline) {
          return Status::Error("Stopped or timed out reading fulltext index definitions");
        }
        pause();
      }
      if (stopping_ || Clock::now() >= deadline) {
        return Status::Error("Stopped or timed out reading fulltext index definitions");
      }
      return std::move(future).get();
    } catch (const std::exception& error) {
      return Status::Error("Reading fulltext index definitions failed: %s", error.what());
    }
  };
  if (stopping_) return Code::E_REBUILD_INDEX_FAILED;
  // CREATE may already be committed in Meta while the local cache and entries
  // are still empty. An empty local collection must not prove rebuild success.
  auto snapshot = definitions();
  if (!snapshot.ok()) {
    LOG(ERROR) << "Tantivy rebuild metadata lookup failed: " << snapshot.status();
    return Code::E_REBUILD_INDEX_FAILED;
  }
  Definitions expected;
  for (const auto& item : snapshot.value()) {
    if (item.second.get_space_id() == space) expected.emplace(item);
  }
  auto enqueueAndWait = [&](bool rebuild) {
    std::unique_lock<std::mutex> guard(queueMutex_);
    if (stopping_ || Clock::now() >= deadline) return false;
    Event event;
    event.space = space;
    event.part = 0;
    event.rebuild = rebuild;
    const auto sequence = event.sequence = ++submitted_;
    queue_.push_back(std::move(event));
    wake_.notify_all();
    return wake_.wait_until(guard, deadline, [&] { return stopping_ || committed_ >= sequence; }) &&
           !stopping_;
  };
  if (!enqueueAndWait(true)) return Code::E_REBUILD_INDEX_FAILED;
  // CLEAR SPACE with no authoritative fulltext definitions remains valid.
  if (expected.empty()) return Code::SUCCEEDED;

  auto nextMetadataCheck = Clock::now();
  while (!stopping_ && Clock::now() < deadline) {
    std::unordered_map<std::string, std::shared_ptr<Entry>> observed;
    bool allReady = true;
    bool observedNotReady = false;
    {
      std::lock_guard<std::mutex> guard(entriesMutex_);
      for (const auto& item : expected) {
        auto it = entries_.find(item.first);
        if (it == entries_.end() || !(it->second->definition == item.second)) {
          allReady = false;
          continue;
        }
        observed.emplace(it->first, it->second);
        if (!it->second->ready) {
          allReady = false;
          observedNotReady = true;
        }
      }
    }
    // Check outside all manager locks, including immediately before success.
    // A concurrent DROP or replacement invalidates this rebuild's snapshot.
    if (allReady || Clock::now() >= nextMetadataCheck) {
      auto current = definitions();
      if (!current.ok()) return Code::E_REBUILD_INDEX_FAILED;
      for (const auto& item : expected) {
        auto it = current.value().find(item.first);
        if (it == current.value().end() || !(it->second == item.second)) {
          LOG(ERROR) << "Tantivy rebuild definition changed: " << item.first;
          return Code::E_REBUILD_INDEX_FAILED;
        }
      }
      nextMetadataCheck = Clock::now() + std::chrono::seconds(1);
    }
    if (allReady) {
      std::lock_guard<std::mutex> guard(entriesMutex_);
      for (const auto& item : observed) {
        auto it = entries_.find(item.first);
        if (it == entries_.end() || it->second != item.second || !it->second->ready) {
          return Code::E_REBUILD_INDEX_FAILED;
        }
      }
      return stopping_ ? Code::E_REBUILD_INDEX_FAILED : Code::SUCCEEDED;
    }
    if (observedNotReady) {
      // A new entry is published before open/build completes. Let the single
      // worker pass a queue barrier before distinguishing in-progress work from
      // failure. Do not read Entry::error, which is not synchronized for readers.
      if (!enqueueAndWait(false)) return Code::E_REBUILD_INDEX_FAILED;
      std::lock_guard<std::mutex> guard(entriesMutex_);
      for (const auto& item : observed) {
        auto it = entries_.find(item.first);
        if (it == entries_.end() || it->second != item.second || !it->second->ready) {
          LOG(ERROR) << "Tantivy rebuild did not produce a ready index: " << item.first;
          return Code::E_REBUILD_INDEX_FAILED;
        }
      }
      continue;
    }
    pause();
  }
  LOG(ERROR) << "Tantivy rebuild stopped or timed out waiting for definitions in space " << space;
  return Code::E_REBUILD_INDEX_FAILED;
}
}  // namespace nebula::storage
