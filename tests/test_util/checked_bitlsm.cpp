#include "test_util/checked_bitlsm.h"

#include <cstring>
#include <sstream>

#include "bit_lsm_utils.h"  // DecodeAttr (production read path)

namespace bit_lsm {
namespace {

// Independent payload extractor from the documented v2 value layout:
// [var_end:u32 x n_cat][cont:8B x n_cont][cat bytes][payload]
// (implemented from the layout spec, not via ValueLayout, so it stays an
// independent check of the production encoder)
std::string PayloadOf(std::string_view buf, const BitLSMOptions& options) {
  std::uint32_t n_cat = 0;
  for (const AttrSpec& s : options.attr_specs)
    if (s.role == AttrRole::UNORDERED) n_cat++;
  std::uint32_t n_cont = options.attr_num - n_cat;
  std::uint32_t cat_base =
      n_cat * static_cast<std::uint32_t>(sizeof(std::uint32_t)) +
      n_cont * static_cast<std::uint32_t>(sizeof(double));
  std::uint32_t last_end = 0;
  if (n_cat > 0)
    std::memcpy(&last_end, buf.data() + (n_cat - 1) * sizeof(std::uint32_t),
                sizeof(std::uint32_t));
  return std::string(buf.substr(cat_base + last_end));
}

std::string AttrToString(const Attr& a) {
  if (std::holds_alternative<double>(a)) {
    std::ostringstream o;
    o << std::get<double>(a);
    return o.str();
  }
  return "'" + std::get<std::string>(a) + "'";
}

std::string RecordToString(const Record& r) {
  std::ostringstream o;
  o << "{";
  for (std::size_t i = 0; i < r.attrs.size(); ++i) {
    if (i) o << ",";
    o << AttrToString(r.attrs[i]);
  }
  o << "|payload=" << r.payload << "}";
  return o.str();
}

}  // namespace

std::string CheckedBitLSM::Context() const {
  std::ostringstream o;
  o << "\n--- repro context ---\n";
  if (has_seed_) o << "BITLSM_TEST_SEED=" << seed_ << "\n";
  o << "ops (" << trace_.size() << "):";
  for (const auto& t : trace_) o << " " << t;
  o << "\n";
  return o.str();
}

::testing::AssertionResult CheckedBitLSM::Put(const std::string& key,
                                              const std::vector<Attr>& attrs,
                                              const std::string& payload) {
  ref_.Put(key, attrs, payload);
  trace_.push_back("Put(" + key + ")");
  rocksdb::Status s = engine_->Put(key, attrs, payload);
  if (!s.ok())
    return ::testing::AssertionFailure()
           << "engine Put(" << key << ") failed: " << s.ToString() << Context();
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult CheckedBitLSM::Delete(const std::string& key) {
  ref_.Delete(key);
  trace_.push_back("Delete(" + key + ")");
  rocksdb::Status s = engine_->Delete(key);
  if (!s.ok())
    return ::testing::AssertionFailure()
           << "engine Delete(" << key << ") failed: " << s.ToString()
           << Context();
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult CheckedBitLSM::PutBatch(
    const std::vector<std::string>& keys,
    const std::vector<std::vector<Attr>>& attrs_list,
    const std::vector<std::string>& payloads) {
  // Mirror in order so duplicate keys within the batch are latest-wins on both
  // sides.
  for (std::size_t i = 0; i < keys.size(); ++i) {
    ref_.Put(keys[i], attrs_list[i], payloads[i]);
    trace_.push_back("Batch.Put(" + keys[i] + ")");
  }
  rocksdb::Status s = engine_->PutBatch(keys, attrs_list, payloads);
  if (!s.ok())
    return ::testing::AssertionFailure()
           << "engine PutBatch failed: " << s.ToString() << Context();
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult CheckedBitLSM::Flush() {
  trace_.push_back("Flush()");
  rocksdb::Status s = engine_->GetInternalDB()->Flush(rocksdb::FlushOptions());
  if (!s.ok())
    return ::testing::AssertionFailure()
           << "Flush failed: " << s.ToString() << Context();
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult CheckedBitLSM::CompactAll() {
  trace_.push_back("CompactAll()");
  rocksdb::Status s = engine_->GetInternalDB()->CompactRange(
      rocksdb::CompactRangeOptions(), nullptr, nullptr);
  if (!s.ok())
    return ::testing::AssertionFailure()
           << "CompactRange failed: " << s.ToString() << Context();
  return ::testing::AssertionSuccess();
}

std::map<std::string, Record> CheckedBitLSM::ScanEngine(BitLSMQuery& query) {
  std::map<std::string, Record> out;
  auto it = engine_->NewIterator(query);
  if (!it) {
    ADD_FAILURE() << "NewIterator returned nullptr (invalid query?) for "
                  << query.ToString() << Context();
    return out;
  }
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    std::string_view val = it->value().ToStringView();
    Record r;
    r.attrs.reserve(options_.attr_num);
    for (std::uint32_t i = 0; i < options_.attr_num; ++i) {
      AttrView av = DecodeAttr(options_, val, i);
      if (std::holds_alternative<double>(av))
        r.attrs.emplace_back(std::get<double>(av));
      else
        r.attrs.emplace_back(std::string(std::get<std::string_view>(av)));
    }
    r.payload = PayloadOf(val, options_);
    out.emplace(std::move(key), std::move(r));
  }
  return out;
}

::testing::AssertionResult CheckedBitLSM::VerifyQuery(BitLSMQuery query) {
  std::map<std::string, Record> expected = ref_.ExpectedResult(query);
  std::map<std::string, Record> actual = ScanEngine(query);

  std::ostringstream diff;
  bool ok = true;
  for (const auto& [k, er] : expected) {
    auto it = actual.find(k);
    if (it == actual.end()) {
      ok = false;
      diff << "  MISSING (only in reference): " << k << " "
           << RecordToString(er) << "\n";
    } else if (!(it->second == er)) {
      ok = false;
      diff << "  MISMATCH " << k << " ref=" << RecordToString(er)
           << " engine=" << RecordToString(it->second) << "\n";
    }
  }
  for (const auto& [k, ar] : actual) {
    if (expected.find(k) == expected.end()) {
      ok = false;
      diff << "  EXTRA (only in engine): " << k << " " << RecordToString(ar)
           << "\n";
    }
  }
  if (ok) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure()
         << "query " << query.ToString() << " mismatch (expected "
         << expected.size() << " rows, engine returned " << actual.size()
         << "):\n"
         << diff.str() << Context();
}

::testing::AssertionResult CheckedBitLSM::VerifyFullScan() {
  return VerifyQuery(BitLSMQuery());
}

}  // namespace bit_lsm
