/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "graph/context/iterator/Iterator.h"

#include "common/memory/MemoryUtils.h"

DECLARE_int32(num_rows_to_check_memory);

namespace nebula {
namespace graph {

bool Iterator::hitsSysMemoryHighWatermark() const {
  if (checkMemory_) {
    if (numRowsModN_ >= FLAGS_num_rows_to_check_memory) {
      numRowsModN_ -= FLAGS_num_rows_to_check_memory;
    }
    if (UNLIKELY(numRowsModN_ == 0)) {
      uint64_t usedBytes = 0;
      uint64_t maxBytes = 0;
      if (memory::MemoryUtils::hitsOneQueryMemoryLimit()) {
        LOG(WARNING) << "===============in hitsSysMemoryHighWatermark";

        throw std::runtime_error(
            folly::sformat("Used memory({} bytes) exceeds one_query_max_memory_usage({} bytes).",
                           usedBytes,
                           maxBytes));
      }
    }
  }
  return false;
}

std::ostream& operator<<(std::ostream& os, Iterator::Kind kind) {
  switch (kind) {
    case Iterator::Kind::kDefault:
      os << "default";
      break;
    case Iterator::Kind::kSequential:
      os << "sequential";
      break;
    case Iterator::Kind::kGetNeighbors:
      os << "get neighbors";
      break;
    case Iterator::Kind::kProp:
      os << "Prop";
      break;
  }
  os << " iterator";
  return os;
}

}  // namespace graph
}  // namespace nebula
