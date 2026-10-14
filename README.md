# Rime UserDB Cleaner 插件

一个 Rime processor 插件：输入触发码后，将用户词典中已失效的词条**物理删除**，并同步清除同步目录下所有 `.userdb.txt` 快照中的对应行，避免下一轮同步将它们重新合并回本地词典。

## 功能特性

- **就地清理用户词典**：直接遍历本地 LevelDb（`<name>.userdb` 目录）或 `plain_userdb`（`<name>.userdb.txt` 文件），对命中判据的词条执行 `Db::Erase` 真正删除，不删除、不清空词典文件本身
- **清理所有同步快照**：递归清理同步目录下**每个设备目录**中的 `*.userdb.txt`（不仅限于本机那份），先写入临时文件再原子替换
- **先同步、再两侧同时清理、最后收尾**：在进程内调用 librime 的 `installation_update` 与 `user_dict_sync`，不调用 WeaselDeployer 外部进程
- **判据来自 librime 本身**：直接调用 `UserDictionary::CreateDictEntry` 判断某行是否仍可能被检索到；插件不复刻其阈值常量与衰减公式，librime 升级后判据自动保持一致
- **逐条留痕**：rime 日志逐条记录被删除的词条；同步目录下的 `userdb_cleaner.txt` 记录全部词条（弹窗仅列出前 10 条及总条数）
- **运行防护**：`sync` 目录根下的 `.userdb_cleaner.lock` 保证同一时刻只有一个清理任务运行；崩溃残留的 `*.userdb.txt.tmp` 在下次运行时自动清除
- 支持按词典名过滤（**排除名单**）、支持简略 / 详细两种弹窗；清理以部署任务的形式在 librime 维护工作线程上执行（与官方 `RimeSyncUserData` 使用相同机制），弹窗在独立线程显示、不阻塞输入

## 判定标准：什么算"失效"

共两条判据，**本地词典与所有快照文件使用同一套**（否则本地删除后快照仍保留，下一轮同步又会将词条合并回来）：

1. **`c < clean_threshold`**（自定义判据，由 `use_custom_threshold` 控制，默认**不启用**）：`c` 为提交次数。比较为**严格小于**，默认 `0` 仅删除 `c` 为负的删除标记行；设为 `1` 则连从未使用过的 `c = 0` 行一并删除。`use_custom_threshold` 为 `false`（默认）时本判据整体关闭，仅使用第 2 条 librime 内部判据。
2. **librime 判定为不可见的行**：调用 `UserDictionary::CreateDictEntry(key, value, present_tick)`，返回空即说明输入法不会再给出该行，无论 `c` 多大都会删除（其内部包含 `commits < 0`、衰减后 `dee <= kDiscardThreshold` 等全部条件）。判定使用的 `present_tick` 取本机该词典的 `/tick`；本地词典无法打开时回退使用快照文件头中的 `#@/tick`。

两类行**不受判据影响**：

- **`t = 0` 的行**：从 table/stabledb 导入的短语（如 `custom_phrase`），librime 对其从不执行衰减淘汰 → 一律保留
- **无法解析出 `c/d/t` 的行** → 一律保留，但会计数并在弹窗与日志中报告（避免词典格式变化后清理行为静默失效）

## 清理流程

1. **清理前同步**：将 `sync/` 下**所有**设备目录的快照合并进本地词典，再将本地词典全量导出为本机的 `*.userdb.txt`
2. **备份本机快照**：将 `sync/<本机 installation_id>/*.userdb.txt` 复制为同目录下的 `*.userdb_backup.txt`（覆盖旧备份；仅复制不移动，其他设备目录不做改动）。该步骤放在同步之后执行，因为 `user_data_sync_dir() = sync_dir / user_id` 需待 `installation_update` 执行后才可信
3. **清理本地词典**：扫描用户数据目录、按排除名单过滤，使用 `QueryAll` 原始游标逐行判定，命中的直接执行 `Erase`（包括第 1 步刚合并进来的其他设备的行）
4. **清理同步快照**：对 `sync/` 下所有 `*.userdb.txt` 删除同样判定的行，并一并清除残留的 `*.userdb.txt.tmp`
5. **清理后同步**：合并已清理的快照并再次导出，保证发布出去的快照与本地状态一致

