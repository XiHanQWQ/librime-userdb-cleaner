# Rime UserDB Cleaner 插件

这是一个 Rime 输入法的自定义插件，用于清理用户词典中的无效词条。支持三种清理模式：按 `c=` 值、按 `tick` 差值、或先按 `c=` 再按 `tick` 差值。

## 功能特性

- 清理用户数据目录下的 `.userdb` 文件夹（清空其中所有文件）
- 清理同步目录下所有 `.userdb.txt` 文件中的无效词条
- 三种清理模式：
  - `c` — 按 `c=` 值清理（c < clean_threshold 则删除）
  - `tick` — 按 `tick` 差值清理（全局 tick - 词条 tick > tick_threshold 则删除）
  - `c_then_tick` — 先按 `c=` 值，再按 `tick` 差值，两步过滤
- 支持按词典名称过滤，只清理指定的 userdb
- 支持简略和详细两种清理结果通知模式
- 可配置是否备份每个 `.userdb.txt` 为 `.userdb_backup.txt`
- 可配置是否将清理结果写回文件（不写回时仅扫描统计）
- 记录所有被删除的词条到同步目录的 `userdb_cleaner.txt` 日志文件
- 清理前后自动执行用户词典同步（Windows 下调用 WeaselDeployer，其他平台调用 Rime 同步任务）
- 在 `tick`/`c_then_tick` 模式下，同步前自动备份并清理 sync_dir 中当前用户的旧 `.userdb.txt` 文件，阻止同步的 Restore(Merger) 步骤将所有词条的 tick 统一刷成总 tick

## 安装配置

### 1. 编译插件

将插件源代码编译为 Rime 插件模块，确保链接到 Rime 核心库。

### 2. 配置 Rime 配置文件（如 `default.custom.yaml` 或具体方案的 `.schema.yaml`）

在目标的 Rime 方案（schema）中添加以下配置：

```yaml
engine/processors:
  - userdb_cleaner    # 添加在合适的位置

userdb_cleaner:
  trigger_input: "/clean"               # 触发清理的输入字符串，默认 "/del"
  full_information_display: true        # 是否显示完整清理信息，默认 false
  clean_mode: "c"                       # 清理模式: "c"(按c值), "tick"(按tick差值), "c_then_tick"(先c后tick)，默认 "c"
  clean_threshold: 0                    # c值清理阈值：c < clean_threshold 时删除，默认 0
  tick_threshold: 3000                  # tick差值清理阈值：全局tick-词条tick > tick_threshold 时删除，默认 3000
  enable_backup: true                   # 是否在清理前备份 .userdb.txt 文件，默认 false
  enable_write_clean_file: true         # 是否将清理后的结果写回文件，默认 false
  cleanup_userdb_list:                  # 需要清理的词典列表，未设置或为空时清理所有
    - 词典名称1
    - 词典名称2
```

或者使用列表语法：

```yaml
userdb_cleaner:
  cleanup_userdb_list: ["词典名称1", "词典名称2"]
```

### 3. 配置项详细说明

| 配置项 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `trigger_input` | 字符串 | `"/del"` | 在输入法中输入该字符串后，将触发清理任务 |
| `full_information_display` | 布尔 | `false` | `false` 时仅显示删除词条总数；`true` 时额外显示被清理的文件夹、文件名及每个删除的词条 |
| `clean_mode` | 字符串 | `"c"` | 清理模式：`"c"` — 仅按 c 值；`"tick"` — 仅按 tick 差值；`"c_then_tick"` — 先按 c 值过滤，再按 tick 差值过滤剩余词条 |
| `clean_threshold` | 整数 | `0` | c 值清理阈值：对于包含 `c=数值` 的词条行，若 `c < clean_threshold` 则删除。（`"c"` 和 `"c_then_tick"` 模式使用） |
| `tick_threshold` | 整数 | `3000` | tick 差值清理阈值：词条的 `tick` 值与文件头部 `#@/tick`（总 tick）的差值超过此值时删除。（`"tick"` 和 `"c_then_tick"` 模式使用） |
| `enable_backup` | 布尔 | `false` | 是否在清理前将每个 `.userdb.txt` 文件备份为 `.userdb_backup.txt` |
| `enable_write_clean_file` | 布尔 | `false` | 是否将清理后的结果写回文件。设为 `false` 时仅扫描统计，不修改文件（模拟运行） |
| `cleanup_userdb_list` | 字符串数组 | 空（清理所有） | 指定需要清理的词典名称（不包含 `.userdb` 或 `.userdb.txt` 后缀）。例如 `["luna_pinyin", "custom"]` |

