#include <rime/component.h>
#include <rime/registry.h>
#include <rime_api.h>

#include "userdb_cleaner.hpp"

namespace rime {

static void rime_custom_initialize() {
  Registry& r = Registry::instance();
  r.Register("userdb_cleaner", new Component<UserdbCleaner>);
}

static void rime_custom_finalize() {}

RIME_REGISTER_MODULE(custom)

}  // namespace rime
