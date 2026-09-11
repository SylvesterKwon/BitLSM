#include "test_util/checked_bitlsm.h"

#include <cstring>
#include <sstream>

#include "bit_lsm_utils.h"  // DecodeAttr (production read path)

namespace bit_lsm {
namespace {

// Independent payload extractor from the documented v3 value layout:
// [null bitmap][var_end:u32 x n_variable][fixed slots][variable bytes][payload]
// (implemented from the layout spec, not via ValueLayout, so it stays an
// independent check of the production encoder)
std::string PayloadOf(std::string_view buf, const BitLSMOptions& options) {
  std::uint32_t n_var = 0, n_nullable = 0, fixed_bytes = 0;
  for (const AttrSpec& s : options.attr_specs) {
    if (s.nullable) n_nullable++;
    if (s.physical_type == PhysicalType::kVarBinary)
      n_var++;
    else
      fixed_bytes += s.width;
  }
  const std::uint32_t null_bytes = (n_nullable + 7) / 8;
  const std::uint32_t var_base =
      null_bytes + n_var * static_cast<std::uint32_t>(sizeof(std::uint32_t)) +
      fixed_bytes;
  std::uint32_t last_end = 0;
  if (n_var > 0)
    std::memcpy(&last_end,
                buf.data() + null_bytes + (n_var - 1) * sizeof(std::uint32_t),
                sizeof(std::uint32_t));
  return std::string(buf.substr(var_base + last_end));
}

std::string AttrToString(const Attr& a) {
  if (std::holds_alternative<std::monostate>(a)) return "NULL";
  std::ostringstream o;
  if (std::holds_alternative<int64_t>(a)) {
    o << std::get<int64_t>(a);
  } else if (std::holds_alternative<uint64_t>(a)) {
    o << std::get<uint64_t>(a) << "u";
  } else if (std::holds_alternative<double>(a)) {
    o << std::get<double>(a);
  } else {
    o << "'" << std::get<std::string>(a) << "'";
  }
  return o.str();
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

void CheckedBitLSM::HoldSnapshot() {
  if (held_snapshot_ != nullptr) return;
  trace_.push_back("HoldSnapshot()");
  held_snapshot_ = engine_->GetInternalDB()->GetSnapshot();
}

void CheckedBitLSM::ReleaseHeldSnapshot() {
  if (held_snapshot_ == nullptr) return;
  trace_.push_back("ReleaseHeldSnapshot()");
  engine_->GetInternalDB()->ReleaseSnapshot(held_snapshot_);
  held_snapshot_ = nullptr;
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
  if (!it->status().ok())
    ADD_FAILURE() << "BitLSM iterator error: " << it->status().ToString()
                  << " for " << query.ToString() << Context();
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
