/* Copyright (c) 2026. Licensed under Apache 2.0. */
#ifndef STORAGE_FULLTEXT_TANTIVYTOPK_H_
#define STORAGE_FULLTEXT_TANTIVYTOPK_H_

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/plugin/fulltext/tantivy/TantivyIndex.h"

namespace nebula::storage::fulltext {

struct TopKCandidate {
  plugin::TantivyIndex::Hit hit;
  std::string id;
  // Cache the successful primary read. A second read after truncation could
  // lose a candidate to a concurrent deletion without fetching its replacement.
  std::string data;
};

enum class TopKRead { Found, Missing, Error };
enum class TopKResult { Ready, SearchError, ReadError, InvalidData };

// Decode a FIXED_STRING VID and reject embedded NUL followed by nonzero bytes
// for the bounded path. Plain trailing zero padding is canonical.
inline bool decodeCanonicalVid(const std::string& padded, std::string& id) {
  const auto end = padded.find('\0');
  id = padded.substr(0, end);
  return end == std::string::npos || padded.find_first_not_of('\0', end) == std::string::npos;
}

// Search(limit, hits) returns bool; limit=0 means the complete candidate set.
// Read(key, data) returns TopKRead. Decode(key, id, canonical) validates the tag
// key and extracts its decoded VID. Callback errors remain owned by the caller.
// This small callback boundary also permits deterministic stale-hit regression
// tests without deleting primary records behind a running cluster's back.
template <typename Search, typename Read, typename Decode>
TopKResult prepareTopK(size_t limit,
                       Search&& search,
                       Read&& read,
                       Decode&& decode,
                       std::vector<TopKCandidate>& output,
                       bool& fallback) {
  output.clear();
  fallback = false;
  if (limit == 0) return TopKResult::InvalidData;
  std::vector<plugin::TantivyIndex::Hit> hits;
  if (!search(limit, hits)) return TopKResult::SearchError;
  std::unordered_set<std::string> seen;
  for (auto& hit : hits) {
    TopKCandidate candidate{std::move(hit), {}, {}};
    bool canonical = false;
    if (!std::isfinite(candidate.hit.score) || !decode(candidate.hit.key, candidate.id, canonical))
      return TopKResult::InvalidData;
    // Rust total_cmp distinguishes +0.0/-0.0; Graph's numeric comparison does
    // not. A zero-score boundary therefore needs Graph's full tie ordering.
    if (!canonical || candidate.hit.score == 0.0 || !seen.emplace(candidate.id).second) {
      fallback = true;
      break;
    }
    const auto status = read(candidate.hit.key, candidate.data);
    if (status == TopKRead::Error) return TopKResult::ReadError;
    if (status == TopKRead::Missing) {
      fallback = true;
      break;
    }
    output.emplace_back(std::move(candidate));
  }
  const auto rankedBefore = [](const TopKCandidate& lhs, const TopKCandidate& rhs) {
    return lhs.hit.score != rhs.hit.score ? lhs.hit.score > rhs.hit.score : lhs.id < rhs.id;
  };
  if (!fallback) {
    std::sort(output.begin(), output.end(), rankedBefore);
    return TopKResult::Ready;
  }

  // Emit nothing from the bounded attempt. Missing keys and decoded-ID aliases
  // can consume K slots; signed zero can change the tie order. Retrieve all hits
  // before selecting K live unique IDs using Graph's ordering.
  output.clear();
  hits.clear();
  if (!search(0, hits)) return TopKResult::SearchError;
  std::vector<TopKCandidate> candidates;
  candidates.reserve(hits.size());
  for (auto& hit : hits) {
    TopKCandidate candidate{std::move(hit), {}, {}};
    bool canonical = false;
    if (!std::isfinite(candidate.hit.score) || !decode(candidate.hit.key, candidate.id, canonical))
      return TopKResult::InvalidData;
    candidates.emplace_back(std::move(candidate));
  }
  std::sort(candidates.begin(), candidates.end(), rankedBefore);
  seen.clear();
  for (auto& candidate : candidates) {
    if (seen.count(candidate.id) != 0) continue;
    const auto status = read(candidate.hit.key, candidate.data);
    if (status == TopKRead::Error) return TopKResult::ReadError;
    if (status == TopKRead::Missing) continue;
    // Mark an ID only after a live primary read, keeping its highest live score.
    seen.emplace(candidate.id);
    output.emplace_back(std::move(candidate));
    if (output.size() == limit) break;
  }
  return TopKResult::Ready;
}

}  // namespace nebula::storage::fulltext
#endif
