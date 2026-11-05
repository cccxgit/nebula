/* Copyright (c) 2026. Licensed under Apache 2.0. */
#include "TantivyIndex.h"

#include "TantivyBridge.h"

namespace nebula {
namespace plugin {
namespace {
NebulaTantivySlice slice(const std::string& value) {
  return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}
std::vector<NebulaTantivySlice> slices(const std::vector<std::string>& values) {
  std::vector<NebulaTantivySlice> result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.push_back(slice(value));
  }
  return result;
}
void onHit(void* context, const uint8_t* key, size_t length, double score) {
  auto* out = static_cast<std::vector<TantivyIndex::Hit>*>(context);
  out->push_back({std::string(reinterpret_cast<const char*>(key), length), score});
}
}  // namespace

TantivyIndex::~TantivyIndex() {
  nebula_tantivy_close(handle_);
}

bool TantivyIndex::checked(int result) {
  error_ = result == 0 ? "" : nebula_tantivy_last_error();
  return result == 0;
}

bool TantivyIndex::open(const std::string& path,
                        const std::vector<std::string>& fields,
                        bool writable,
                        size_t writerMemoryBytes,
                        const std::string& analyzer) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (handle_ != nullptr) {
    error_ = "Tantivy index is already open";
    return false;
  }
  auto fieldSlices = slices(fields);
  handle_ = nebula_tantivy_open(path.c_str(), fieldSlices.data(), fields.size(),
                              writable ? 1 : 0, writerMemoryBytes, slice(analyzer));
  return checked(handle_ == nullptr ? -1 : 0);
}

bool TantivyIndex::setWritable(bool writable) {
  std::lock_guard<std::mutex> lock(mutex_);
  return checked(nebula_tantivy_set_writable(handle_, writable ? 1 : 0));
}
bool TantivyIndex::upsert(const std::string& key,
                         const std::vector<std::string>& fieldValues) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto valueSlices = slices(fieldValues);
  return checked(nebula_tantivy_upsert(handle_, slice(key), valueSlices.data(),
                                     fieldValues.size()));
}
bool TantivyIndex::remove(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  return checked(nebula_tantivy_remove(handle_, slice(key)));
}
bool TantivyIndex::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  return checked(nebula_tantivy_clear(handle_));
}
bool TantivyIndex::commit() {
  std::lock_guard<std::mutex> lock(mutex_);
  return checked(nebula_tantivy_commit(handle_));
}
bool TantivyIndex::reload() {
  std::lock_guard<std::mutex> lock(mutex_);
  return checked(nebula_tantivy_reload(handle_));
}
bool TantivyIndex::search(const std::string& field,
                         const std::string& pattern,
                         SearchMode mode,
                         std::vector<Hit>& out,
                         size_t limit,
                         const std::string& keyPrefix) {
  std::lock_guard<std::mutex> lock(mutex_);
  out.clear();
  auto result = nebula_tantivy_search(handle_, slice(field), slice(pattern),
                                    static_cast<int>(mode), limit, slice(keyPrefix),
                                    onHit, &out);
  if (result != 0) {
    out.clear();
  }
  return checked(result);
}
std::string TantivyIndex::error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

}  // namespace plugin
}  // namespace nebula
