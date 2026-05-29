/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef META_PROCESSORS_PARTS_RENAMESPACEHARDPROCESSOR_H_
#define META_PROCESSORS_PARTS_RENAMESPACEHARDPROCESSOR_H_

#include <optional>

#include "meta/processors/BaseProcessor.h"

namespace nebula {
namespace meta {

struct RenameSpaceHardReq final {
  std::string oldName;
  std::string newName;
  bool dryRun{true};
  std::optional<GraphSpaceID> expectedSpaceId;
  std::string operatorName;
  std::string comment;
};

class RenameSpaceHardProcessor : public BaseProcessor<cpp2::ExecResp> {
 public:
  static RenameSpaceHardProcessor* instance(kvstore::KVStore* kvstore) {
    return new RenameSpaceHardProcessor(kvstore);
  }

  void process(const RenameSpaceHardReq& req);

 private:
  explicit RenameSpaceHardProcessor(kvstore::KVStore* kvstore)
      : BaseProcessor<cpp2::ExecResp>(kvstore) {}

  nebula::cpp2::ErrorCode checkLeader();
};

}  // namespace meta
}  // namespace nebula

#endif  // META_PROCESSORS_PARTS_RENAMESPACEHARDPROCESSOR_H_
