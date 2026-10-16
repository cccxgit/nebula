// Copyright (c) 2026 cccxgit. All rights reserved.
//
// This source code is licensed under Apache 2.0 License.

// Diagnostic-only executable. Uses the repository's unmodified AtomicLogBuffer.cpp.
// Dependencies are included before private is exposed, so the macro affects only
// AtomicLogBuffer's access control in this translation unit, never its layout.
#include <folly/lang/Aligned.h>
#include <gtest/gtest_prod.h>
#include "common/thrift/ThriftTypes.h"
#include "common/utils/LogIterator.h"
#define private public
#include "kvstore/wal/AtomicLogBuffer.h"
#undef private
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <malloc.h>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using Buffer = nebula::wal::AtomicLogBuffer;
static int64_t statusKB(const std::string& key) {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(key + ":", 0) == 0) {
      std::istringstream v(line.substr(key.size() + 1));
      int64_t n = 0;
      v >> n;
      return n;
    }
  }
  return -1;
}
static void snapshot(const std::string& phase,
                     size_t rounds,
                     const std::vector<std::shared_ptr<Buffer>>& buffers) {
  uint64_t nodes = 0, dirty = 0, refs = 0, logical = 0, usable = 0;
  for (const auto& b : buffers) {
    logical += b->size_.load();
    dirty += b->dirtyNodes_.load();
    refs += b->refs_.load();
    for (auto* p = b->head_.load(); p; p = p->next_) {
      ++nodes;
      usable += malloc_usable_size(p);
    }
  }
  std::cout << phase << ',' << buffers.size() << ',' << rounds << ',' << logical << ',' << nodes
            << ',' << dirty << ',' << refs << ',' << nodes * sizeof(nebula::wal::Node) << ','
            << usable << ',' << statusKB("VmRSS") << ',' << statusKB("VmHWM") << std::endl;
}
int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  if (argc != 5) {
    std::cerr << "usage: probe PARTITIONS CAPACITY each|none|held ROUND,ROUND,...\n";
    return 2;
  }
  const int partitions = std::stoi(argv[1]);
  const int capacity = std::stoi(argv[2]);
  const std::string mode = argv[3];
  std::cerr << "sizeof_Record=" << sizeof(nebula::wal::Record)
            << " sizeof_Node=" << sizeof(nebula::wal::Node)
            << " alignof_Node=" << alignof(nebula::wal::Node)
            << " sizeof_string=" << sizeof(std::string)
            << " empty_accounted=" << nebula::wal::Record(1, 1, "").size()
            << " capacity=" << capacity << " mode=" << mode << '\n';
  std::vector<std::shared_ptr<Buffer>> buffers;
  std::vector<std::unique_ptr<Buffer::Iterator>> held;
  for (int i = 0; i < partitions; ++i) buffers.emplace_back(Buffer::instance(capacity));
  std::cout << "phase,partitions,records_per_partition,logical_bytes,nodes,dirty_nodes,refs,node_"
               "struct_bytes,node_malloc_usable_bytes,rss_kib,hwm_kib\n";
  std::stringstream milestones(argv[4]);
  std::string value;
  size_t current = 0;
  while (std::getline(milestones, value, ',')) {
    const size_t end = std::stoull(value);
    while (current < end) {
      ++current;
      for (const auto& b : buffers) {
        b->push(current, 1, 1, "");
        if (mode == "held" && current == 1) held.emplace_back(b->iterator(1, 1));
        if (mode != "none") {
          auto reader = b->iterator(current, current);
          if (!reader->valid() || reader->logId() != static_cast<int64_t>(current) ||
              !reader->logMsg().empty())
            return 3;
        }
      }
    }
    snapshot("push", current, buffers);
  }
  if (mode == "held") {
    held.clear();
    snapshot("release_held", current, buffers);
  } else if (mode == "none") {
    for (const auto& b : buffers) {
      auto reader = b->iterator(current, current);
    }
    snapshot("release_one_per_buffer", current, buffers);
  }
  buffers.clear();
  snapshot("destroy_all", current, buffers);
  google::ShutdownGoogleLogging();
}
