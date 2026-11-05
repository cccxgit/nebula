/* Copyright (c) 2026. Licensed under Apache 2.0. */
#include "storage/exec/IndexTantivyScanNode.h"

#include "codec/RowReaderWrapper.h"
#include "common/utils/NebulaKeyUtils.h"
#include "kvstore/Part.h"
#include "storage/exec/QueryUtils.h"
#include "storage/fulltext/TantivyManager.h"
namespace nebula::storage {
IndexTantivyScanNode::IndexTantivyScanNode(RuntimeContext* context,
                                           const cpp2::IndexQueryContext& query,
                                           size_t limit)
    : IndexNode(context, "TantivyIndexScanNode"), query_(query), limit_(limit) {}
IndexTantivyScanNode::IndexTantivyScanNode(const IndexTantivyScanNode& node)
    : IndexNode(node), query_(node.query_), columns_(node.columns_), limit_(node.limit_) {}
std::unique_ptr<IndexNode> IndexTantivyScanNode::copy() {
  return std::make_unique<IndexTantivyScanNode>(*this);
}
std::string IndexTantivyScanNode::identify() {
  return fmt::format("{}(index={},field={},mode={},topk={},fallback={})",
                     name_,
                     query_.fulltext_index_name_ref().value_or(""),
                     query_.fulltext_field_ref().value_or(""),
                     query_.fulltext_mode_ref().value_or(0),
                     limit_,
                     fallback_);
}
ErrorCode IndexTantivyScanNode::init(InitContext& ctx) {
  if (!context_->env()->tantivy_) return ErrorCode::E_UNSUPPORTED;
  auto index = context_->env()->metaClient_->getFTIndexByNameFromCache(
      spaceId_, query_.fulltext_index_name_ref().value_or(""));
  if (!index.ok()) return ErrorCode::E_INDEX_NOT_FOUND;
  const auto& schema = index.value().get_depend_schema();
  if (context_->isEdge()) {
    if (schema.getType() != nebula::cpp2::SchemaID::Type::edge_type ||
        schema.get_edge_type() != context_->edgeType_) {
      return ErrorCode::E_INVALID_PARM;
    }
  } else if (schema.getType() != nebula::cpp2::SchemaID::Type::tag_id ||
             schema.get_tag_id() != context_->tagId_) {
    return ErrorCode::E_INVALID_PARM;
  }
  columns_.assign(ctx.requiredColumns.begin(), ctx.requiredColumns.end());
  ctx.returnColumns = columns_;
  ctx.retColMap.clear();
  for (size_t i = 0; i < columns_.size(); ++i) ctx.retColMap[columns_[i]] = i;
  return ErrorCode::SUCCEEDED;
}
ErrorCode IndexTantivyScanNode::doExecute(PartitionID part) {
  part_ = part;
  cursor_ = 0;
  hits_.clear();
  candidates_.clear();
  fallback_ = false;
  auto p = context_->env()->kvstore_->part(spaceId_, part);
  if (!ok(p)) return error_ = error(p);
  if (!value(p)->isLeader()) return error_ = ErrorCode::E_LEADER_CHANGED;
  auto search = [&](size_t limit, std::vector<plugin::TantivyIndex::Hit>& hits) {
    error_ = context_->env()->tantivy_->search(spaceId_,
                                               part,
                                               query_.fulltext_index_name_ref().value_or(""),
                                               query_.fulltext_field_ref().value_or(""),
                                               query_.fulltext_value_ref().value_or(""),
                                               query_.fulltext_mode_ref().value_or(0),
                                               hits,
                                               limit);
    return error_ == ErrorCode::SUCCEEDED;
  };
  if (limit_ == 0) {
    search(0, hits_);
    return error_;
  }
  auto read = [&](const std::string& key, std::string& data) {
    const auto code = context_->env()->kvstore_->get(spaceId_, part, key, &data);
    if (code == ErrorCode::E_KEY_NOT_FOUND) return fulltext::TopKRead::Missing;
    if (code == ErrorCode::SUCCEEDED) return fulltext::TopKRead::Found;
    error_ = code;
    return fulltext::TopKRead::Error;
  };
  auto decode = [&](const std::string& key, std::string& id, bool& canonical) {
    if (!NebulaKeyUtils::isTag(context_->vIdLen(), key) ||
        NebulaKeyUtils::getTagId(context_->vIdLen(), key) != context_->tagId_)
      return false;
    canonical = fulltext::decodeCanonicalVid(
        NebulaKeyUtils::getVertexId(context_->vIdLen(), key).str(), id);
    return true;
  };
  const auto result = fulltext::prepareTopK(limit_, search, read, decode, candidates_, fallback_);
  if (result == fulltext::TopKResult::InvalidData) error_ = ErrorCode::E_INVALID_DATA;
  return error_;
}
IndexNode::Result IndexTantivyScanNode::doNext() {
  if (error_ != ErrorCode::SUCCEEDED) return Result(error_);
  if (limit_ != 0) {
    if (cursor_ == candidates_.size()) return Result();
    const auto& candidate = candidates_[cursor_++];
    return readRow(candidate.hit, candidate.data);
  }
  while (cursor_ < hits_.size()) {
    const auto& hit = hits_[cursor_++];
    std::string data;
    auto code = context_->env()->kvstore_->get(spaceId_, part_, hit.key, &data);
    if (code == ErrorCode::E_KEY_NOT_FOUND) continue;
    if (code != ErrorCode::SUCCEEDED) return Result(code);
    return readRow(hit, data);
  }
  return Result();
}
IndexNode::Result IndexTantivyScanNode::readRow(const plugin::TantivyIndex::Hit& hit,
                                                const std::string& data) {
  auto* env = context_->env();
  auto vidLen = context_->vIdLen();
  auto decodeId = [&](folly::StringPiece id) -> Value {
    if (context_->isIntId()) {
      int64_t value = 0;
      memcpy(&value, id.data(), sizeof(value));
      return Value(value);
    }
    return Value(id.subpiece(0, id.find_first_of('\0')).str());
  };
  auto reader = context_->isEdge() ? RowReaderWrapper::getEdgePropReader(
                                         env->schemaMan_, spaceId_, context_->edgeType_, data)
                                   : RowReaderWrapper::getTagPropReader(
                                         env->schemaMan_, spaceId_, context_->tagId_, data);
  if (!reader) return Result(ErrorCode::E_INVALID_DATA);
  auto schema = context_->isEdge() ? env->schemaMan_->getEdgeSchema(spaceId_, context_->edgeType_)
                                   : env->schemaMan_->getTagSchema(spaceId_, context_->tagId_);
  if (!schema) return Result(ErrorCode::E_INVALID_DATA);
  Row row;
  for (const auto& column : columns_) {
    if (column == "_score" && query_.fulltext_mode_ref().value_or(0) == 0) {
      row.emplace_back(hit.score);
    } else if (column == kVid) {
      row.emplace_back(decodeId(NebulaKeyUtils::getVertexId(vidLen, hit.key)));
    } else if (column == kTag) {
      row.emplace_back(context_->tagId_);
    } else if (column == kSrc) {
      row.emplace_back(decodeId(NebulaKeyUtils::getSrcId(vidLen, hit.key)));
    } else if (column == kDst) {
      row.emplace_back(decodeId(NebulaKeyUtils::getDstId(vidLen, hit.key)));
    } else if (column == kRank) {
      row.emplace_back(NebulaKeyUtils::getRank(vidLen, hit.key));
    } else if (column == kType) {
      row.emplace_back(context_->edgeType_);
    } else {
      auto field = schema->field(column);
      if (!field) return Result(ErrorCode::E_INVALID_PARM);
      auto value = QueryUtils::readValue(reader.get(), column, field);
      if (!value.ok()) return Result(ErrorCode::E_INVALID_DATA);
      row.emplace_back(std::move(value).value());
    }
  }
  return Result(std::move(row));
}
}  // namespace nebula::storage
