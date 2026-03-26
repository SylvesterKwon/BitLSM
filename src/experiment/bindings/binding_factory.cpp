#include "binding.h"
#include "bitlsm_binding.h"
#include "noindex_binding.h"
#include "sick_binding.h"
#include "silu_binding.h"

namespace experiment {

std::unique_ptr<Binding> CreateBinding(const std::string& name) {
  if (name == "bitlsm") return std::make_unique<BitLSMBinding>();
  if (name == "no-index") return std::make_unique<NoIndexBinding>();
  if (name == "si-ck") return std::make_unique<SICKBinding>();
  if (name == "si-lu") return std::make_unique<SILUBinding>();
  return nullptr;
}

}  // namespace experiment
