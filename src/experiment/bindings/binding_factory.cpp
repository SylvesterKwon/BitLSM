#include "binding.h"
#include "bitlsm_binding.h"
#include "no_index_binding.h"
#include "si_ck_binding.h"
#include "si_eager_binding.h"
#include "si_lu_binding.h"

namespace experiment {

std::unique_ptr<Binding> CreateBinding(const std::string& name) {
  if (name == "bitlsm") return std::make_unique<BitLSMBinding>();
  if (name == "no-index") return std::make_unique<NoIndexBinding>();
  if (name == "si-ck") return std::make_unique<SICKBinding>();
  if (name == "si-lu") return std::make_unique<SILUBinding>();
  if (name == "si-eager") return std::make_unique<SIEagerBinding>();
  return nullptr;
}

}  // namespace experiment
