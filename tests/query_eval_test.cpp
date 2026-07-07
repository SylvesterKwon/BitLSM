#include <gtest/gtest.h>
#include <rocksdb/slice.h>

#include <string>
#include <vector>

#include "bit_lsm_option.h"
#include "bit_lsm_query.h"
#include "bit_lsm_utils.h"

using namespace bit_lsm;

namespace {
BitLSMOptions MakeOptions(std::vector<AttrSpec> types) {
  BitLSMOptions options;
  options.attr_num = static_cast<uint32_t>(types.size());
  options.attr_specs = std::move(types);
  options.read_seqno = 0;
  options.rho = 0.5;
  return options;
}
// 연속형 1개 + 범주형 1개 스키마로 한 행을 인코딩.
std::string Encode2(const BitLSMOptions& options, double cont,
                    const std::string& cat) {
  std::string out;
  EncodeValue(options, {cont, cat}, "p", out);
  return out;
}
}  // namespace

TEST(QueryEval, EmptyQueryMatchesAll) {
  BitLSMOptions options = MakeOptions({AttrRole::ORDERED});
  std::string out;
  EncodeValue(options, {42.0}, "p", out);

  BitLSMQuery query;  // 빈 쿼리 → 항상 true
  EXPECT_TRUE(query.CheckCondition(rocksdb::Slice(out), options));
}

TEST(QueryEval, OrderedGreaterEqual) {
  BitLSMOptions options = MakeOptions({AttrRole::ORDERED, AttrRole::UNORDERED});
  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});

  EXPECT_TRUE(query.CheckCondition(rocksdb::Slice(Encode2(options, 15.0, "x")),
                                   options));
  EXPECT_FALSE(query.CheckCondition(rocksdb::Slice(Encode2(options, 5.0, "x")),
                                    options));
  // >= 는 경계값 포함.
  EXPECT_TRUE(query.CheckCondition(rocksdb::Slice(Encode2(options, 10.0, "x")),
                                   options));
}

TEST(QueryEval, OrClauseSameAttribute) {
  BitLSMOptions options = MakeOptions({AttrRole::ORDERED});
  // (a0 == 1.0 OR a0 == 2.0)
  OrClause clause = {{0, CompareOp::EQUAL, 1.0}, {0, CompareOp::EQUAL, 2.0}};
  BitLSMQuery query(std::vector<OrClause>{clause});

  // 첫 번째 OR 분기(a0==1.0) 단락 경로도 확인.
  std::string first_match;
  EncodeValue(options, {1.0}, "p", first_match);
  EXPECT_TRUE(query.CheckCondition(rocksdb::Slice(first_match), options));

  std::string match;
  EncodeValue(options, {2.0}, "p", match);
  EXPECT_TRUE(query.CheckCondition(rocksdb::Slice(match), options));

  std::string no_match;
  EncodeValue(options, {3.0}, "p", no_match);
  EXPECT_FALSE(query.CheckCondition(rocksdb::Slice(no_match), options));
}

TEST(QueryEval, AndOfClausesMixedTypes) {
  BitLSMOptions options = MakeOptions({AttrRole::ORDERED, AttrRole::UNORDERED});
  // (a0 >= 10) AND (a1 == "apple")
  BitLSMQuery query(std::vector<OrClause>{
      OrClause{{0, CompareOp::GREATER_EQUAL, 10.0}},
      OrClause{{1, CompareOp::EQUAL, std::string("apple")}}});

  EXPECT_TRUE(query.CheckCondition(
      rocksdb::Slice(Encode2(options, 15.0, "apple")), options));
  EXPECT_FALSE(query.CheckCondition(
      rocksdb::Slice(Encode2(options, 15.0, "banana")), options));
  EXPECT_FALSE(query.CheckCondition(
      rocksdb::Slice(Encode2(options, 5.0, "apple")), options));
}

// Workload: a sweep of queries (empty, single-cond, same-attr OR, cross-attr
//           OR, multi-clause AND, boundary >= / <) evaluated over a grid of
//           rows by both CheckCondition (reference) and CompiledQuery::Eval
//           (engine path).
// Threat: compiled predicate slots, comparand storage, or op mapping diverge
//         from the reference semantics, silently corrupting engine results.
TEST(QueryEval, CompiledQueryMatchesCheckCondition) {
  BitLSMOptions options = MakeOptions({AttrRole::ORDERED, AttrRole::UNORDERED});

  std::vector<BitLSMQuery> queries;
  queries.emplace_back();  // empty query matches all
  queries.emplace_back(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  queries.emplace_back(
      std::vector<QueryCondition>{{1, CompareOp::EQUAL, std::string("apple")}});
  queries.emplace_back(std::vector<OrClause>{
      {{0, CompareOp::LESS, 5.0}, {0, CompareOp::GREATER, 20.0}}});
  queries.emplace_back(
      std::vector<OrClause>{{{0, CompareOp::GREATER_EQUAL, 10.0},
                             {1, CompareOp::EQUAL, std::string("pear")}}});
  queries.emplace_back(std::vector<OrClause>{
      OrClause{{0, CompareOp::LESS_EQUAL, 15.0}},
      OrClause{{1, CompareOp::EQUAL, std::string("apple")},
               {1, CompareOp::EQUAL, std::string("pear")}}});

  const double conts[] = {4.0, 5.0, 10.0, 15.0, 20.0, 25.0};
  const char* cats[] = {"apple", "banana", "pear", ""};

  for (BitLSMQuery& q : queries) {
    ASSERT_TRUE(q.Validate(options).ok());
    CompiledQuery compiled(q, options);
    for (double c : conts) {
      for (const char* s : cats) {
        std::string row = Encode2(options, c, s);
        EXPECT_EQ(compiled.Eval(rocksdb::Slice(row)),
                  q.CheckCondition(rocksdb::Slice(row), options))
            << q.ToString() << " on (" << c << ", '" << s << "')";
      }
    }
  }
}
