/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef META_DRAINERPROCESSOR_H_
#define META_DRAINERPROCESSOR_H_

#include "meta/processors/BaseProcessor.h"

namespace nebula {
namespace meta {

/**
 * @brief Register drainer service endpoints on primary cluster metad.
 *        The host list represents the drainer service addresses that the
 *        primary cluster can push WAL entries to.
 */
class SignInDrainerProcessor : public BaseProcessor<cpp2::ExecResp> {
 public:
  static SignInDrainerProcessor* instance(kvstore::KVStore* kvstore) {
    return new SignInDrainerProcessor(kvstore);
  }

  void process(const cpp2::SignInDrainerReq& req);

 private:
  explicit SignInDrainerProcessor(kvstore::KVStore* kvstore)
      : BaseProcessor<cpp2::ExecResp>(kvstore) {}
};

/**
 * @brief Unregister drainer service from primary cluster metad.
 */
class SignOutDrainerProcessor : public BaseProcessor<cpp2::ExecResp> {
 public:
  static SignOutDrainerProcessor* instance(kvstore::KVStore* kvstore) {
    return new SignOutDrainerProcessor(kvstore);
  }

  void process(const cpp2::SignOutDrainerReq& req);

 private:
  explicit SignOutDrainerProcessor(kvstore::KVStore* kvstore)
      : BaseProcessor<cpp2::ExecResp>(kvstore) {}
};

/**
 * @brief List registered drainer clients that have been signed in.
 */
class ListDrainerClientsProcessor : public BaseProcessor<cpp2::ListDrainerClientsResp> {
 public:
  static ListDrainerClientsProcessor* instance(kvstore::KVStore* kvstore) {
    return new ListDrainerClientsProcessor(kvstore);
  }

  void process(const cpp2::ListDrainerClientsReq& req);

 private:
  explicit ListDrainerClientsProcessor(kvstore::KVStore* kvstore)
      : BaseProcessor<cpp2::ListDrainerClientsResp>(kvstore) {}
};

/**
 * @brief Add drainer nodes on the backup cluster side.
 *        Assigns one drainer per partition in round-robin fashion.
 */
class AddDrainerProcessor : public BaseProcessor<cpp2::ExecResp> {
 public:
  static AddDrainerProcessor* instance(kvstore::KVStore* kvstore) {
    return new AddDrainerProcessor(kvstore);
  }

  void process(const cpp2::AddDrainerReq& req);

 private:
  explicit AddDrainerProcessor(kvstore::KVStore* kvstore)
      : BaseProcessor<cpp2::ExecResp>(kvstore) {}
};

/**
 * @brief Remove drainer nodes for a given space.
 */
class RemoveDrainerProcessor : public BaseProcessor<cpp2::ExecResp> {
 public:
  static RemoveDrainerProcessor* instance(kvstore::KVStore* kvstore) {
    return new RemoveDrainerProcessor(kvstore);
  }

  void process(const cpp2::RemoveDrainerReq& req);

 private:
  explicit RemoveDrainerProcessor(kvstore::KVStore* kvstore)
      : BaseProcessor<cpp2::ExecResp>(kvstore) {}
};

/**
 * @brief List all drainer nodes for a given space.
 */
class ListDrainersProcessor : public BaseProcessor<cpp2::ListDrainersResp> {
 public:
  static ListDrainersProcessor* instance(kvstore::KVStore* kvstore) {
    return new ListDrainersProcessor(kvstore);
  }

  void process(const cpp2::ListDrainersReq& req);

 private:
  explicit ListDrainersProcessor(kvstore::KVStore* kvstore)
      : BaseProcessor<cpp2::ListDrainersResp>(kvstore) {}
};

/**
 * @brief Get sync status (per partition progress) for a given space.
 *        Returns the WAL commit log ID that each partition has synced to.
 */
class GetSyncStatusProcessor : public BaseProcessor<cpp2::GetSyncStatusResp> {
 public:
  static GetSyncStatusProcessor* instance(kvstore::KVStore* kvstore) {
    return new GetSyncStatusProcessor(kvstore);
  }

  void process(const cpp2::GetSyncStatusReq& req);

 private:
  explicit GetSyncStatusProcessor(kvstore::KVStore* kvstore)
      : BaseProcessor<cpp2::GetSyncStatusResp>(kvstore) {}
};

/**
 * @brief Stop (pause) sync for a given space.
 *        Sets a flag in metad so that SyncListeners stop processing WAL logs.
 */
class StopSyncProcessor : public BaseProcessor<cpp2::ExecResp> {
 public:
  static StopSyncProcessor* instance(kvstore::KVStore* kvstore) {
    return new StopSyncProcessor(kvstore);
  }

  void process(const cpp2::StopSyncReq& req);

 private:
  explicit StopSyncProcessor(kvstore::KVStore* kvstore)
      : BaseProcessor<cpp2::ExecResp>(kvstore) {}
};

/**
 * @brief Restart (resume) sync for a given space.
 *        Clears the pause flag so that SyncListeners resume processing WAL logs.
 */
class RestartSyncProcessor : public BaseProcessor<cpp2::ExecResp> {
 public:
  static RestartSyncProcessor* instance(kvstore::KVStore* kvstore) {
    return new RestartSyncProcessor(kvstore);
  }

  void process(const cpp2::RestartSyncReq& req);

 private:
  explicit RestartSyncProcessor(kvstore::KVStore* kvstore)
      : BaseProcessor<cpp2::ExecResp>(kvstore) {}
};

}  // namespace meta
}  // namespace nebula
#endif  // META_DRAINERPROCESSOR_H_
