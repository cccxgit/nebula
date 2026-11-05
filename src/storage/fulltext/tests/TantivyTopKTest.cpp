/* Copyright (c) 2026. Licensed under Apache 2.0. */
// Standalone regression of the exact preparation helper used by Storage.
// Build with g++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc, this source,
// and -o /tmp/tantivy_topk_test. No Nebula service or third-party library needed.
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>

#include "storage/fulltext/TantivyTopK.h"

using nebula::plugin::TantivyIndex;
using nebula::storage::fulltext::TopKCandidate;
using nebula::storage::fulltext::TopKRead;
using nebula::storage::fulltext::TopKResult;
using nebula::storage::fulltext::decodeCanonicalVid;
using nebula::storage::fulltext::prepareTopK;

namespace {
constexpr size_t kWidth = 24;

void require(bool condition, const std::string& reason) {
  if (!condition) throw std::runtime_error(reason);
}

std::string key(const std::string& vid, uint32_t part = 1) {
  require(vid.size() <= kWidth, "test VID is too long");
  std::string result(sizeof(part), '\0');
  std::memcpy(result.data(), &part, sizeof(part));
  result += vid;
  result.append(kWidth - vid.size(), '\0');
  result.append(4, '\x07');
  return result;
}

std::string decoded(const std::string& key) {
  const auto padded = key.substr(4, kWidth);
  return padded.substr(0, padded.find('\0'));
}

using Ranked = std::pair<double, std::string>;
bool rankedBefore(const Ranked& lhs, const Ranked& rhs) {
  return lhs.first != rhs.first ? lhs.first > rhs.first : lhs.second < rhs.second;
}

struct Fixture {
  std::vector<TantivyIndex::Hit> hits;
  std::unordered_map<std::string, std::string> primary;
  std::vector<size_t> searches;
  std::map<std::string, size_t> reads;
  std::string readError;
  bool searchError{false};

  void add(const std::string& vid, double score = 1.0, bool live = true, uint32_t part = 1) {
    const auto encoded = key(vid, part);
    hits.push_back({encoded, score});
    if (live) primary[encoded] = "primary:" + vid;
  }

  TopKResult run(size_t limit, std::vector<TopKCandidate>& out, bool& fallback) {
    return prepareTopK(
        limit,
        [&](size_t requested, auto& result) {
          searches.push_back(requested);
          if (searchError) return false;
          result = hits;
          // Reproduce the actual Rust collector's raw-key tie order.
          std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            if (a.score == 0.0 && b.score == 0.0 &&
                std::signbit(a.score) != std::signbit(b.score)) {
              return !std::signbit(a.score);  // Rust total_cmp: +0.0 ranks before -0.0.
            }
            return a.score != b.score ? a.score > b.score : a.key < b.key;
          });
          if (requested != 0 && result.size() > requested) result.resize(requested);
          // Full fallback must sort itself, independent of collector order.
          if (requested == 0) std::reverse(result.begin(), result.end());
          return true;
        },
        [&](const auto& encoded, auto& data) {
          ++reads[encoded];
          if (encoded == readError) return TopKRead::Error;
          const auto it = primary.find(encoded);
          if (it == primary.end()) return TopKRead::Missing;
          data = it->second;
          return TopKRead::Found;
        },
        [](const auto& encoded, auto& id, bool& canonical) {
          if (encoded.size() != 8 + kWidth) return false;
          canonical = decodeCanonicalVid(encoded.substr(4, kWidth), id);
          return true;
        },
        out,
        fallback);
  }

  std::vector<Ranked> oracle() const {
    std::vector<Ranked> ranked;
    for (const auto& hit : hits) {
      if (primary.count(hit.key)) ranked.emplace_back(hit.score, decoded(hit.key));
    }
    std::sort(ranked.begin(), ranked.end(), rankedBefore);
    std::unordered_set<std::string> seen;
    ranked.erase(
        std::remove_if(ranked.begin(),
                       ranked.end(),
                       [&](const auto& item) { return !seen.emplace(item.second).second; }),
        ranked.end());
    return ranked;
  }
};

std::vector<Ranked> rank(const std::vector<TopKCandidate>& candidates) {
  std::vector<Ranked> result;
  for (const auto& candidate : candidates) result.emplace_back(candidate.hit.score, candidate.id);
  return result;
}

void verify(Fixture& fixture, size_t limit, bool expectedFallback) {
  std::vector<TopKCandidate> result;
  bool fallback = false;
  require(fixture.run(limit, result, fallback) == TopKResult::Ready, "preparation failed");
  require(fallback == expectedFallback, "unexpected fallback decision");
  auto expected = fixture.oracle();
  if (expected.size() > limit) expected.resize(limit);
  require(rank(result) == expected, "top K differs from full live sorted/deduplicated oracle");
  for (const auto& candidate : result) {
    require(candidate.data == fixture.primary.at(candidate.hit.key),
            "primary value was not cached");
  }
  require(
      fixture.searches == (fallback ? std::vector<size_t>{limit, 0} : std::vector<size_t>{limit}),
      "wrong bounded/fallback search sequence");
}

