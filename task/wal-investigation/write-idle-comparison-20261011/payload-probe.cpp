// Copyright (c) 2026 cccxgit. All rights reserved.
//
// This source code is licensed under Apache 2.0 License.

// Observation driver only: links the pinned, unmodified AtomicLogBuffer implementation.
// Single writer; each read completes before the next push or observation.
#include <folly/lang/Aligned.h>
#include <gtest/gtest_prod.h>
#include "common/thrift/ThriftTypes.h"
#include "common/utils/LogIterator.h"
#define private public
#include "kvstore/wal/AtomicLogBuffer.h"
#undef private
#include <cstdint>
#include <fstream>
#include <iostream>
#include <malloc.h>
#include <sstream>
#include <string>

using Buffer = nebula::wal::AtomicLogBuffer;

int64_t rssKiB() {
  std::ifstream stream("/proc/self/status");
  std::string line;
  while (std::getline(stream, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      std::istringstream value(line.substr(6));
      int64_t rss = 0;
      value >> rss;
      return rss;
    }
  }
  return -1;
}

void sample(const std::string& phase, int64_t pushed, const Buffer& buffer) {
  uint64_t nodes = 0, dirty = 0, records = 0, nonempty = 0;
  uint64_t nodeUsable = 0, payloadUsable = 0, payloadBytes = 0, activeLogical = 0;
  for (auto* node = buffer.head_.load(); node; node = node->next_) {
    ++nodes;
    dirty += node->markDeleted_.load();
    nodeUsable += malloc_usable_size(node);
    if (!node->markDeleted_.load()) activeLogical += node->size_;
    for (int32_t i = 0; i < node->pos_.load(); ++i) {
      const auto& msg = (*node->records_)[i].msg_;
      ++records;
      nonempty += !msg.empty();
      payloadBytes += msg.size();
      // libstdc++ short strings store data inside the string object. Only ask
      // the allocator about strings whose storage is outside that object.
      auto data = reinterpret_cast<uintptr_t>(msg.data());
      auto object = reinterpret_cast<uintptr_t>(&msg);
      if (data < object || data >= object + sizeof(msg)) {
        payloadUsable += malloc_usable_size(const_cast<char*>(msg.data()));
      }
    }
  }
  CHECK_EQ(dirty, static_cast<uint64_t>(buffer.dirtyNodes_.load()));
  CHECK_EQ(activeLogical, static_cast<uint64_t>(buffer.size_.load()));
  CHECK_EQ(buffer.refs_.load(), 0);
  std::cout << phase << ',' << pushed << ',' << buffer.size_.load() << ',' << nodes << ',' << dirty
            << ',' << records << ',' << nonempty << ',' << payloadBytes << ',' << nodeUsable << ','
            << payloadUsable << ',' << nodeUsable + payloadUsable << ',' << rssKiB() << '\n';
}

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  if (argc != 4) {
    std::cerr << "usage: payload-probe PAYLOAD_BYTES WARMUP_RECORDS MILESTONES\n"
              << "WARMUP_RECORDS=0 keeps the payload; otherwise warm up then append empty logs.\n";
    return 2;
  }
  const auto payloadSize = std::stoul(argv[1]);
  const auto warmup = std::stoll(argv[2]);
  if (payloadSize > 4096 || warmup < 0 || warmup > 1000000) return 2;
  constexpr int32_t capacity = 8388608;
  auto buffer = Buffer::instance(capacity);
  std::string payload(payloadSize, 'x');
  int64_t pushed = 0;
  auto append = [&](folly::StringPiece message) {
    buffer->push(++pushed, 1, 1, message);
    auto reader = buffer->iterator(pushed, pushed);
    CHECK(reader->valid());
    CHECK_EQ(reader->logId(), pushed);
    CHECK(reader->logMsg() == message);
  };
  std::cerr << "capacity=" << capacity << " payload_bytes=" << payloadSize
            << " warmup_records=" << warmup << " sizeof_record=" << sizeof(nebula::wal::Record)
            << " sizeof_node=" << sizeof(nebula::wal::Node)
            << " reader_mode=release_after_each_push\n";
  std::cout
      << "phase,total_pushed,active_logical_bytes,retained_nodes,dirty_nodes,retained_records,"
         "retained_nonempty_records,retained_payload_bytes,node_usable_bytes,"
         "payload_usable_bytes,node_and_payload_usable_bytes,rss_kib\n";
  sample("initial", pushed, *buffer);
  for (int64_t i = 0; i < warmup; ++i) append(payload);
  if (warmup) sample("warmup_done", pushed, *buffer);
  std::istringstream steps(argv[3]);
  std::string token;
  int64_t previous = 0;
  while (std::getline(steps, token, ',')) {
    auto target = std::stoll(token);
    CHECK_GE(target, previous);
    CHECK_LE(target, 2000000);
    while (pushed < warmup + target) append(warmup ? folly::StringPiece("") : payload);
    sample(warmup ? "after_stop_empty" : "constant_payload", pushed, *buffer);
    previous = target;
  }
  buffer.reset();
  std::cerr << "verified: payload roundtrip, logical/node/dirty counters, no retained readers; "
            << "RSS_after_buffer_destroy_KiB=" << rssKiB() << '\n';
  google::ShutdownGoogleLogging();
}
