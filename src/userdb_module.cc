#include <rime/component.h>
#include <rime/registry.h>
#include <rime_api.h>

#include "userdb_cleaner.h"

namespace rime {

static void rime_userdbcleaner_initialize() {
  Registry &r = Registry::instance();
  r.Register("userdb_cleaner", new Component<UserdbCleaner>);
}

static void rime_userdbcleaner_finalize() {}

RIME_REGISTER_MODULE(userdbcleaner)

} // namespace rime