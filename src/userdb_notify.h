#ifndef USERDB_NOTIFY_H_
#define USERDB_NOTIFY_H_

namespace rime {

struct UserdbCleanStats;

void NotifyUserdbCleanResult(const UserdbCleanStats &stats,
                             bool full_information_display);

} // namespace rime

#endif // USERDB_NOTIFY_H_