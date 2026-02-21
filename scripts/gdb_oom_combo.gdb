set pagination off
set print pretty on
set breakpoint pending on
set detach-on-fork off
set follow-fork-mode parent
handle SIGPIPE nostop noprint pass

printf "\n[OOM-COMBO] gdb script loaded.\n"
printf "[OOM-COMBO] tips: use 'continue' to run, 'info break' to inspect breakpoints.\n\n"

# -------- Global memory signal --------
b nebula::memory::MemoryUtils::hitsHighWatermark
commands
  silent
  printf "\n>>> hit MemoryUtils::hitsHighWatermark\n"
  bt 3
  continue
end

b nebula::graph::Executor::checkMemoryWatermark
commands
  silent
  printf "\n>>> hit Executor::checkMemoryWatermark\n"
  p nebula::memory::MemoryUtils::kHitMemoryHighWatermark
  p FLAGS_system_memory_high_watermark_ratio
  continue
end

# -------- Scheme 2: executor in-loop memory checks --------
b nebula::graph::StorageAccessExecutor::checkMemoryAndAbortQuery
commands
  silent
  printf "\n>>> hit StorageAccessExecutor::checkMemoryAndAbortQuery\n"
  p nebula::memory::MemoryUtils::kHitMemoryHighWatermark
  bt 5
  continue
end

b nebula::graph::TraverseExecutor::buildRequestVids
commands
  silent
  printf "\n>>> hit TraverseExecutor::buildRequestVids\n"
  p FLAGS_num_rows_to_check_memory
  continue
end

b nebula::graph::TraverseExecutor::buildAdjList
commands
  silent
  printf "\n>>> hit TraverseExecutor::buildAdjList\n"
  p FLAGS_num_rows_to_check_memory
  continue
end

# -------- Scheme 1: storage inflight limiting --------
# collectResponse is a template function, rbreak is more reliable than b.
rbreak collectResponse
commands
  silent
  printf "\n>>> hit *collectResponse* (template)\n"
  p FLAGS_max_storage_inflight_per_query
  p nebula::memory::MemoryUtils::kHitMemoryHighWatermark
  bt 5
  continue
end

# Optional deep breakpoint (manual, high frequency):
#   rbreak markAbortAndSkipUnlaunched

printf "[OOM-COMBO] breakpoints configured. run 'continue' now.\n"
