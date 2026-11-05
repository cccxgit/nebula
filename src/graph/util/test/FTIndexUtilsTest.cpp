// Copyright (c) 2026 vesoft inc. All rights reserved.
//
// This source code is licensed under Apache 2.0 License.

#include <gtest/gtest.h>

#include "common/expression/ConstantExpression.h"
#include "common/expression/LogicalExpression.h"
#include "common/expression/PropertyExpression.h"
#include "common/expression/RelationalExpression.h"
#include "graph/util/FTIndexUtils.h"

namespace nebula {
namespace graph {

class FTIndexUtilsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    meta::cpp2::FTIndex index;
    index.space_id_ref() = 1;
    index.depend_schema_ref()->tag_id_ref() = 7;
    index.fields_ref() = {"title", "body"};
    indexes_.emplace("article_text", std::move(index));
  }

  Expression* predicate(Expression::Kind kind,
                        const std::string& field = "body",
                        const std::string& value = "Graph 数据库") {
    return RelationalExpression::makeKind(&pool_,
                                          kind,
                                          TagPropertyExpression::make(&pool_, "article", field),
                                          ConstantExpression::make(&pool_, value));
  }

  bool select(const Expression* filter, storage::cpp2::IndexQueryContext* context) {
    return FTIndexUtils::buildNativeIndexContext(indexes_, 1, 7, false, filter, context);
  }

  ObjectPool pool_;
  std::unordered_map<std::string, meta::cpp2::FTIndex> indexes_;
};

TEST_F(FTIndexUtilsTest, MatchesEveryIndexFieldAndPreservesLiteral) {
  const std::vector<Expression::Kind> kinds = {
      Expression::Kind::kContains, Expression::Kind::kStartsWith, Expression::Kind::kEndsWith};
  for (size_t i = 0; i < kinds.size(); ++i) {
    storage::cpp2::IndexQueryContext context;
    auto* filter = predicate(kinds[i]);
    ASSERT_TRUE(select(filter, &context));
    EXPECT_FALSE(context.index_id_ref().is_set());
    EXPECT_EQ("article_text", *context.fulltext_index_name_ref());
    EXPECT_EQ("body", *context.fulltext_field_ref());
    EXPECT_EQ("Graph 数据库", *context.fulltext_value_ref());
    EXPECT_EQ(i + 1, *context.fulltext_mode_ref());
    EXPECT_EQ(Expression::encode(*filter), context.get_filter());
  }
}

TEST_F(FTIndexUtilsTest, KeepsAllConjunctsForExactRecheck) {
  auto* filter =
      LogicalExpression::makeAnd(&pool_,
                                 predicate(Expression::Kind::kRelEQ, "unindexed", "active"),
                                 predicate(Expression::Kind::kContains));
  storage::cpp2::IndexQueryContext context;
  ASSERT_TRUE(select(filter, &context));
  EXPECT_EQ(Expression::encode(*filter), context.get_filter());
  EXPECT_EQ("body", *context.fulltext_field_ref());
}

TEST_F(FTIndexUtilsTest, NeverUsesOnlyOneOrBranchOrReversesStringOperands) {
  storage::cpp2::IndexQueryContext context;
  auto* disjunction =
      LogicalExpression::makeOr(&pool_,
                                predicate(Expression::Kind::kContains),
                                predicate(Expression::Kind::kRelEQ, "unindexed", "active"));
  EXPECT_FALSE(select(disjunction, &context));
  EXPECT_FALSE(select(predicate(Expression::Kind::kNotContains), &context));
  auto* reversed =
      RelationalExpression::makeContains(&pool_,
                                         ConstantExpression::make(&pool_, "Graph"),
                                         TagPropertyExpression::make(&pool_, "article", "body"));
  EXPECT_FALSE(select(reversed, &context));
}

TEST_F(FTIndexUtilsTest, RejectsWrongSpaceSchemaTypeAndUnindexedField) {
  auto* filter = predicate(Expression::Kind::kContains);
  storage::cpp2::IndexQueryContext context;
  EXPECT_FALSE(FTIndexUtils::buildNativeIndexContext(indexes_, 2, 7, false, filter, &context));
  EXPECT_FALSE(FTIndexUtils::buildNativeIndexContext(indexes_, 1, 8, false, filter, &context));
  EXPECT_FALSE(select(predicate(Expression::Kind::kContains, "missing"), &context));
  auto* edgeFilter =
      RelationalExpression::makeContains(&pool_,
                                         EdgePropertyExpression::make(&pool_, "article", "body"),
                                         ConstantExpression::make(&pool_, "Graph"));
  EXPECT_FALSE(FTIndexUtils::buildNativeIndexContext(indexes_, 1, 7, true, edgeFilter, &context));
}

TEST_F(FTIndexUtilsTest, SelectsDeterministicallyAmongOverlappingIndexes) {
  indexes_.emplace("another_text", indexes_.at("article_text"));
  storage::cpp2::IndexQueryContext context;
  ASSERT_TRUE(select(predicate(Expression::Kind::kContains), &context));
  EXPECT_EQ("another_text", *context.fulltext_index_name_ref());
}

TEST_F(FTIndexUtilsTest, PreservesEmptyLiteralForFullCandidateRecheck) {
  storage::cpp2::IndexQueryContext context;
  ASSERT_TRUE(select(predicate(Expression::Kind::kContains, "body", ""), &context));
  EXPECT_EQ("", *context.fulltext_value_ref());
}

}  // namespace graph
}  // namespace nebula
