#include "test_util/checked_bitlsm.h"

#include <cstring>
#include <sstream>

#include "bit_lsm_utils.h"  // DecodeAttr (production read path)

namespace bit_lsm {
namespace {

// Independent payload extractor from the documented value layout:
// [attr_cnt:u32][offset[0..N]:u32][payload_offset:u32][data...]
std::string PayloadOf(std::string_view buf, std::uint32_t attr_cnt) {
  std::uint32_t payload_off_pos =
      sizeof(std::uint32_t) + attr_cnt * sizeof(std::uint32_t);
  std::uint32_t payload_off;
  std::memcpy(&payload_off, buf.data() + payload_off_pos,
              sizeof(std::uint32_t));
  return std::string(buf.substr(payload_off));
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
      AttrView av = DecodeAttr(options_.attr_types[i], val, i);
      if (std::holds_alternative<double>(av))
        r.attrs.emplace_back(std::get<double>(av));
      else
        r.attrs.emplace_back(std::string(std::get<std::string_view>(av)));
    }
    r.payload = PayloadOf(val, options_.attr_num);
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
