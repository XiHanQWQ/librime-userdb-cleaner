# Rime UserDB Cleaner 插件

这是一个 Rime 输入法的自定义插件，用于清理用户词典中很少使用的词条（基于 `c=` 字段的阈值判断）。

## 功能特性

- **就地清理用户词典**：直接遍历 LevelDb，把 `c < clean_threshold` 的词条物理 `Erase` 掉（含 `c` 为负值的删除标记行），不会删除或清空 `.userdb` 目录，不影响输入法正常使用
- **清理同步快照**：删除同步目录下 `.userdb.txt` 文件中 `c < clean_threshold` 的词条行，避免下次同步时把刚清理掉的词条合并回来
- **清理前后各同步一次**：旧快照先改名备份，「重建快照同步」基于清理后的本地词典重新生成快照，清理后再「收尾同步」；进程内调用 librime 的 `installation_update` + `user_dict_sync` 任务，不调用 WeaselDeployer 外部进程
- 支持按词典名称过滤，**通过黑名单跳过指定的 userdb**，其余词典照常清理
- 支持简略和详细两种清理结果通知模式
- 同步前把旧快照 `.userdb.txt` 改名保留为 `.userdb_backup.txt`（同名覆盖旧备份）；清理新快照时先写临时文件再原子替换
- 把删除的词条**全部**记入同步目录的 `userdb_cleaner.txt` 日志文件（每行 10 条）
- 清理在独立线程中运行，不会阻塞输入法界面；同一时刻只允许一次清理任务

## 安装配置

### 1. 编译插件

将插件源代码编译为 Rime 插件模块，确保链接到 Rime 核心库。

### 2. 配置 Rime 配置文件（如 `default.custom.yaml` 或具体方案的 `.schema.yaml`）

在目标的 Rime 方案（schema）中添加以下配置：

```yaml
engine/processors:
  - userdb_cleaner    # 添加在 speller 之后

userdb_cleaner:
  trigger_input: "/clean"               # 触发清理的输入字符串，默认 "/del"
  full_information_display: true        # 是否显示完整清理信息，默认 false
  clean_threshold: 1                    # 删除条件：c < clean_threshold，默认 0
  exclude_userdb_list:                  # 黑名单：不参与清理的词典，未设置或为空时清理所有
    - 词典名称1
    - 词典名称2
```

或者使用列表语法：

```yaml
userdb_cleaner:
  exclude_userdb_list: ["词典名称1", "词典名称2"]
```

### 3. 配置项详细说明

| 配置项 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `trigger_input` | 字符串 | `"/del"` | 在输入法中输入该字符串后触发清理；设为空字符串则永不触发 |
| `full_information_display` | 布尔 | `false` | `false` 时仅显示删除统计；`true` 时额外显示扫描的快照数、涉及的词典与文件、以及被删除的词条（最多列 10 条，旁边标出删除总条数） |
| `clean_threshold` | 整数 | `0` | 删除条件：`c < clean_threshold`。找不到 `c=` 字段的行一律保留。实时词典与同步快照都按此条件处理，`c` 为负值（删除标记）的行在阈值大于该负值时也会被删除 |
| `exclude_userdb_list` | 字符串数组 | 空（清理所有） | **黑名单**：指定不参与清理的词典名称，可带或不带 `.userdb` / `.userdb.txt` 后缀。例如 `["rime_sheep_pro", "custom"]` 表示跳过这两个，清理其余全部 |

## 使用说明

1. 在 Rime 输入法处于可输入状态时，键入配置的触发字符串（默认 `/del`），输入法会清空当前输入并启动清理任务。
2. 清理任务在后台线程中按顺序执行：
   1. **清理本地词典**：先关闭所有输入会话（活动会话持有 `.userdb` 文件锁），再扫描用户数据目录下的 `*.userdb`，按 `exclude_userdb_list` 黑名单过滤；对每本词典用 `QueryAll` 原始遍历，把 `c < clean_threshold` 的词条（含负值标记行）物理 `Erase`
   2. **旧快照改名备份**：把同步目录下的 `*.userdb.txt` 改名为同目录的 `*.userdb_backup.txt`（覆盖旧备份，黑名单词典的快照跳过、保持不变）
   3. **重建快照同步**：进程内调用 `installation_update` + `user_dict_sync`；旧快照已不在，不会合并旧数据，直接基于清理后的本地词典导出生成全新的 `.userdb.txt`
   4. **清理新快照**：对刚生成的 `*.userdb.txt` 删除 `c < clean_threshold` 的词条行；先写临时文件再原子替换
   5. **收尾同步**：合并清理后的快照并再次导出，确认没有词条被合并回来
