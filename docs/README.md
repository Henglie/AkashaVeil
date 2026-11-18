# 使用文档 · AkashaVeil

[项目首页](../README.md) · [English overview](../README_EN.md) · [下载 v0.1.2](https://github.com/Henglie/AkashaVeil/releases/tag/v0.1.2)

| 文档 | 内容 |
|---|---|
| [使用指南](USER_GUIDE.md) | 首次运行、保护与恢复、高级模式、图片水印、配置与托盘 |
| [隐水印说明](WATERMARK.md) | R-DCT 原理、参数、画质代价与验证边界；附英文摘要 |
| [命令行与 JSON](CLI.md) | 命令、目标格式、JSON 字段、退出码与脚本示例 |
| [常见问题](FAQ.md) | 捕获差异、权限、便携模式、隐水印、数据与退出 |
| [功能介绍](../介绍文章.md) | 按使用场景介绍本版功能 |
| [更新日志](../CHANGELOG.md) | 正式版本历史 |
| [贡献说明](../CONTRIBUTING.md) | 构建、代码风格、问题反馈与提交范围 |
| [发布清单](../发布清单.md) | 发行包内容、校验方式与源码公开范围 |

程序内按 **F1** 可打开九语离线帮助。本文档对应 **0.1.2**。升级时请保留自己的配置文件，并查看新版本说明。

## 先分清两个概念

- **本机画面**：你在显示器上直接看到的窗口。
- **捕获画面**：截图、录屏或会议软件收到的画面。

“保护所选”改变的是捕获结果，不是本机桌面窗口的外观。默认方案“窗口透明”会让窗口从捕获画面中消失；“窗口涂黑”会让它变成黑块。两种方案在主界面右侧的“默认保护方案”编组中切换。不同捕获后端会有不同表现，操作后应使用目标截图 / 录屏软件验证。

## English readers

The [English README](../README_EN.md) covers downloads, setup, commands, shortcuts, privacy, compatibility, and building. The application itself includes English offline help under **F1**. These extended Markdown guides are in Simplified Chinese.
