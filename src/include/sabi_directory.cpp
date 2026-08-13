#include "sabi_directory.h"

#include <iostream>

#include "file/filename.h"
#include "sabi.h"
#include "sabi_blob_source.h"
#include "table/block_based/block_based_table_reader.h"
#include "util/coding.h"

namespace bit_lsm {
using namespace rocksdb;

namespace {
// Footer: [u32 directory_off][u32 version][u32 magic].
constexpr uint32_t kFooterBytes = 3 * sizeof(uint32_t);
constexpr uint32_t kSectionARecordBytes = 3 * sizeof(uint32_t);
}  // namespace

size_t SABIDirectory::ApproximateMemoryUsage() const {
  size_t usage = sizeof(*this);
  usage += schema.roles.capacity() * sizeof(AttrRole);
  usage += bitmap_index.bitmap_nums.capacity() * sizeof(uint32_t);
  usage += distinct_cnts.capacity() * sizeof(uint64_t);
  usage += bitmap_offsets.capacity() * sizeof(uint32_t);
  usage += data_entries_cnt_psum.capacity() * sizeof(uint32_t);
  usage += block_handles.capacity() * sizeof(BlockHandle);
  const auto& binning_policy = bitmap_index.binning_policy;
  usage += binning_policy.capacity() *
           sizeof(std::decay_t<decltype(binning_policy)>::value_type);
  for (const auto& policy : binning_policy) {
    if (const auto* ord = std::get_if<std::vector<uint64_t>>(&policy)) {
      usage += ord->capacity() * sizeof(uint64_t);
    } else if (const auto* cat =
                   std::get_if<std::vector<std::pair<std::string, uint32_t>>>(
                       &policy)) {
      usage += cat->capacity() * sizeof(std::pair<std::string, uint32_t>);
      // Count heap allocations only; short values live inline (SSO).
      static const size_t kSSOCapacity = std::string().capacity();
      for (const auto& entry : *cat)
        if (entry.first.capacity() > kSSOCapacity)
          usage += entry.first.capacity() + 1;  // + NUL
    }
  }
  return usage;
}

bool BuildSABIDirectory(SABIBlobSource& src, SABIDirectory* out) {
  const uint32_t blob_size = static_cast<uint32_t>(src.BlobSize());
  if (blob_size < kFooterBytes) return false;
  std::string buf;

  const char* footer = src.Read(blob_size - kFooterBytes, kFooterBytes, buf);
  if (!src.ok() || DecodeFixed32(footer + 8) != kSABIFooterMagic) return false;
  const uint32_t directory_off = DecodeFixed32(footer);
  const uint32_t version = DecodeFixed32(footer + 4);
  if (version < kBitLSMMinReadFormatVersion || version > kBitLSMFormatVersion ||
      directory_off >= blob_size) {
    return false;
  }

  // The directory runs from directory_off to the footer, and its fields are
  // only sized by ones that precede them, so read it whole and walk forward
  // exactly as SABIReader does.
  std::string dir_buf;
  const uint32_t dir_len = blob_size - kFooterBytes - directory_off;
  const char* dir = src.Read(directory_off, dir_len, dir_buf);
  if (!src.ok()) return false;

  const uint32_t attr_num = DecodeFixed32(dir);
  dir += sizeof(uint32_t);
  out->schema.roles.resize(attr_num);
  for (uint32_t i = 0; i < attr_num; ++i)
    out->schema.roles[i] = static_cast<AttrRole>(static_cast<uint8_t>(dir[i]));
  dir += attr_num;

  out->bitmap_index.bitmap_nums.resize(attr_num);
  uint32_t total_bins = 0;
  for (uint32_t i = 0; i < attr_num; ++i) {
    out->bitmap_index.bitmap_nums[i] = DecodeFixed32(dir);
    total_bins += out->bitmap_index.bitmap_nums[i];
    dir += sizeof(uint32_t);
  }
  const uint32_t index_entries_cnt = DecodeFixed32(dir);
  dir += sizeof(uint32_t);

  out->distinct_cnts.assign(attr_num, 0);
  if (version >= 6) {
    for (uint32_t i = 0; i < attr_num; ++i) {
      out->distinct_cnts[i] = DecodeFixed64(dir);
      dir += sizeof(uint64_t);
    }
  }

  std::vector<uint32_t> policy_offsets(attr_num + 1);
  for (uint32_t i = 0; i <= attr_num; ++i) {
    policy_offsets[i] = DecodeFixed32(dir);
    dir += sizeof(uint32_t);
  }

  const uint32_t bitmaps_cnt = total_bins + 1;  // + tombstone
  out->bitmap_offsets.resize(bitmaps_cnt + 1);
  for (uint32_t i = 0; i <= bitmaps_cnt; ++i) {
    out->bitmap_offsets[i] = DecodeFixed32(dir);
    dir += sizeof(uint32_t);
  }

  // Binning policies. ORDERED bodies are headerless (bin_count + 1
  // boundaries); UNORDERED bodies carry their entry count. Same decode as
  // SABIReader, over ranges instead of a materialised blob.
  out->bitmap_index.binning_policy.resize(attr_num);
  for (uint32_t i = 0; i < attr_num; ++i) {
    std::string policy_buf;
    const uint32_t policy_len = policy_offsets[i + 1] - policy_offsets[i];
    const char* ptr = src.Read(policy_offsets[i], policy_len, policy_buf);
    if (!src.ok()) return false;
    const char* end = ptr + policy_len;

    if (out->schema.roles[i] == AttrRole::UNORDERED) {
      uint32_t entry_count = DecodeFixed32(ptr);
      ptr += sizeof(uint32_t);
      std::vector<std::pair<std::string, uint32_t>> policy(entry_count);
      for (uint32_t j = 0; j < entry_count; ++j) {
        uint32_t key_len = 0;
        const char* key_start = GetVarint32Ptr(ptr, end, &key_len);
        if (key_start == nullptr) return false;
        policy[j] = {std::string(key_start, key_len),
                     DecodeFixed32(key_start + key_len)};
        ptr = key_start + key_len + sizeof(uint32_t);
      }
      out->bitmap_index.binning_policy[i] = std::move(policy);
    } else {
      const uint32_t boundary_count = out->bitmap_index.bitmap_nums[i] + 1;
      std::vector<uint64_t> policy(boundary_count);
      for (uint32_t j = 0; j < boundary_count; ++j)
        policy[j] = DecodeFixed64(ptr + j * sizeof(uint64_t));
      out->bitmap_index.binning_policy[i] = std::move(policy);
    }
  }

  // Section A, one read: contiguous at the head of the blob, and the whole
  // table is needed to map a rowId to its data block.
  std::string section_a;
  const char* recs =
      src.Read(0, index_entries_cnt * kSectionARecordBytes, section_a);
  if (!src.ok()) return false;
  out->data_entries_cnt_psum.resize(index_entries_cnt);
  out->block_handles.resize(index_entries_cnt);
  for (uint32_t i = 0; i < index_entries_cnt; ++i) {
    const char* p = recs + i * kSectionARecordBytes;
    out->data_entries_cnt_psum[i] = DecodeFixed32(p);
    out->block_handles[i].set_offset(DecodeFixed32(p + sizeof(uint32_t)));
    out->block_handles[i].set_size(DecodeFixed32(p + 2 * sizeof(uint32_t)));
  }
  return true;
}

const SABIDirectory* SABIDirectoryRegistry::Get(uint64_t file_number,
                                                BlockBasedTable* bbt,
                                                std::shared_ptr<Cache> cache) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(file_number);
    if (it != map_.end()) return it->second.get();
  }

  const BlockBasedTable::Rep* rep = bbt->get_rep();
  if (rep->udi_handle.IsNull() || rep->udi_handle.size() == 0) return nullptr;

  auto dir = std::make_unique<SABIDirectory>();
  dir->blob_offset = rep->udi_handle.offset();
  dir->blob_size = rep->udi_handle.size();

  SABIBlobSource src(rep->file.get(), dir->blob_offset, dir->blob_size,
                     file_number, cache);
  if (!BuildSABIDirectory(src, dir.get())) {
    std::cerr << "[SABIDirectoryRegistry] file " << file_number
              << ": could not parse the SABI blob directory\n";
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mu_);
  // Another thread may have built the same directory while this one read; keep
  // whichever landed first so callers never see two copies for one file.
  auto [it, inserted] = map_.emplace(file_number, std::move(dir));
  (void)inserted;
  return it->second.get();
}

void SABIDirectoryRegistry::Drop(uint64_t file_number) {
  std::lock_guard<std::mutex> lock(mu_);
  map_.erase(file_number);
}

size_t SABIDirectoryRegistry::ApproximateMemoryUsage() const {
  std::lock_guard<std::mutex> lock(mu_);
  size_t usage = sizeof(*this);
  for (const auto& [num, dir] : map_) {
    (void)num;
    usage += dir->ApproximateMemoryUsage();
  }
  return usage;
}

size_t SABIDirectoryRegistry::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return map_.size();
}

void SABIRegistryCleaner::OnTableFileDeleted(
    const TableFileDeletionInfo& info) {
  uint64_t number = 0;
  FileType type;
  // The listener reports a path, not a number; everything else about the file
  // is already gone by now.
  std::string name = info.file_path;
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);
  if (ParseFileName(name, &number, &type) && type == kTableFile) {
    registry_->Drop(number);
  }
}

}  // namespace bit_lsm
