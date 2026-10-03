# Rime UserDB Cleaner 插件

一个 Rime 处理器插件：通过输入触发码，物理删除用户词典中已无用的词条，并同步清理所有设备同步目录下的 `.userdb.txt` 快照，防止已删除词条在后续同步中被重新合并。

## 核心特性

- **原地物理删除**：直接遍历本地 LevelDB（`<name>.userdb` 目录）或 `plain_userdb`（`<name>.userdb.txt` 文件），对满足条件的词条调用 `Db::Erase` 进行物理删除，不删除或清空词典本身。
- **全量快照清理**：递归清理同步目录下所有设备目录中的 `*.userdb.txt` 快照（非仅本机），采用“先写临时文件、再原子替换”策略。
- **同步-清理-同步流程**：在插件进程内调用 librime 的 `installation_update` 与 `user_dict_sync`，不依赖 WeaselDeployer 外部进程。
- **判据与 librime 一致**：使用 `UserDictionary::CreateDictEntry` 判断词条是否仍可被检索，避免硬编码阈值和衰减公式，确保与 librime 升级同步。
- **完整审计追踪**：rime 日志逐条记录删除词条；同步目录下的 `userdb_cleaner.txt` 记录全部删除词条（弹窗仅显示前 10 条及总数）。
- **运行防护**：通过同步目录下的 `.userdb_cleaner.lock` 保证单任务运行；日志超过 2 MB 自动轮转；自动清理崩溃残留的 `*.userdb.txt.tmp`。
- **灵活配置**：支持词典黑名单过滤、简略/详细弹窗模式，清理过程在独立线程执行，不阻塞输入。

## 删除判定标准

本地词典与所有快照文件使用同一套判定规则，确保一致性：

1. **`c < clean_threshold`**：`c` 为提交次数，严格小于阈值时删除。默认 `0` 仅删除 `c` 为负的删除标记行；设为 `1` 会同时删除从未使用过的 `c = 0` 词条。
2. **librime 判定为不可见**：调用 `UserDictionary::CreateDictEntry(key, value, present_tick)`，返回空表示该词条永远不会被输入法检索到，无论 `c` 值大小均删除。`present_tick` 取本机该词典的 `/tick`；若本地词典无法打开，则回退使用快照文件头中的 `#@/tick`。

以下两类行不受判定影响：

- **`t = 0` 的行**：从 table/stabledb 导入的短语（如 `custom_phrase`），librime 从不进行衰减淘汰，因此一律保留。
- **无法解析 `c/d/t` 的行**：一律保留，但会计数并在弹窗与日志中报告，避免因格式变化导致静默失效。

## 清理流程

1. **清理前同步**：合并 `sync/` 下所有设备目录的快照至本地词典，再将本地词典全量导出为本机的 `*.userdb.txt`。
2. **备份本机快照**：将 `sync/<本机 installation_id>/*.userdb.txt` 复制为同目录下的 `*.userdb_backup.txt`（覆盖旧备份；仅复制不移动，其他设备目录不受影响）。此步骤须在同步之后执行，因为 `user_data_sync_dir() = sync_dir / user_id` 依赖 `installation_update` 的结果。
3. **清理本地词典**：扫描用户数据目录，按黑名单过滤，使用 `QueryAll` 原始游标逐行判定，命中则直接 `Erase`（包括第 1 步合并进来的其他设备词条）。
4. **清理同步快照**：对 `sync/` 下所有 `*.userdb.txt` 删除同样判定的行，并清理残留的 `*.userdb.txt.tmp`。
5. **清理后同步**：合并已清理的快照并再次导出，确保发布出去的快照与本地状态一致。

整个流程由 `sync` 目录根下的 `.userdb_cleaner.lock` 保护，同一时刻仅允许一个清理任务运行。

## 安装与配置

### 1. 编译插件

将插件编译为 Rime 插件模块，确保链接 Rime 核心库（`UserDictionary::CreateDictEntry` 为 `RIME_DLL` 导出符号）。

### 2. 配置方案文件（如 `*.schema.yaml`）

```yaml
engine/processors:
  - userdb_cleaner                      # 置于 speller 之后

userdb_cleaner:
  trigger_input: "/clean"               # 触发清理的输入串，默认 "/del"
  clean_threshold: 1                    # 删除条件：c < clean_threshold，默认 0
  full_information_display: true        # 弹窗是否显示详细信息，默认 false
  exclude_userdb_list: []               # 黑名单：不参与清理的词典，空 = 全部清理
```

黑名单也可使用列表语法：

```yaml
userdb_cleaner:
  exclude_userdb_list: ["rime_sheep_pro", "custom"]
```

### 3. 配置项说明