3. 把删除记录、改名数量与两次同步结果追加到同步目录的 `userdb_cleaner.txt`。
4. 清理完成后弹出消息框通知结果（Windows 使用原生 MessageBox，macOS 使用 osascript，Linux 依次尝试 zenity / kdialog / notify-send，均不可用时写入日志）。

## 删除方式说明

词条会从 LevelDb 中**物理移除**（`Db::Erase`），而不是 Rime 默认的“标记为删除”（把 `c` 改为负值）：

- 输入法不会再检索到这些词条，同步快照里也不会再有对应的行
- 本地不留删除标记行 → 同步全量导出的快照里没有它们 → 下次运行不会再重复上报同一批词
- 代价：删除标记不再跨设备传播。其他设备同步过来的同名词条会重新出现（再用 `/del` 删一次即可）

## 注意事项

- 清理任务在独立线程中运行，不会阻塞输入法界面；触发时若上一次清理尚未结束，本次请求会被忽略。
- `clean_threshold` 是**严格小于**的比较条件：`c < clean_threshold` 才会删除。实时 `.userdb` 与同步快照 `.userdb.txt` 各自独立判断，判断条件相同：
  - 默认值 `0`：只删除 `c` 为负值的删除标记行，正向词条（`c >= 0`）全部保留。
  - 设为正整数，例如 `1`：删除所有 `c < 1` 的条目，**包括 `c` 为负值的删除标记行**，即从未使用过的 `c = 0` 词条也会被清掉。
  - 本地遍历走 `QueryAll` 原始游标，librime 在 `CreateDictEntry` 里的过滤（负值、过期衰减）对判断不生效。想保留删除标记行只能把阈值设成负值，但那样正向词条也全部不清理，等同于关闭功能。
- `exclude_userdb_list` 为**黑名单**：列出的词典会被跳过，未列出的词典都会被清理。列表为空时清理所有词典。它同时作用于实时 `.userdb` 的清理、快照的改名备份与新快照的清理；黑名单词典的快照保持原样，重建快照同步照常合并它们。
- 同步前旧快照 `.userdb.txt` 会改名为同目录的 `*.userdb_backup.txt`（同名覆盖上一次的备份），旧快照内容保留在该文件中；新快照清理时不再另行复制备份，因为它的内容随时可由同步重新导出。
- 日志文件 `userdb_cleaner.txt` 位于同步目录（`sync_dir`）中，可通过 Rime 安装配置中的 `sync_dir` 找到；日志记录**全部**被删除的词条（每行 10 条），弹窗只在 `full_information_display: true` 时列出前 10 条并标出删除总条数。
- 同步在插件进程内完成（`installation_update` + `user_dict_sync` 任务），是纯本地的 LevelDb 与文件操作，不联网、不调用 WeaselDeployer 外部进程，通常耗时不到 1 秒。
- 清理本地词典与每次同步前都会先关闭所有输入会话（与官方 `RimeSyncUserData` 做法一致）：活动会话持有 `.userdb` 文件锁，不关闭则插件与同步都无法打开词典（Windows 下报 `LOCK: 另一个程序正在使用此文件`）。代价是此刻正在输入的拼音串会被重置，前端会自动重建会话。
- 某次同步失败不会中止清理：任务继续执行，失败结果记入 `userdb_cleaner.txt` 日志并在弹窗中显示"重建快照同步/收尾同步：失败"。
- 无法打开的词典会被跳过并记入日志，不会中断整体清理。