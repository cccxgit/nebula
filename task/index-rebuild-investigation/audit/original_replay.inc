nebula::cpp2::ErrorCode RebuildIndexTask::buildIndexOnOperations(
    GraphSpaceID space, PartitionID part, kvstore::RateLimiter* rateLimiter) {
  if (canceled_) {
    LOG(INFO) << folly::sformat("Rebuild index canceled, space={}, part={}", space, part);
    return nebula::cpp2::ErrorCode::E_USER_CANCEL;
  }

  while (true) {
    if (UNLIKELY(canceled_)) {
      LOG(INFO) << folly::sformat("Rebuild index canceled, space={}, part={}", space, part);
      return nebula::cpp2::ErrorCode::E_USER_CANCEL;
    }

    std::unique_ptr<kvstore::KVIterator> operationIter;
    auto operationPrefix = OperationKeyUtils::operationPrefix(part);
    auto operationRet = env_->kvstore_->prefix(space, part, operationPrefix, &operationIter);
    if (operationRet != nebula::cpp2::ErrorCode::SUCCEEDED) {
      LOG(INFO) << "Processing Part " << part << " Failed";
      return operationRet;
    }

    std::unique_ptr<kvstore::BatchHolder> batchHolder = std::make_unique<kvstore::BatchHolder>();
    batchHolder->reserve(kReserveNum);
    while (operationIter->valid()) {
      auto opKey = operationIter->key();
      auto opVal = operationIter->val();
      // replay operation record
      if (OperationKeyUtils::isModifyOperation(opKey)) {
        VLOG(1) << "Processing Modify Operation " << opKey;
        auto key = OperationKeyUtils::getOperationKey(opKey);
        batchHolder->put(std::move(key), opVal.str());
      } else if (OperationKeyUtils::isDeleteOperation(opKey)) {
        VLOG(1) << "Processing Delete Operation " << opVal;
        batchHolder->remove(opVal.str());
      } else {
        LOG(INFO) << "Unknown Operation Type";
        return nebula::cpp2::ErrorCode::E_INVALID_OPERATION;
      }

      batchHolder->remove(opKey.str());
      if (batchHolder->size() > FLAGS_rebuild_index_batch_size) {
        auto ret = writeOperation(space, part, batchHolder.get(), rateLimiter);
        if (nebula::cpp2::ErrorCode::SUCCEEDED != ret) {
          LOG(INFO) << "Write Operation Failed";
          return ret;
        }
      }
      operationIter->next();
    }

    auto ret = writeOperation(space, part, batchHolder.get(), rateLimiter);
    if (nebula::cpp2::ErrorCode::SUCCEEDED != ret) {
      LOG(INFO) << "Write Operation Failed";
      return ret;
    }

    // When the processed operation size is less than the batch size,
    // we will mark the lock's building in StorageEnv and refuse writing for
    // a short piece of time.
    if (batchHolder->size() <= FLAGS_rebuild_index_batch_size) {
      // lock the part
      auto key = std::make_tuple(space, part);
      auto stateIter = env_->rebuildIndexGuard_->find(key);
      // If the state is LOCKED, we should wait the on flying request process
      // finished.
      if (stateIter != env_->rebuildIndexGuard_->cend() &&
          stateIter->second == IndexState::BUILDING) {
        env_->rebuildIndexGuard_->assign(std::move(key), IndexState::LOCKED);

        // Waiting all of the on flying requests have finished.
        int32_t currentRequestNum;
        do {
          currentRequestNum = env_->onFlyingRequest_.load();
          VLOG(1) << "On Flying Request: " << currentRequestNum;
          usleep(100);
        } while (currentRequestNum != 0);
      } else {
        break;
      }
    }
  }
  return nebula::cpp2::ErrorCode::SUCCEEDED;
}
