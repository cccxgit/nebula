nebula::cpp2::ErrorCode RebuildIndexTask::writeOperation(GraphSpaceID space,
                                                         PartitionID part,
                                                         kvstore::BatchHolder* batchHolder,
                                                         kvstore::RateLimiter* rateLimiter) {
  folly::Baton<true, std::atomic> baton;
  auto result = nebula::cpp2::ErrorCode::SUCCEEDED;
  auto encoded = encodeBatchValue(batchHolder->getBatch());
  rateLimiter->consume(static_cast<double>(batchHolder->size()),                   // toConsume
                       static_cast<double>(FLAGS_rebuild_index_part_rate_limit),   // rate
                       static_cast<double>(FLAGS_rebuild_index_part_rate_limit));  // burstSize
  env_->kvstore_->asyncAppendBatch(
      space, part, std::move(encoded), [&result, &baton](nebula::cpp2::ErrorCode code) {
        if (code != nebula::cpp2::ErrorCode::SUCCEEDED) {
          result = code;
        }
        baton.post();
      });
  baton.wait();
  return result;
}
