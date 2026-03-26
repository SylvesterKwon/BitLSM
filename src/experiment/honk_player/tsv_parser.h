#pragma once
#include <fstream>
#include <stdexcept>
#include <string>
#include <variant>

namespace honk {

enum class OpType { WRITE, UPDATE, READ, PAUSE };

struct WriteOp {
  std::string pk;
  std::string json;
};

struct ReadOp {
  std::string json;
};

struct PauseOp {
  double seconds;
};

struct Operation {
  OpType type;
  std::variant<WriteOp, ReadOp, PauseOp> data;
};

class TSVReader {
  std::ifstream file_;
  std::string line_;

 public:
  explicit TSVReader(const std::string& path) : file_(path) {
    if (!file_.is_open())
      throw std::runtime_error("Cannot open: " + path);
  }

  bool Next(Operation& op) {
    while (std::getline(file_, line_)) {
      if (line_.empty()) continue;

      size_t t1 = line_.find('\t');
      if (t1 == std::string::npos)
        throw std::runtime_error("Malformed TSV line");

      switch (line_[0]) {
        case 'w':
        case 'u': {
          size_t t2 = line_.find('\t', t1 + 1);
          if (t2 == std::string::npos)
            throw std::runtime_error("Malformed w/u line");
          op.type = (line_[0] == 'w') ? OpType::WRITE : OpType::UPDATE;
          op.data = WriteOp{line_.substr(t1 + 1, t2 - t1 - 1),
                            line_.substr(t2 + 1)};
          return true;
        }
        case 'r':
          op.type = OpType::READ;
          op.data = ReadOp{line_.substr(t1 + 1)};
          return true;
        case 'p':
          op.type = OpType::PAUSE;
          op.data = PauseOp{std::stod(line_.substr(t1 + 1))};
          return true;
        default:
          throw std::runtime_error("Unknown op: " + line_.substr(0, 1));
      }
    }
    return false;
  }
};

}  // namespace honk
