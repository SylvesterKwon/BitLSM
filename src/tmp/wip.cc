#include <string>

#include "rocksdb/table_properties.h"

using namespace std;
using namespace rocksdb;

class FastBitsetBuilder : public TablePropertiesCollector {
   private:
    string bitmap_;               // 완성된 비트맵을 바로 저장할 곳
    unsigned char current_byte_;  // 현재 조립 중인 1바이트 (8비트)
    int bit_index_;               // 현재 바이트의 몇 번째 비트인지 (0~7)
    uint64_t count_;              // (선택) 총 갯수 세기용

   public:
    // 생성자에서 초기화
    FastBitsetBuilder() : current_byte_(0), bit_index_(0), count_(0) {}

    Status AddUserKey(const Slice& key, const Slice& value, EntryType type,
                      SequenceNumber seq, uint64_t file_size) override {
        // 1. 조건 체크 (Value가 있고, 첫 글자가 '1'인가?)
        bool is_target =
            (type == kTypeValue && value.size() > 0 && value[0] == '1');

        // 2. 비트 조립 (Bit Manipulation)
        if (is_target) {
            // 해당 비트 위치를 1로 만듦 (OR 연산)
            current_byte_ |= (1 << bit_index_);
        }
        // (조건이 아니면 0이어야 하므로 아무것도 안 해도 됨, 기본이 0이라서)

        bit_index_++;
        count_++;

        // 3. 8비트가 꽉 차면 문자열에 저장하고 초기화
        if (bit_index_ == 8) {
            bitmap_.push_back(static_cast<char>(current_byte_));
            current_byte_ = 0;
            bit_index_ = 0;
        }

        return Status::OK();
    }

    Status Finish(UserCollectedProperties* properties) override {
        // 4. 남은 자투리 비트가 있으면 마지막 바이트 저장
        if (bit_index_ > 0) {
            bitmap_.push_back(static_cast<char>(current_byte_));
        }

        // 5. 바로 저장 (변환 과정 없음!)
        properties->insert({"value_bitmap", bitmap_});

        // 메타데이터에 총 갯수도 같이 넣어두면 나중에 읽을 때 편합니다.
        properties->insert({"total_key_count", to_string(count_)});

        return Status::OK();
    }

    const char* Name() const override { return "FastBitsetBuilder"; }

    UserCollectedProperties GetReadableProperties() const override {
        return UserCollectedProperties{};
    }
};