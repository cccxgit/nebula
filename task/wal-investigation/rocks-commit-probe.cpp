// Copyright (c) 2026 cccxgit. All rights reserved.
//
// This source code is licensed under Apache 2.0 License.

// Isolated RocksDB mechanism probe. This is NOT a Nebula service replacement.
// Uses the same 8-byte systemCommitKey and 16-byte commitMsg representation as
// NebulaKeyUtils.cpp:101-108 and Part.cpp:403-410 on this little-endian host.
#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/version.h>
#include <rocksdb/write_batch.h>
#include <jemalloc/jemalloc.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using DBs = std::vector<std::unique_ptr<rocksdb::DB>>;

void checked(const rocksdb::Status& s) {
  if (!s.ok()) throw std::runtime_error(s.ToString());
}

std::string commitKey(int32_t part) {
  // Types.h kPartitionOffset=8, kSystem=4, kSystemCommit=1.
  int32_t item = (part << 8) | 4;
  uint32_t type = 1;
  std::string result;
  result.append(reinterpret_cast<const char*>(&item), sizeof(item));
  result.append(reinterpret_cast<const char*>(&type), sizeof(type));
  return result;
}

std::string commitValue(int64_t logId, int64_t term) {
  std::string result;
  result.reserve(16);
  result.append(reinterpret_cast<const char*>(&logId), sizeof(logId));
  result.append(reinterpret_cast<const char*>(&term), sizeof(term));
  return result;
}

uint64_t sumProperty(const DBs& dbs, const std::string& name) {
  uint64_t total = 0;
  for (const auto& db : dbs) {
    uint64_t value = 0;
    if (!db->GetIntProperty(name, &value)) throw std::runtime_error("Missing property " + name);
    total += value;
  }
  return total;
}

uint64_t rssKiB() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("VmRSS:", 0) == 0) return std::stoull(line.substr(6));
  }
  return 0;
}

size_t allocatorAllocated() {
  uint64_t epoch = 1;
  size_t epochLen = sizeof(epoch);
  if (mallctl("epoch", &epoch, &epochLen, &epoch, sizeof(epoch)))
    throw std::runtime_error("mallctl epoch");
  size_t value = 0, len = sizeof(value);
  if (mallctl("stats.allocated", &value, &len, nullptr, 0))
    throw std::runtime_error("mallctl allocated");
  return value;
}

void sample(const DBs& dbs,
            const std::shared_ptr<rocksdb::Cache>& cache,
            const char* phase,
            int round,
            int parts,
            std::chrono::steady_clock::time_point start) {
  std::cout << phase << ',' << round << ',' << (uint64_t(round) * parts * dbs.size()) << ','
            << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
            << ',' << rssKiB() << ',' << allocatorAllocated() << ','
            << sumProperty(dbs, "rocksdb.cur-size-active-mem-table") << ','
            << sumProperty(dbs, "rocksdb.cur-size-all-mem-tables") << ','
            << sumProperty(dbs, "rocksdb.size-all-mem-tables") << ','
            << sumProperty(dbs, "rocksdb.num-entries-active-mem-table") << ','
            << sumProperty(dbs, "rocksdb.num-entries-imm-mem-tables") << ','
            << sumProperty(dbs, "rocksdb.estimate-table-readers-mem") << ',' << cache->GetUsage()
            << std::endl;
}

int main(int argc, char** argv) {
  try {
    if (argc != 5)
      throw std::runtime_error(
          "Usage: rocks-commit-probe NEW_DIRECTORY DB_COUNT PARTS_PER_DB ROUNDS");
    const std::filesystem::path root(argv[1]);
    const int count = std::stoi(argv[2]), parts = std::stoi(argv[3]), rounds = std::stoi(argv[4]);
    if (count <= 0 || count > 50 || parts <= 0 || rounds <= 0)
      throw std::runtime_error("Invalid dimensions");
    if (std::filesystem::exists(root)) throw std::runtime_error("Refuse to reuse data directory");
    std::filesystem::create_directories(root);
    uint16_t endian = 1;
    if (*reinterpret_cast<char*>(&endian) != 1)
      throw std::runtime_error("Probe expects little-endian host");
    std::cerr << "RocksDB " << ROCKSDB_MAJOR << '.' << ROCKSDB_MINOR << '.' << ROCKSDB_PATCH
              << " dbs=" << count << " parts_per_db=" << parts << " rounds=" << rounds
              << " key_bytes=" << commitKey(1).size() << " value_bytes=" << commitValue(1, 1).size()
              << " write_buffer_size=67108864 max_write_buffer_number=4"
              << " inplace_update_support=false disableWAL=false sync=false allocator=jemalloc\n";
    std::cerr << "key(part1)_hex=0401000001000000 "
                 "value(log1,term1)_hex=01000000000000000100000000000000\n";
    auto cache = rocksdb::NewLRUCache(4 * 1024 * 1024, 8);
    rocksdb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 64 * 1024 * 1024;
    options.max_write_buffer_number = 4;
    options.max_bytes_for_level_base = 256 * 1024 * 1024;
    options.compression = rocksdb::kSnappyCompression;
    options.row_cache = rocksdb::NewLRUCache(16000000, 8);
    rocksdb::BlockBasedTableOptions table;
    table.block_cache = cache;
    table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
    DBs dbs;
    for (int i = 0; i < count; ++i) {
      rocksdb::DB* db = nullptr;
      checked(rocksdb::DB::Open(options, (root / ("db" + std::to_string(i))).string(), &db));
      dbs.emplace_back(db);
    }
    rocksdb::WriteOptions write;
    write.disableWAL = false;
    write.sync = false;
    write.no_slowdown = false;
    const auto start = std::chrono::steady_clock::now();
    std::cout << "phase,round,writes,elapsed_seconds,rss_kib,jemalloc_allocated_bytes,active_"
                 "memtable_bytes,all_memtable_bytes,all_memtable_with_pinned_bytes,active_memtable_"
                 "entries,immutable_memtable_entries,table_reader_bytes,shared_block_cache_bytes\n";
    sample(dbs, cache, "opened", 0, parts, start);
    for (int round = 1; round <= rounds; ++round) {
      for (const auto& db : dbs) {
        for (int part = 1; part <= parts; ++part) {
          // One WriteBatch per committed partition heartbeat, as Part::commitLogs.
          rocksdb::WriteBatch batch(4096);
          checked(batch.Put(commitKey(part), commitValue(round, 1)));
          checked(db->Write(write, &batch));
        }
      }
      if (round % 1000 == 0 || round == rounds) sample(dbs, cache, "writing", round, parts, start);
    }
    rocksdb::FlushOptions flush;
    flush.wait = true;
    for (const auto& db : dbs) checked(db->Flush(flush));
    sample(dbs, cache, "flushed", rounds, parts, start);
    uint64_t keys = 0;
    for (const auto& db : dbs) {
      std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(rocksdb::ReadOptions()));
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        ++keys;
        if (it->value().ToString() != commitValue(rounds, 1))
          throw std::runtime_error("Unexpected committed value");
      }
      checked(it->status());
    }
    if (keys != uint64_t(count) * parts) throw std::runtime_error("Unexpected logical key count");
    std::cerr << "verified_logical_keys_after_flush=" << keys << " latest_value_matches=true\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