整个流程由 `sync` 目录根下的 `.userdb_cleaner.lock` 保护，同一时刻仅运行一个清理任务。

## 安装配置

### 1. 编译插件

将插件编译为 Rime 插件模块，并确保链接 Rime 核心库（`UserDictionary::CreateDictEntry` 是 `RIME_DLL` 导出符号）。

### 2. 配置方案文件（如具体方案的 `.schema.yaml`）

```yaml
engine/processors:
  - userdb_cleaner                      # 建议放在 speller 之后

userdb_cleaner:
  trigger_input: "/clean"               # 触发清理的输入串，默认 "/del"
  clean_threshold: 1                    # 删除条件：c < clean_threshold，默认 0
  use_custom_threshold: true            # 是否启用自定义 c 值判据，默认 false
  full_information_display: true        # 弹窗是否显示详细信息，默认 false
  exclude_userdb_list: []               # 排除名单：不参与清理的词典，空 = 全部清理
```

排除名单也可使用行内列表语法：

```yaml
userdb_cleaner:
  exclude_userdb_list: ["rime_sheep_pro", "custom"]
```

### 3. 配置项说明

| 配置项 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `trigger_input` | 字符串 | `"/del"` | 输入该串后触发清理；设为空字符串则永不触发 |
| `clean_threshold` | 整数 | `0` | 删除条件 `c < clean_threshold`，仅当 `use_custom_threshold` 为 `true` 时生效 |
| `use_custom_threshold` | 布尔 | `false` | 是否启用自定义 c 值废词判据（`c < clean_threshold`）。`false`（默认）时仅使用 librime 内部判据，librime 判定为不可见的行仍会被删除；`t = 0` 的短语行与无法解析出 `c/d/t` 的行在任何开关状态下都一律保留 |
| `full_information_display` | 布尔 | `false` | `false` 仅显示统计；`true` 额外显示扫描 / 备份数量、处理的词典与文件、删除的词条（最多 10 条，并标出总条数） |
| `exclude_userdb_list` | 字符串数组 | 空（全部清理） | **排除名单**：不参与清理的词典名，可带或不带 `.userdb` / `.userdb.txt` 后缀。该名单同时作用于本地清理、快照备份与快照清理；名单内词典的快照既不备份也不清理，但同步合并照常进行 |

## 输出

**弹窗**（Windows 原生 MessageBox，macOS 使用 osascript，Linux 依次尝试 zenity / kdialog / notify-send，均不可用时写入日志）

- 默认：清理完成 → 用户词典移除数、同步快照删除行数、耗时、删除记录所在文件，以及两次同步的结果
- 存在跳过行时追加一行：`跳过 N 行 t=0 短语、N 行无法解析的行`
- `full_information_display: true` 时额外显示：已扫描快照数、备份的本机快照数、处理的词典 / 文件列表、删除的词条（最多 10 条，标题旁标出 `( N条 )` 总数）
- 无法获取运行锁时仅提示「另一个清理任务正在运行」

**rime 日志**：每个被删除的词条一行，例如

```
userdb cleaner: erased '星露谷' from 'rime_sheep_pro' (xlg 	星露谷).
userdb cleaner: removed row 'xlg 	星露谷	c=15936696 d=1 t=1' from 'rime_sheep_pro.userdb.txt'.
```

**`sync/userdb_cleaner.txt`**：每次有效运行追加「时间戳 + 统计 + 全部被删除的词条」（每行 10 条，同一词条仅记录一次）；日志只追加、不轮转、无体积上限。

## 手工置顶短语的注意事项（Lua pin 等）

如果通过 Lua 直接向 userdb 写入「置顶短语」，例如：

```lua
pin_db:update(key, "c=" .. encoded_commit .. " d=0 t=1")
```

**`d = 0` 的行会被判定为"不可见"而直接删除**，原因有二：

- `t != 0` 时衰减公式为 `d * exp((t - present)/200)`，`d = 0` 恒为 0，必然 `<= kDiscardThreshold`
- 同步合并会将行的 `t` 重写为当前 tick（`UserDbMerger::Put` 中 `o.tick = max_tick_`），因此 `d = 0` 的行**同步一次即失效**，写入 `t = 0` 也无法保全

