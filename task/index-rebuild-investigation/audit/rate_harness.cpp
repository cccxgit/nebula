
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