| 配置项 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `trigger_input` | 字符串 | `"/del"` | 输入该串后触发清理；设为空字符串则永不触发 |
| `clean_threshold` | 整数 | `0` | 删除条件 `c < clean_threshold`；此外，无论阈值多少，librime 判定为不可见的行一样会被清除。`t = 0` 的短语行一律保留，无法解析 `c/d/t` 的行保留并计数上报 |
| `full_information_display` | 布尔 | `false` | `false` 仅显示统计信息；`true` 额外显示扫描/备份数量、处理的词典与文件、删除的词条（最多 10 条，并标注总数） |
| `exclude_userdb_list` | 字符串数组 | 空（全部清理） | **黑名单**：不参与清理的词典名，可带或不带 `.userdb` / `.userdb.txt` 后缀。作用于本地清理、快照备份与快照清理；黑名单词典的快照既不备份也不清理，但同步仍会合并它们 |

## 输出说明

**弹窗通知**（Windows 使用原生 MessageBox，macOS 使用 osascript，Linux 依次尝试 zenity / kdialog / notify-send，均不可用时写入日志）

- 默认：清理完成 → 用户词典移除数、同步快照删除行数、耗时、删除记录文件路径，以及两次同步结果。
- 若存在跳过行，追加一行：`跳过 N 行 t=0 短语、N 行无法解析的行`。
- `full_information_display: true` 时额外显示：已扫描快照数、备份的本机快照数、处理的词典/文件列表、删除的词条（最多 10 条，标题旁标注 `( N条 )` 总数）。
- 无法获取运行锁时，仅提示「另一个清理任务正在运行」。

**rime 日志**：每个被删除的词条记录一行，例如：

```
userdb cleaner: erased '星露谷' from 'rime_sheep_pro' (xlg 	星露谷).
userdb cleaner: removed row 'xlg 	星露谷	c=15936696 d=1 t=1' from 'rime_sheep_pro.userdb.txt'.
```

**`sync/userdb_cleaner.txt`**：每次有效运行追加「时间戳 + 统计 + 全部被删除的词条」（每行 10 条，同一词条仅记录一次）；文件超过 2 MB 时轮转为 `userdb_cleaner.txt.1`。

## 手工置顶短语（Lua pin 等）

若通过 Lua 直接向 userdb 写入置顶短语，例如：

```lua
pin_db:update(key, "c=" .. encoded_commit .. " d=0 t=1")
```

**`d = 0` 会被判定为“不可见”并直接清除**，原因如下：

- 当 `t != 0` 时，衰减公式为 `d * exp((t - present)/200)`，`d = 0` 恒为 0，必然 `<= kDiscardThreshold`。
- 同步合并会将行的 `t` 重写为当前 tick（`UserDbMerger::Put` 中 `o.tick = max_tick_`），因此 `d = 0` 的行在同步一次后即失效，即使写 `t = 0` 也无法保留。

正确做法是给 `d` 一个正数（排名由 `c` 决定，`d` 仅决定是否被淘汰）：

```lua
pin_db:update(key, "c=" .. encoded_commit .. " d=1 t=1")
```

## 注意事项

- **物理删除不可逆，插件不备份本地词典**。唯一的回滚手段是第 2 步生成的 `sync/<本机 installation_id>/*.userdb_backup.txt`（清理前的快照）：将其重命名为 `*.userdb.txt` 后再次触发同步，词条会合并回本地。注意合并为“只增不减”，回滚会带回备份中的所有词条。
- 清理与同步均需释放 `.userdb` 文件锁：两次同步各自会先关闭所有输入会话（遵循官方 `RimeSyncUserData` 做法），本地清理仅在无法打开词典时再关闭一次。关闭会话会重置当前正在输入的拼音串，前端随后会自动重建。
- 同步与清理均在插件进程内完成，为纯本地 LevelDB 与文件操作，不联网、不调用 WeaselDeployer 外部进程，通常耗时低于 1 秒。
- 单次同步失败不会中止清理：失败结果记入日志，并在弹窗显示「清理前同步 / 清理后同步：失败」。
- 无法打开的词典会被跳过并记入日志，不会中断整体清理。
- 运行锁为同步目录根下的 `.userdb_cleaner.lock`：获取失败则跳过本轮；超过 10 分钟视为陈锁自动接管。云盘同步存在延迟且 mtime 不一定保留，跨设备互斥仅为尽力而为。
- 清理任务在独立线程中运行，若上一次尚未结束，本次请求将被忽略。

## 已知限制

- **跨设备删除传播失效**：本地不再保留墓碑行，在其他设备上通过 `/del` 删除的词条，本机无法收到删除标记，会以正数 `c` 合并回来（需在该设备上也执行删除）。
- **未运行本插件的设备会导致重复上报**：若其他设备未运行本插件，其保留的 `c < clean_threshold` 行会在每轮「清理前同步」时被合并进来、再次清除并重复上报；只有所有设备均运行本插件才能收敛。
- **跨设备并发无法真正互斥**：锁文件依赖云盘，两台设备同时清理仍可能互相覆盖快照。
- **平台验证有限**：仅在 Windows 上实测通过；清理后依赖前端重建会话，macOS / Linux 未经验证；插件无单元测试，CI 仅执行编译。