正确写法是给 `d` 一个正数（排序由 `c` 决定，`d` 仅决定该行是否存活）：

```lua
pin_db:update(key, "c=" .. encoded_commit .. " d=1 t=1")
```

## 注意事项

- **物理删除不可逆，插件不备份本地词典**。唯一的回滚手段是第 2 步生成的 `sync/<本机 installation_id>/*.userdb_backup.txt`（清理前的快照）：将其重命名回 `*.userdb.txt` 后再触发一次同步，其中的词条会合并回本地。注意同步合并为「只增不减」，回滚会将备份中的所有词条全部带回。
- 清理与同步都需要释放 `.userdb` 文件锁：两次同步各自会先关闭所有输入会话（官方 `RimeSyncUserData` 的做法），本地清理仅在词典无法打开时才再关闭一次。关闭会话会重置此刻正在输入的拼音串；Weasel 会自动重建会话，fcitx5-rime 需要执行其自身的「重新部署 / 同步」动作来重建（见下一条）。
- 同步与清理均在插件进程内完成，是纯本地的 LevelDb 与文件操作，不联网、不调用 WeaselDeployer 外部进程，通常耗时不足 1 秒。
- 某次同步失败不会中止清理：失败结果记入日志，并在弹窗中显示「清理前同步 / 清理后同步：失败」。
- 无法打开的词典会被跳过并记入日志，不会中断整体清理流程。
- 运行锁位于同步目录根下的 `.userdb_cleaner.lock`：无法获取锁即跳过本轮，超过 10 分钟视为陈锁自动接管。云盘同步存在延迟、mtime 也未必保留，跨设备互斥仅为尽力而为。
- **清理以维护任务运行，绝不能在按键事件内同步执行**：触发清理的会话被 `RimeProcessKey` 调用栈上的 `shared_ptr` 强持有，只要仍在按键事件内，其持有的 `.userdb` LOCK 就无法释放（日志表现为 `LOCK: 另一个程序正在使用此文件`，同步全部失败）。触发时仅执行三件事：清空会话、将清理任务排入 `Deployer`、调用 `StartMaintenance()` 进入维护模式；清理本体在维护工作线程上运行，该线程上没有栈引用，会话锁得以真正释放，任务开头另有「清理会话 + 重试」兜底。也不可放到任意自建线程上运行——librime 的会话表未加锁（`Service::CleanupAllSessions` 直接清空 map），非前端线程销毁会话会与前端构成数据竞争，fcitx5 下会直接崩溃。维护模式期间 `Service::disabled()` 使所有会话 API 安全失效（按键直接透传），结束时 Deployer 发出 `deploy success`，Weasel 收到后自动重建会话；弹窗（模态）在独立线程显示，不阻塞维护收尾。
- **fcitx5-rime 不会自动重建失效的会话**：清理销毁会话后，fcitx5-rime 对每个已存在的输入上下文仍缓存旧的 session id（`RimeSessionHolder`），仅在其自身的「重新部署 / 同步」动作中统一释放重建（`RimeEngine::releaseAllSession`），失效后不会自动新建。因此在 fcitx5 上清理完成后，需对当前窗口执行一次 fcitx5-rime 的「重新部署」或「同步」（或重开窗口 / 重启 fcitx5）即可恢复输入；主线程执行方案已消除崩溃本身。

## 已知局限

- **跨设备的删除传播失效**：本地不再保留墓碑行，在其他设备上执行 `/del` 删除的词条，本机收不到删除标记，会带正数 `c` 合并回来（需要在那台设备上也执行一次删除）。
- **其他设备未运行本插件时会重复上报**：该设备会一直保留 `c < clean_threshold` 的行，本机每轮「清理前同步」都会将其合并进来、再清理一次、再上报一次；只有每台设备都运行本插件才能收敛。
- **跨设备并发无法真正互斥**：锁文件依赖云盘，两台设备同时清理仍可能互相覆盖快照。
- **仅在 Windows 上实测过**：清理后依赖前端重建会话，macOS / Linux 未验证；插件无单元测试，CI 仅执行编译。
