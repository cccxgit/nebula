/* Copyright (c) 2026. Licensed under Apache 2.0. */
#ifndef STORAGE_FULLTEXT_TANTIVYMANAGER_H_
#define STORAGE_FULLTEXT_TANTIVYMANAGER_H_

#include <condition_variable>
#include <deque>
#include <thread>

#include "clients/meta/MetaClient.h"
#include "common/plugin/fulltext/tantivy/TantivyIndex.h"
#include "kvstore/KVStore.h"

namespace nebula::storage {

// A single bounded worker per storaged. Indexes are per (space, definition),
// with partition prefixes inside the index, rather than a writer per replica.
class TantivyManager final {
 public:
  TantivyManager(meta::MetaClient* meta, meta::SchemaManager* schemas, std::string root);
  ~TantivyManager();
  void start(kvstore::KVStore* store);
  void stop();
  void enqueue(GraphSpaceID space,
               PartitionID part,
               std::vector<kvstore::CommittedMutation>&& mutations);
  nebula::cpp2::ErrorCode search(GraphSpaceID space,
                                 PartitionID part,
                                 const std::string& name,
                                 const std::string& field,
                                 const std::string& text,
                                 int32_t mode,
                                 std::vector<plugin::TantivyIndex::Hit>& hits,
                                 size_t limit = 0);
  nebula::cpp2::ErrorCode rebuild(GraphSpaceID space);

 private:
  struct Entry {
    meta::cpp2::FTIndex definition;
    std::unique_ptr<plugin::TantivyIndex> index;
    // A query must not cross a clear/rebuild or a close/remove/recreate boundary.
    std::mutex lifecycleMutex;
    std::atomic<bool> ready{false};
    std::string error;
    uint64_t documents{0};
  };
  struct Event {
    GraphSpaceID space;
    PartitionID part;
    uint64_t sequence{0};
    size_t bytes{0};
    bool rebuild{false};
    std::vector<kvstore::CommittedMutation> mutations;
  };
  void run();
  void refreshDefinitions();
  bool build(const std::string& name, const std::shared_ptr<Entry>& entry);
  bool apply(const std::shared_ptr<Entry>& entry, const kvstore::CommittedMutation& mutation);
  bool matches(const Entry& entry, folly::StringPiece key, size_t vidLen) const;
  void fail(const std::shared_ptr<Entry>& entry, const std::string& reason);
  bool waitFor(uint64_t sequence, std::chrono::seconds timeout);
  meta::MetaClient* meta_;
  meta::SchemaManager* schemas_;
  kvstore::KVStore* store_{nullptr};
  std::string root_;
  std::thread worker_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> interested_{false};
  std::mutex queueMutex_;
  std::condition_variable wake_;
  std::deque<Event> queue_;
  size_t queuedBytes_{0};
  uint64_t submitted_{0};
  uint64_t committed_{0};
  std::mutex entriesMutex_;
  std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
};
}  // namespace nebula::storage
#endif