## 使用说明

1. 在 Rime 输入法处于可输入状态时，键入配置的触发字符串（默认 `/del`），输入法会自动清空当前输入并开始在后台运行清理任务。
2. 清理过程会依次执行：
   - 若为 `tick` 或 `c_then_tick` 模式：备份并清理 sync_dir 中当前用户的旧 `.userdb.txt` 文件（阻止同步时 tick 被重置）
   - 前置同步（将当前用户词典状态同步到 `sync` 目录）
   - 清空用户数据目录中指定的 `.userdb` 文件夹下的所有文件
   - 扫描同步目录下所有 `.userdb.txt` 文件，根据模式删除无效词条行
   - 将删除的词条记录到同步目录的 `userdb_cleaner.txt` 文件（按时间追加）
   - 后置同步（将清理后的词典重新同步回用户目录）
3. 清理完成后会弹出消息框（Windows 使用原生 MessageBox，Linux/macOS 尝试 zenity/kdialog/notify-send）通知结果。

### 清理模式详解

**模式 `c`** — 按提交次数清理（默认）：
每条词条包含 `c=数值`（提交次数），若 `c < clean_threshold` 则删除。适合清理使用频率低的词条。

**模式 `tick`** — 按时间戳差值清理：
每条词条包含 `t=数值`（记录时的 tick），文件头部有 `#@/tick`（总 tick）。若 `总tick - 词条tick > tick_threshold` 则删除。适合清理长期未使用的过期词条。此模式下同步前会自动清理 sync_dir 中的旧文件，防止同步操作将所有词条的 tick 重置为总 tick。

**模式 `c_then_tick`** — 两步过滤：
第一步：若 `c < clean_threshold` 则删除。
第二步：对 c 值通过的词条，若 `总tick - 词条tick > tick_threshold` 则也删除。
两步中任意一步命中即删除。同时具备低频率和长期未使用两种删除策略的优点。

## 注意事项

- 清理任务在独立线程中运行，不会阻塞输入法界面。
- 当 `enable_backup` 为 `true` 时，每个 `.userdb.txt` 文件修改前都会在同一目录下生成 `.userdb_backup.txt` 备份文件。
- 当 `enable_write_clean_file` 为 `false` 时，不会修改任何文件，可用于模拟运行查看将删除的词条。
- 如果未指定 `cleanup_userdb_list`，则会处理所有匹配的词典（即所有 `.userdb` 文件夹和 `.userdb.txt` 文件）。
- 由于 `c=` 值是非负整数，默认阈值 `0` 意味着只有 `c < 0` 的词条会被删除，实际上不会删除任何词条。建议根据实际需要设置正整数值。
- 日志文件 `userdb_cleaner.txt` 位于同步目录（`sync_dir`）中，可通过 Rime 安装配置中的 `sync_dir` 找到。
- 使用 `tick` 或 `c_then_tick` 模式时，插件会在同步前自动备份并删除 sync_dir 中当前用户目录下的旧 `.userdb.txt` 文件。这是为了防止 Rime 的 Sync 操作在 Restore(Merger) 步骤中，将所有词条的 tick 值统一刷新为当前的总 tick 值，从而导致 tick 差值计算失效。
- tick 清理不会影响其他设备的同步数据：仅删除当前运行设备对应用户的同步目录下的文件。