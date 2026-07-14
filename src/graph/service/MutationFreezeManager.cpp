/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under the Apache License, Version 2.0.
 */

#include "graph/service/MutationFreezeManager.h"

#include "graph/service/GraphFlags.h"
#include "parser/AdminSentences.h"
#include "parser/ExplainSentence.h"
#include "parser/SequentialSentences.h"
#include "parser/Sentence.h"

namespace nebula {
namespace graph {

MutationLease::MutationLease(MutationFreezeManager* manager) : manager_(manager) {}

MutationLease::~MutationLease() {
  if (manager_ != nullptr) {
    manager_->release();
  }
}

MutationFreezeManager& MutationFreezeManager::instance() {
  static MutationFreezeManager manager;
  return manager;
}

Status MutationFreezeManager::checkMutationAllowed(const Sentence* sentence) const {
  if (isMutating(sentence) && !FLAGS_enable_graph_mutation) {
    return Status::Error("MUTATION_FROZEN: graphd is read-only for migration export");
  }
  return Status::OK();
}

StatusOr<std::unique_ptr<MutationLease>> MutationFreezeManager::acquire(
    const Sentence* sentence) {
  if (!isMutating(sentence)) {
    return std::unique_ptr<MutationLease>();
  }
  if (!FLAGS_enable_graph_mutation) {
    return Status::Error("MUTATION_FROZEN: graphd is read-only for migration export");
  }

  activeMutationCount_.fetch_add(1, std::memory_order_acq_rel);
  // A flag update can race with validation. Roll back the lease so a successful freeze never
  // reports FROZEN while a post-freeze mutation remains admitted.
  if (!FLAGS_enable_graph_mutation) {
    release();
    return Status::Error("MUTATION_FROZEN: graphd is read-only for migration export");
  }
  return std::make_unique<MutationLease>(this);
}

bool MutationFreezeManager::isMutating(const Sentence* sentence) const {
  if (sentence == nullptr) {
    return false;
  }

  switch (sentence->kind()) {
    case Sentence::Kind::kSequential: {
      auto* sequential = static_cast<const SequentialSentences*>(sentence);
      for (const auto* child : sequential->sentences()) {
        if (isMutating(child)) {
          return true;
        }
      }
      return false;
    }
    case Sentence::Kind::kExplain: {
      auto* explain = static_cast<const ExplainSentence*>(sentence);
      return explain->isProfile() && isMutating(explain->seqSentences());
    }
    case Sentence::Kind::kSetConfig: {
      auto* setConfig = static_cast<SetConfigSentence*>(const_cast<Sentence*>(sentence));
      auto* item = setConfig->configItem();
      return item == nullptr || item->getName() == nullptr ||
             *item->getName() != "enable_graph_mutation";
    }
    case Sentence::Kind::kGo:
    case Sentence::Kind::kSet:
    case Sentence::Kind::kPipe:
    case Sentence::Kind::kUse:
    case Sentence::Kind::kMatch:
    case Sentence::Kind::kDescribeTag:
    case Sentence::Kind::kDescribeEdge:
    case Sentence::Kind::kDescribeTagIndex:
    case Sentence::Kind::kDescribeEdgeIndex:
    case Sentence::Kind::kShowHosts:
    case Sentence::Kind::kShowSpaces:
    case Sentence::Kind::kShowParts:
    case Sentence::Kind::kShowTags:
    case Sentence::Kind::kShowEdges:
    case Sentence::Kind::kShowTagIndexes:
    case Sentence::Kind::kShowEdgeIndexes:
    case Sentence::Kind::kShowTagIndexStatus:
    case Sentence::Kind::kShowUsers:
    case Sentence::Kind::kShowRoles:
    case Sentence::Kind::kShowCreateSpace:
    case Sentence::Kind::kShowCreateTag:
    case Sentence::Kind::kShowCreateEdge:
    case Sentence::Kind::kShowCreateTagIndex:
    case Sentence::Kind::kShowCreateEdgeIndex:
    case Sentence::Kind::kShowSnapshots:
    case Sentence::Kind::kShowCharset:
    case Sentence::Kind::kShowCollation:
    case Sentence::Kind::kShowGroups:
    case Sentence::Kind::kShowZones:
    case Sentence::Kind::kShowStats:
    case Sentence::Kind::kShowServiceClients:
    case Sentence::Kind::kShowFTIndexes:
    case Sentence::Kind::kDescribeUser:
    case Sentence::Kind::kDescribeSpace:
    case Sentence::Kind::kYield:
    case Sentence::Kind::kShowConfigs:
    case Sentence::Kind::kGetConfig:
    case Sentence::Kind::kFetchVertices:
    case Sentence::Kind::kFetchEdges:
    case Sentence::Kind::kFindPath:
    case Sentence::Kind::kLimit:
    case Sentence::Kind::kGroupBy:
    case Sentence::Kind::kReturn:
    case Sentence::Kind::kOrderBy:
    case Sentence::Kind::kAdminShowJobs:
    case Sentence::Kind::kGetSubgraph:
    case Sentence::Kind::kDescribeZone:
    case Sentence::Kind::kListZones:
    case Sentence::Kind::kShowListener:
    case Sentence::Kind::kLookup:
    case Sentence::Kind::kShowSessions:
    case Sentence::Kind::kShowQueries:
    case Sentence::Kind::kShowMetaLeader:
    case Sentence::Kind::kUnwind:
      return false;
    default:
      // New or unclassified sentences must be denied during a migration freeze.
      return true;
  }
}

uint64_t MutationFreezeManager::activeMutationCount() const {
  return activeMutationCount_.load(std::memory_order_acquire);
}

bool MutationFreezeManager::isFrozen() const {
  return !FLAGS_enable_graph_mutation && activeMutationCount() == 0;
}

void MutationFreezeManager::release() {
  auto previous = activeMutationCount_.fetch_sub(1, std::memory_order_acq_rel);
  DCHECK_GT(previous, 0);
}

}  // namespace graph
}  // namespace nebula
