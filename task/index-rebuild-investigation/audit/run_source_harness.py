#!/usr/bin/env python3
"""Extract unchanged baseline algorithms; compile with explicit test doubles.

Never modifies production sources or services. Log all compiler/run output here.
"""
from pathlib import Path
import hashlib
import json
import subprocess

OUT = Path(__file__).resolve().parent
ROOT = OUT.parents[2]
BASE = "de9b3ed800a6627d9845e9289b6bbc5b6faf460a"

def source(path):
    return subprocess.check_output(["git", "show", f"{BASE}:{path}"], cwd=ROOT, text=True)

def block(text, start, stop):
    return text[text.index(start):text.index(stop, text.index(start))]

header = source("src/kvstore/LogEncoder.h")
rebuild = source("src/storage/admin/RebuildIndexTask.cpp")
common = source("src/storage/CommonUtils.h")
pieces = {
    "log_enums": block(header, "enum LogType", "/**"),
    "batch_holder": block(header, "class BatchHolder", "\n}  // namespace kvstore"),
    "encode_batch": block(source("src/kvstore/LogEncoder.cpp"), "std::string encodeBatchValue(", "\nstd::vector<std::pair<BatchLogType"),
    "replay": block(rebuild, "nebula::cpp2::ErrorCode RebuildIndexTask::buildIndexOnOperations(", "\nnebula::cpp2::ErrorCode RebuildIndexTask::removeLegacyLogs"),
    "write_operation": block(rebuild, "nebula::cpp2::ErrorCode RebuildIndexTask::writeOperation(", "\n}  // namespace storage"),
    "request_wrapper": block(common, "class IndexCountWrapper", "\nenum class ResultStatus"),
    "rate_limiter": block(source("src/kvstore/RateLimiter.h"), "class RateLimiter", "\n}  // namespace kvstore"),
}
manifest = {"commit": BASE, "excerpts_sha256": {k: hashlib.sha256(v.encode()).hexdigest() for k, v in pieces.items()}}
(OUT / "source_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
for name, value in pieces.items():
    (OUT / f"original_{name}.inc").write_text(value)

preamble = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <unistd.h>
#include <boost/core/noncopyable.hpp>
#include "common/cpp/helpers.h"
// Only infrastructure stand-ins below; original excerpts remain byte-identical.
struct NullLog { template<class T> NullLog& operator<<(const T&) { return *this; } };
#define LOG(level) NullLog()
#define VLOG(level) NullLog()
#define UNLIKELY(x) (x)
namespace folly {
class StringPiece {
 public:
  explicit StringPiece(const std::string& s) : s_(&s) {}
  std::string str() const { return *s_; }
  const std::string& raw() const { return *s_; }
 private:
  const std::string* s_;
};
template<class... A> std::string sformat(const char* s, A&&...) { return s; }
template<bool, template<typename> class> class Baton {
 public:
  void post() { done_ = true; }
  void wait() { assert(done_); }
 private: bool done_{false};
};
}
uint32_t FLAGS_rebuild_index_batch_size = 128 * 1024;
uint32_t FLAGS_rebuild_index_part_rate_limit = 512 * 1024;
namespace nebula {
using GraphSpaceID = int32_t;
using PartitionID = int32_t;
namespace cpp2 { enum class ErrorCode { SUCCEEDED, E_USER_CANCEL, E_INVALID_OPERATION }; }
namespace time {
struct WallClock {
 static int64_t fastNowInMilliSec() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
 }
};
}
namespace kvstore {
#include "original_log_enums.inc"
#include "original_batch_holder.inc"
#include "original_encode_batch.inc"
class RateLimiter {
 public:
  std::vector<size_t> consumed;
  void consume(double n, double, double) { consumed.push_back(static_cast<size_t>(n)); }
};
class KVIterator {
 public:
  std::vector<std::pair<std::string, std::string>> rows;
  size_t index{0};
  bool valid() const { return index < rows.size(); }
  folly::StringPiece key() const { return folly::StringPiece(rows.at(index).first); }
  folly::StringPiece val() const { return folly::StringPiece(rows.at(index).second); }
  void next() { ++index; }
};
struct Store {
 std::map<std::string, std::string> entries;
 std::vector<size_t> encodedSizes;
 std::vector<uint32_t> encodedOperations;
 std::function<void(size_t)> afterAppend;
 size_t prefixCalls{0};
 cpp2::ErrorCode prefix(GraphSpaceID, PartitionID, const std::string& p,
                       std::unique_ptr<KVIterator>* out) {
  ++prefixCalls;
  auto it = std::make_unique<KVIterator>();
  for (const auto& kv : entries) if (kv.first.compare(0, p.size(), p) == 0) it->rows.push_back(kv);
  *out = std::move(it);
  return cpp2::ErrorCode::SUCCEEDED;
 }
 static uint32_t u32(const std::string& s, size_t pos) {
  uint32_t v; std::memcpy(&v, s.data()+pos, sizeof(v)); return v;
 }
 void asyncAppendBatch(GraphSpaceID, PartitionID, std::string&& encoded,
                       std::function<void(cpp2::ErrorCode)> cb) {
  encodedSizes.push_back(encoded.size());
  auto count = u32(encoded, 9);
  encodedOperations.push_back(count);
  size_t pos = 13;
  for (uint32_t i = 0; i < count; ++i) {
   auto type = static_cast<BatchLogType>(encoded.at(pos++));
   auto nk = u32(encoded,pos); pos += 4;
   auto key = encoded.substr(pos,nk); pos += nk;
   auto nv = u32(encoded,pos); pos += 4;
   auto val = encoded.substr(pos,nv); pos += nv;
   if (type == OP_BATCH_PUT) entries[key] = val;
   else if (type == OP_BATCH_REMOVE) entries.erase(key);
   else assert(false);
  }
  assert(pos == encoded.size());
  cb(cpp2::ErrorCode::SUCCEEDED);
  if (afterAppend) afterAppend(encodedSizes.size());
 }
};
}
// Fixed synthetic prefix/header respects 16-byte modify-op overhead; no key codec claim.
class OperationKeyUtils {
 public:
  static std::string operationPrefix(PartitionID) { return "OP01"; }
  static bool isModifyOperation(const folly::StringPiece& p) { return p.raw().at(12) == 'M'; }
  static bool isDeleteOperation(const folly::StringPiece& p) { return p.raw().at(12) == 'D'; }
  static std::string getOperationKey(const folly::StringPiece& p) { return p.raw().substr(16); }
};
namespace storage {
enum class IndexState { STARTING, BUILDING, LOCKED, FINISHED };
using IndexKey = std::tuple<GraphSpaceID, PartitionID>;
struct Guard : std::map<IndexKey, IndexState> {
 std::atomic<bool> locked{false};
 void assign(IndexKey k, IndexState v) { at(k) = v; if (v == IndexState::LOCKED) locked = true; }
};
struct StorageEnv {
 kvstore::Store* kvstore_;
 std::unique_ptr<Guard> rebuildIndexGuard_{new Guard};
 std::atomic<int32_t> onFlyingRequest_{0};
};
#include "original_request_wrapper.inc"
class RebuildIndexTask {
 public:
  explicit RebuildIndexTask(StorageEnv* e) : env_(e) {}
  cpp2::ErrorCode buildIndexOnOperations(GraphSpaceID, PartitionID, kvstore::RateLimiter*);
  cpp2::ErrorCode writeOperation(GraphSpaceID, PartitionID, kvstore::BatchHolder*, kvstore::RateLimiter*);
  StorageEnv* env_;
  std::atomic<bool> canceled_{false};
};
const int32_t kReserveNum = 1024 * 4;
#include "original_replay.inc"
#include "original_write_operation.inc"
}
}
'''
main = r'''
using namespace nebula;
static std::string makeKey(size_t i, size_t len) {
 std::string key(len, 'I');
 std::string id = std::to_string(i);
 key.replace(len-id.size(), id.size(), id);
 return key;
}
void fill(kvstore::Store& s, size_t n, size_t keyLen) {
 for(size_t i=1; i<=n; ++i) {
  auto key = makeKey(i, keyLen);
  s.entries[std::string("OP01") + std::string(8, '\0') + "M000" + key] = "";
 }
}
void runBatch(size_t n, size_t keyLen, size_t batch, size_t rate, bool cancel) {
 FLAGS_rebuild_index_batch_size = batch;
 FLAGS_rebuild_index_part_rate_limit = rate;
 kvstore::Store store; fill(store,n,keyLen);
 storage::StorageEnv env; env.kvstore_ = &store;
 (*env.rebuildIndexGuard_)[{1,1}] = storage::IndexState::BUILDING;
 storage::RebuildIndexTask task(&env);
 kvstore::RateLimiter limiter;
 if(cancel) store.afterAppend = [&](size_t count) { if(count == 1) task.canceled_ = true; };
 auto begin = std::chrono::steady_clock::now();
 auto code = task.buildIndexOnOperations(1,1,&limiter);
 double ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
 uint64_t totalBytes=0,totalOps=0,consumed=0,large=0;
 for(auto s:store.encodedSizes) totalBytes+=s;
 for(auto s:store.encodedOperations) totalOps+=s;
 for(auto s:limiter.consumed) { consumed+=s; if(s>rate) ++large; }
 size_t pending=0; for(auto& kv:store.entries) if(kv.first.rfind("OP01",0)==0) ++pending;
 std::cout << "batch n="<<n<<" key_len="<<keyLen<<" row_payload="<<(2*keyLen+16)
  <<" batch="<<batch<<" rate="<<rate<<" cancel_after_first="<<cancel<<" code="<<static_cast<int>(code)
  <<" append_calls="<<store.encodedSizes.size()<<" prefix_calls="<<store.prefixCalls
  <<" replayed_lowlevel_ops="<<totalOps<<" encoded_bytes="<<totalBytes
  <<" consumed_bytes="<<consumed<<" calls_over_burst="<<large<<" remaining_ops="<<pending
  <<" final_index_keys="<<(store.entries.size()-pending)<<" elapsed_ms="<<ms<<"\n";
 size_t show=std::min<size_t>(5,store.encodedSizes.size());
 std::cout << " first_calls(batch_payload,lowlevel_ops,encoded_bytes):";
 for(size_t i=0;i<show;++i) std::cout<<" ("<<limiter.consumed[i]<<","<<store.encodedOperations[i]<<","<<store.encodedSizes[i]<<")";
 std::cout<<"\n last_calls:";
 for(size_t i=store.encodedSizes.size()>3 ? store.encodedSizes.size()-3 : 0;i<store.encodedSizes.size();++i)
  std::cout<<" ("<<limiter.consumed[i]<<","<<store.encodedOperations[i]<<","<<store.encodedSizes[i]<<")";
 std::cout<<"\n";
 assert(pending==0 && store.entries.size()==n);
 if(cancel) assert(code==cpp2::ErrorCode::E_USER_CANCEL);
 else assert(code==cpp2::ErrorCode::SUCCEEDED);
}
void runWait(bool cancel) {
 kvstore::Store store;
 storage::StorageEnv env; env.kvstore_=&store;
 (*env.rebuildIndexGuard_)[{1,1}]=storage::IndexState::BUILDING;
 storage::RebuildIndexTask task(&env);
 kvstore::RateLimiter limiter;
 std::atomic<bool> completed{false};
 cpp2::ErrorCode result;
 auto unrelated = std::make_unique<storage::IndexCountWrapper>(&env);
 auto begin=std::chrono::steady_clock::now();
 std::thread t([&] {result=task.buildIndexOnOperations(1,1,&limiter);completed=true;});
 while(!env.rebuildIndexGuard_->locked) std::this_thread::yield();
 if(cancel) task.canceled_=true;
 std::this_thread::sleep_for(std::chrono::milliseconds(350));
 bool completedWhileBusy=completed;
 std::cout<<"wait cancel="<<cancel<<" locked="<<env.rebuildIndexGuard_->locked
  <<" pending_operation_records=0 count_unrelated_request="<<env.onFlyingRequest_
  <<" completed_after_350ms="<<completedWhileBusy<<"\n";
 assert(!completedWhileBusy);
 unrelated.reset(); t.join();
 double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
 std::cout<<"wait_after_release count="<<env.onFlyingRequest_<<" completed="<<completed
  <<" code="<<static_cast<int>(result)<<" elapsed_ms="<<ms<<"\n";
 assert(completed);
}
int main() {
 std::cout<<"source_harness: original baseline methods, mock snapshot KVStore/async callback/limiter; real encoder/BatchHolder/IndexCountWrapper.\n";
 runBatch(2000,24,131072,524288,false);
 runBatch(2200,24,131072,524288,false);
 runBatch(8250,24,131072,524288,false);
 runBatch(5500,40,131072,524288,false);
 runBatch(500,1040,1048576,4194304,false);
 runBatch(510,1040,1048576,4194304,false);
 runBatch(2030,1040,1048576,4194304,false);
 runBatch(2200,24,131072,524288,true);
 runWait(false);
 runWait(true);
}
'''
(OUT / "source_harness.cpp").write_text(preamble + main)

rate = r'''
#include <folly/TokenBucket.h>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
bool FLAGS_skip_wait_in_rate_limiter = false;
namespace nebula { namespace time {
struct WallClock {
 static int64_t fastNowInSec() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
 }
};
} namespace kvstore {
#include "original_rate_limiter.inc"
}}
int main() {
 nebula::kvstore::RateLimiter limiter;
 for (double bytes : {524289.,4194305.}) {
  double burst = bytes-1;
  auto begin = std::chrono::steady_clock::now();
  limiter.consume(bytes,burst,burst);
  double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
  std::cout<<"original_RateLimiter bytes="<<bytes<<" rate_and_burst="<<burst<<" elapsed_ms="<<ms<<"\n";
  if(ms < 990.) return 1;
 }
}
'''
(OUT / "rate_harness.cpp").write_text(rate)

with (OUT / "source_harness.log").open("w") as log:
    log.write(f"baseline={BASE}\n"); log.flush()
    for filename in ("source_harness", "rate_harness"):
        command=["g++","-std=c++17","-O2","-pthread",f"-I{ROOT / 'src'}", "-I/opt/vesoft/third-party/3.3/include",str(OUT/f"{filename}.cpp"),"-o",str(OUT/filename)]
        if filename=="rate_harness":
            command.extend(["-L/opt/vesoft/third-party/3.3/lib","-lfolly","-lglog","-lgflags","-ldouble-conversion","-levent","-lssl","-lcrypto","-ldl"])
        log.write("compile="+" ".join(command)+"\n"); log.flush()
        subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([str(OUT/filename)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
print((OUT / "source_harness.log").read_text())
