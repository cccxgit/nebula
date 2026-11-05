/* Copyright (c) 2026. Licensed under Apache 2.0. */
#ifndef COMMON_PLUGIN_FULLTEXT_TANTIVY_TANTIVYINDEX_H_
#define COMMON_PLUGIN_FULLTEXT_TANTIVY_TANTIVYINDEX_H_

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace nebula {
namespace plugin {

// Owns one on-disk Tantivy index. All methods serialize access to the handle.
// commit() also reloads its reader; close/destruction does not commit pending writes.
class TantivyIndex final {
 public:
  enum class SearchMode { Query = 0, Contains = 1, StartsWith = 2, EndsWith = 3 };
  struct Hit {
    std::string key;
    double score;
  };

  TantivyIndex() = default;
  ~TantivyIndex();
  TantivyIndex(const TantivyIndex&) = delete;
  TantivyIndex& operator=(const TantivyIndex&) = delete;

  bool open(const std::string& path,
            const std::vector<std::string>& fields,
            bool writable = true,
            size_t writerMemoryBytes = 16 * 1024 * 1024,
            const std::string& analyzer = "standard");
  bool setWritable(bool writable);
  bool upsert(const std::string& key, const std::vector<std::string>& fieldValues);
  bool remove(const std::string& key);
  bool clear();
  bool commit();
  bool reload();
  bool search(const std::string& field,
              const std::string& pattern,
              SearchMode mode,
              std::vector<Hit>& out,
              size_t limit = 0,
              const std::string& keyPrefix = "");
  std::string error() const;

 private:
  bool checked(int result);
  mutable std::mutex mutex_;
  void* handle_{nullptr};
  std::string error_;
};

}  // namespace plugin
}  // namespace nebula
#endif
