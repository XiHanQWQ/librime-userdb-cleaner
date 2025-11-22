Rime UserDB Cleaner 插件

这是一个 Rime 输入法的自定义插件，用于清理用户词典中的无效词条。

功能特性

- 清理用户目录下的 `.userdb` 文件夹内容
- 清理同步目录下的 `.userdb.txt` 文件中的无效词条（c ≤ 0 的词条）
- 支持配置需要清理的特定 userdb
- 支持简略和详细两种信息显示模式
- Windows 平台支持自动调用 WeaselDeployer 进行同步

安装配置

1. 编译插件

将插件源代码编译为 Rime 插件模块。

2. 配置说明

在 Rime 配置文件中添加以下配置：

```yaml
engine/processors/@before 1: userdb_cleaner

userdb_cleaner:
  trigger_input: "/clean"  # 触发清理的输入字符串，默认为 "/del"
  full_information_display: true  # 是否显示完整清理信息，默认为 false
  cleanup_userdb_list:  # 需要清理的 userdb 列表，不设置或为空则清理所有
    - 词库名称1
    - 词库名称2
或者
  cleanup_userdb_list: ["词库名称1", "词库名称2"]
```

3. 配置项说明

· trigger_input: 触发清理操作的输入字符串，当输入该字符串时会启动清理任务
· full_information_display:
  · false (默认): 显示简略信息，只显示清理完成和删除的词条数量
  · true: 显示完整信息，包括清理的文件、文件夹和具体删除的词条
· cleanup_userdb_list:
  · 不设置或为空数组: 清理所有 userdb
  · 设置特定数组: 只清理列表中指定的 userdb
