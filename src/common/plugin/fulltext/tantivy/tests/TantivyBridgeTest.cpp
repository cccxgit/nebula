/* Copyright (c) 2026. Licensed under Apache 2.0. */
/* Real disk/FFI integration test; independent of Nebula's test dependencies. */
#include "common/plugin/fulltext/tantivy/TantivyIndex.h"

#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
}  // namespace

int main() {
  using Index = nebula::plugin::TantivyIndex;
  char temporary[] = "/tmp/nebula-tantivy-test-XXXXXX";
  const char* path = mkdtemp(temporary);
  require(path != nullptr, "mkdtemp failed");
  const std::string key("\x01\0\x02\0vertex\0", 11);
  std::vector<Index::Hit> hits;
  try {
    {
      Index index;
      require(index.open(path, {"title", "body"}), index.error());
      require(index.upsert(key, {"Hello Graph", "中文图数据库与 Tantivy"}), index.error());
      require(index.commit(), index.error());
      require(index.search("body",
                           "图数据库",
                           Index::SearchMode::Contains,
                           hits,
                           0,
                           key.substr(0, 4)),
              index.error());
      require(hits.size() == 1 && hits.front().key == key, "binary key or Chinese search mismatch");
      require(index.search("title", "hello", Index::SearchMode::Contains, hits), index.error());
      require(hits.empty(), "case-sensitive substring search returned a false match");
      require(index.search("title", "hello AND graph", Index::SearchMode::Query, hits),
              index.error());
      require(hits.size() == 1 && hits.front().score > 0, "fulltext BM25 search failed");
      require(index.setWritable(false), index.error());
      require(!index.remove(key), "read-only writer accepted a delete");
      require(index.setWritable(true), index.error());
      require(index.upsert(key, {"Updated", "更新后的中文"}), index.error());
      require(index.commit(), index.error());
    }
    {
      Index index;
      require(index.open(path, {"title", "body"}), index.error());
      require(index.search("title", "Updated", Index::SearchMode::StartsWith, hits), index.error());
      require(hits.size() == 1, "persisted update missing after reopen");
      require(index.remove(key), index.error());
      require(index.commit(), index.error());
      require(index.search("title", "", Index::SearchMode::Contains, hits), index.error());
      require(hits.empty(), "deleted key is still searchable");
      require(!index.search("missing", "x", Index::SearchMode::Contains, hits),
              "unknown field silently accepted");
      require(!index.error().empty(), "FFI error detail missing");
    }
    std::cout << "PASS: C++/Rust FFI, Chinese and English, binary keys, partitions, "
                 "case sensitivity, BM25, persistence, update/delete, read-only transitions\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  // The path is created by mkdtemp and contains only this test's generated index.
  const std::string cleanup = std::string("rm -rf -- ") + path;
  return std::system(cleanup.c_str()) == 0 ? 0 : 1;
}
