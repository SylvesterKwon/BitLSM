#pragma once

#include <map>
#include <string>
#include <variant>
#include <vector>

#include "bit_lsm_option.h"
#include "bit_lsm_query.h"

namespace bit_lsm {

// ORDERED = int64/uint64/double per spec; UNORDERED = string
using Attr = std::variant<int64_t, uint64_t, double, std::string>;

// A logical record as the test believes it should be.
struct Record {
  std::vector<Attr> attrs;
  std::string payload;
  bool operator==(const Record& o) const {
    return attrs == o.attrs && payload == o.payload;
  }
};

// Independent in-memory oracle. Mirrors the LSM's *logical* state and answers
// queries by brute force, WITHOUT calling the engine's CheckCondition.
class ReferenceDB {
 public:
  explicit ReferenceDB(BitLSMOptions options) : options_(std::move(options)) {}

  void Put(const std::string& key, std::vector<Attr> attrs,
           std::string payload);
  void Delete(const std::string& key);
  void Clear();

  std::size_t Size() const { return live_.size(); }
  const std::map<std::string, Record>& live() const { return live_; }

  // Keys (sorted) whose live record satisfies the CNF query, with their
  // records.
  std::map<std::string, Record> ExpectedResult(const BitLSMQuery& query) const;

  // Does this record's attrs satisfy the CNF (AND of OR-clauses)?
  bool Match(const BitLSMQuery& query, const std::vector<Attr>& attrs) const;

 private:
  bool MatchCondition(const QueryCondition& cond,
                      const std::vector<Attr>& attrs) const;

  BitLSMOptions options_;
  std::map<std::string, Record> live_;
};

}  // namespace bit_lsm