void canonicalByteOrder() {
  const std::vector<std::string> ids{"",
                                     "a",
                                     "aA",
                                     "aB",
                                     "aa",
                                     "b",
                                     "z",
                                     "\x01",
                                     "\x7f",
                                     "\x80",
                                     "\xff",
                                     std::string("\x80") + "a",
                                     u8"é",
                                     u8"éa",
                                     u8"中文"};
  for (size_t limit = 1; limit <= ids.size(); ++limit) {
    Fixture fixture;
    for (const auto& id : ids) fixture.add(id);
    verify(fixture, limit, false);
    for (const auto& read : fixture.reads) require(read.second == 1, "duplicate primary read");
  }
}

void missingPrimaryFallback() {
  Fixture fixture;
  fixture.add("a", 5, false);
  fixture.add("b", 4);
  fixture.add("c", 3, false);
  fixture.add("d", 2);
  fixture.add("e", 1);
  verify(fixture, 3, true);
}

void signedZeroFallback() {
  Fixture fixture;
  fixture.add("a", -0.0);
  fixture.add("z", +0.0);
  fixture.add("positive", 1.0);
  fixture.add("negative", -1.0);
  // Rust picks positive,z; Graph's numeric tie order requires positive,a.
  verify(fixture, 2, true);
}

void noncanonicalAliases() {
  // Highest live alias score must win, independent of canonical/raw-key order.
  Fixture fixture;
  fixture.add("a", 3);
  fixture.add(std::string("a\0z", 3), 12);
  fixture.add(std::string("a\0y", 3), 10);
  fixture.add("aB", 11);
  fixture.add("b", 9);
  verify(fixture, 3, true);

  Fixture missingAlias;
  missingAlias.add(std::string("a\0z", 3), 9, false);
  missingAlias.add("b", 8);
  missingAlias.add("a", 7);
  missingAlias.add("aB", 6);
  verify(missingAlias, 3, true);

  // An excluded alias cannot improve K distinct canonical results. This is the
  // boundary that permits checking the bounded candidates, not the whole index.
  Fixture excludedAlias;
  excludedAlias.add("a");
  excludedAlias.add(std::string("a\0z", 3));
  excludedAlias.add("aB");
  verify(excludedAlias, 1, false);

  Fixture equalAliases;
  equalAliases.add("a");
  equalAliases.add(std::string("a\0z", 3));
  equalAliases.add("aB");
  equalAliases.add("b");
  verify(equalAliases, 3, true);
}

void crossPartitionPages() {
  std::vector<Fixture> partitions(20);
  std::vector<Ranked> full;
  const std::vector<std::string> suffixes{"", "A", "B", "aa", "\x7f", "\x80", "\xff", u8"中"};
  for (size_t part = 0; part < partitions.size(); ++part) {
    for (int number = 0; number < 10; ++number) {
      for (const auto& suffix : suffixes) {
        const auto id = std::to_string(part) + ":" + std::to_string(number) + suffix;
        partitions[part].add(id, 1.0, true, part + 1);
      }
    }
    const auto local = partitions[part].oracle();
    full.insert(full.end(), local.begin(), local.end());
  }
  std::sort(full.begin(), full.end(), rankedBefore);
  const std::vector<std::pair<size_t, size_t>> pages{
      {0, 1}, {7, 11}, {79, 31}, {401, 53}, {1590, 40}, {1601, 5}};
  for (const auto& page : pages) {
    const auto end = page.first + page.second;
    std::vector<Ranked> merged;
    for (auto& fixture : partitions) {
      std::vector<TopKCandidate> local;
      bool fallback = false;
      require(fixture.run(end, local, fallback) == TopKResult::Ready && !fallback,
              "canonical distributed query fell back");
      const auto ranked = rank(local);
      merged.insert(merged.end(), ranked.begin(), ranked.end());
    }
    std::sort(merged.begin(), merged.end(), rankedBefore);
    const auto actualBegin = std::min(page.first, merged.size());
    const auto actualEnd = std::min(end, merged.size());
    const auto expectedBegin = std::min(page.first, full.size());
    const auto expectedEnd = std::min(end, full.size());
    require(std::vector<Ranked>(merged.begin() + actualBegin, merged.begin() + actualEnd) ==
                std::vector<Ranked>(full.begin() + expectedBegin, full.begin() + expectedEnd),
            "20-partition LIMIT/OFFSET differs from full global order");
  }
}

void errorsPropagate() {
  std::vector<TopKCandidate> result;
  bool fallback;
  Fixture failedSearch;
  failedSearch.searchError = true;
  require(failedSearch.run(1, result, fallback) == TopKResult::SearchError, "search error lost");
  Fixture failedRead;
  failedRead.add("a");
  failedRead.readError = key("a");
  require(failedRead.run(1, result, fallback) == TopKResult::ReadError, "primary error lost");
  Fixture invalid;
  invalid.add("a", std::numeric_limits<double>::infinity());
  require(invalid.run(1, result, fallback) == TopKResult::InvalidData, "invalid score accepted");
  Fixture badKey;
  badKey.hits.push_back({"broken", 1});
  require(badKey.run(1, result, fallback) == TopKResult::InvalidData, "invalid key accepted");
  Fixture empty;
  verify(empty, 3, false);
}
}  // namespace

int main() {
  try {
    canonicalByteOrder();
    missingPrimaryFallback();
    signedZeroFallback();
    noncanonicalAliases();
    crossPartitionPages();
    errorsPropagate();
    std::cout << "PASS: canonical byte order, missing-primary fallback, NUL aliases, "
                 "20-partition tied-score pagination, error propagation\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
