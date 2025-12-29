#include "standalone_secondary_index_experiment.h"
#include <cstring>

using namespace std;
using namespace rocksdb;

class EagerUpdates : public StandaloneSecondaryIndexExperiment {
public:
  virtual Status Insert(const Slice& key, const Slice& value) {
    return Status::OK();
  };
  virtual Status Get(const Slice& key, const IndexType index_type,
                     std::string* value) {
    return Status::OK();
  };
};