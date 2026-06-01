# Rime UserDB Cleaner 插件

这是一个 Rime 输入法的自定义插件，用于清理用户词典中的无效词条（基于 `c=` 字段的阈值判断）。

## 功能特性

- 清理用户数据目录下的 `.userdb` 文件夹（清空其中所有文件）
- 清理同步目录下所有 `.userdb.txt` 文件中的低频词条（`c` 值低于用户设定的阈值时删除）
- 支持按词典名称过滤，只清理指定的 userdb
- 支持简略和详细两种清理结果通知模式
- 自动备份每个 `.userdb.txt` 为 `.userdb_backup.txt`
- 记录所有被删除的词条到同步目录的 `userdb_cleaner.txt` 日志文件
- 清理前后自动执行用户词典同步（Windows 下调用 WeaselDeployer，其他平台调用 Rime 同步任务）

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
  clean_threshold: 0                    # 删除条件：c < clean_threshold，默认 0
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
| `clean_threshold` | 整数 | `0` | 删除条件：对于包含 `c=数值` 的词条行，若 `c < clean_threshold` 则删除；未找到 `c=` 字段的行总是保留 |
| `cleanup_userdb_list` | 字符串数组 | 空（清理所有） | 指定需要清理的词典名称（不包含 `.userdb` 或 `.userdb.txt` 后缀）。例如 `["luna_pinyin", "custom"]` |

## 使用说明

1. 在 Rime 输入法处于可输入状态时，键入配置的触发字符串（默认 `/del`），输入法会自动清空当前输入并开始在后台运行清理任务。
2. 清理过程会依次执行：
   - 前置同步（将当前用户词典状态同步到 `sync` 目录）
   - 清空用户数据目录中指定的 `.userdb` 文件夹下的所有文件
   - 扫描同步目录下所有 `.userdb.txt` 文件，备份并删除其中 `c < clean_threshold` 的词条行
   - 将删除的词条记录到同步目录的 `userdb_cleaner.txt` 文件（按时间追加）
   - 后置同步（将清理后的词典重新同步回用户目录）
3. 清理完成后会弹出消息框（Windows 使用原生 MessageBox，Linux/macOS 尝试 zenity/kdialog/notify-send）通知结果。

## 注意事项

- 清理任务在独立线程中运行，不会阻塞输入法界面。
- 每个 `.userdb.txt` 文件修改前都会在同一目录下生成 `.userdb_backup.txt` 备份文件。
- 如果未指定 `cleanup_userdb_list`，则会处理所有匹配的词典（即所有 `.userdb` 文件夹和 `.userdb.txt` 文件）。
- 由于 `c=` 值是非负整数，默认阈值 `0` 意味着只有 `c < 0` 的词条会被删除，实际上不会删除任何词条。建议根据实际需要设置正整数值。
- 日志文件 `userdb_cleaner.txt` 位于同步目录（`sync_dir`）中，可通过 Rime 安装配置中的 `sync_dir` 找到。