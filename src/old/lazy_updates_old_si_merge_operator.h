// Deprecated: performance issue
class LazyUpdatesSIMergeOperator : public AssociativeMergeOperator {
public:
  bool Merge(const Slice& key, const Slice* existing_value, const Slice& value,
             string* new_value, Logger* logger) const override {
    if (existing_value) {
      MergeIndexValue(existing_value, &value, new_value);
    } else {
      new_value->assign(value.data(), value.size());
    }
    return true;
  }

  const char* Name() const override { return "LazyUpdatesSIMergeOperator"; }
};
