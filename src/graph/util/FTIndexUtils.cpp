// Copyright (c) 2021 vesoft inc. All rights reserved.
//
// This source code is licensed under Apache 2.0 License.

#include "graph/util/FTIndexUtils.h"

#include "common/expression/Expression.h"
#include "graph/util/ExpressionUtils.h"

DECLARE_uint32(ft_request_retry_times);

namespace nebula {
namespace graph {

bool FTIndexUtils::needTextSearch(const Expression* expr) {
  switch (expr->kind()) {
    case Expression::Kind::kESQUERY: {
      return true;
    }
    default:
      return false;
  }
}

bool FTIndexUtils::buildNativeIndexContext(meta::MetaClient* client,
                                           GraphSpaceID spaceId,
                                           int32_t schemaId,
                                           bool isEdge,
                                           const Expression* filter,
                                           storage::cpp2::IndexQueryContext* context) {
  if (filter == nullptr || client == nullptr) {
    return false;
  }
  auto indexes = client->getFTIndexBySpaceFromCache(spaceId);
  return indexes.ok() &&
         buildNativeIndexContext(indexes.value(), spaceId, schemaId, isEdge, filter, context);
}

bool FTIndexUtils::buildNativeIndexContext(
    const std::unordered_map<std::string, meta::cpp2::FTIndex>& indexes,
    GraphSpaceID spaceId,
    int32_t schemaId,
    bool isEdge,
    const Expression* filter,
    storage::cpp2::IndexQueryContext* context) {
  if (filter == nullptr) {
    return false;
  }
  if (filter->kind() == Expression::Kind::kLogicalAnd) {
    for (const auto* operand : static_cast<const LogicalExpression*>(filter)->operands()) {
      if (buildNativeIndexContext(indexes, spaceId, schemaId, isEdge, operand, context)) {
        // The selected conjunct only generates candidates. Keep every conjunct for recheck.
        context->filter_ref() = Expression::encode(*filter);
        return true;
      }
    }
    return false;
  }
  int32_t mode;
  switch (filter->kind()) {
    case Expression::Kind::kContains:
      mode = 1;
      break;
    case Expression::Kind::kStartsWith:
      mode = 2;
      break;
    case Expression::Kind::kEndsWith:
      mode = 3;
      break;
    default:
      return false;
  }
  const auto* predicate = static_cast<const RelationalExpression*>(filter);
  const auto propertyKind =
      isEdge ? Expression::Kind::kEdgeProperty : Expression::Kind::kTagProperty;
  if (predicate->left()->kind() != propertyKind ||
      predicate->right()->kind() != Expression::Kind::kConstant) {
    return false;
  }
  const auto& value = static_cast<const ConstantExpression*>(predicate->right())->value();
  if (!value.isStr()) {
    return false;
  }
  const auto& field = static_cast<const PropertyExpression*>(predicate->left())->prop();
  const std::string* selected = nullptr;
  for (const auto& entry : indexes) {
    const auto& index = entry.second;
    const auto& schema = index.get_depend_schema();
    const bool indexIsEdge = schema.getType() == nebula::cpp2::SchemaID::Type::edge_type;
    if (indexIsEdge != isEdge || index.get_space_id() != spaceId) {
      continue;
    }
    const auto id = isEdge ? schema.get_edge_type() : schema.get_tag_id();
    const auto& fields = index.get_fields();
    if (id != schemaId || std::find(fields.begin(), fields.end(), field) == fields.end()) {
      continue;
    }
    // Stable selection when several indexes cover the same field.
    if (selected == nullptr || entry.first < *selected) {
      selected = &entry.first;
    }
  }
  if (selected == nullptr) {
    return false;
  }
  *context = storage::cpp2::IndexQueryContext();
  context->fulltext_index_name_ref() = *selected;
  context->fulltext_field_ref() = field;
  context->fulltext_value_ref() = value.getStr();
  context->fulltext_mode_ref() = mode;
  context->filter_ref() = Expression::encode(*filter);
  return true;
}

StatusOr<::nebula::plugin::ESAdapter> FTIndexUtils::getESAdapter(meta::MetaClient* client) {
  auto tcs = client->getServiceClientsFromCache(meta::cpp2::ExternalServiceType::ELASTICSEARCH);
  if (!tcs.ok()) {
    return tcs.status();
  }
  if (tcs.value().empty()) {
    return Status::SemanticError("No text search client found");
  }
  std::vector<::nebula::plugin::ESClient> clients;
  for (const auto& c : tcs.value()) {
    std::string protocol = c.conn_type_ref().has_value() ? *c.get_conn_type() : "http";
    std::string address = c.host.toRawString();
    std::string user = c.user_ref().has_value() ? *c.user_ref() : "";
    std::string password = c.pwd_ref().has_value() ? *c.pwd_ref() : "";
    clients.emplace_back(HttpClient::instance(), protocol, address, user, password);
  }
  return ::nebula::plugin::ESAdapter(std::move(clients));
}

}  // namespace graph
}  // namespace nebula
