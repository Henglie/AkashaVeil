# 贡献说明 · Contributing

[简体中文首页](README.md) · [English overview](README_EN.md) · [Issues](https://github.com/Henglie/AkashaVeil/issues)

欢迎提交可复现的问题、文档修正和小范围改进。

## 报告问题

提供软件版本、Windows 版本、EXE 架构、权限、目标程序、捕获方式、复现步骤以及期望 / 实际结果。优先使用不含私人数据的演示窗口或图片；不要上传真实配置、密钥、私人路径或受保护窗口的敏感画面。

捕获问题请注明具体后端。仅提供“保护成功”的状态截图，无法证明实际截图 / 录像符合预期。

## 本地构建

安装 Visual Studio C++ 桌面工具链、Windows SDK、CMake 与 Ninja，然后运行：

```powershell
.\tools\build.ps1 -Architecture x64 -Configuration Release
```

三架构构建使用 `-Architecture all`。输出在 `build/standalone/`。投喂原图是可选的私有构建输入，不提交到仓库；未提供时其他功能照常可用。

## 修改约定

- 保持 C++17、原生 Win32 / GDI+、单 EXE 与 Core / Windows / UI 分层。
- 版本由 `CMakeLists.txt` 的 `project(VERSION)` 定义，不在修复中自行升版。
- 用户文案走现有语言键；维护九语界面、英语回退、RTL 与 DPI 布局。
- 保持纯本地处理，不引入后台联网、遥测或额外设备指纹。
- 新行为说明正常、边界和失败条件；窗口 / 捕获改动检查实际画面、恢复行为及原有配置兼容性。
- 同步中英文 README 与受影响使用说明；不要把未验证能力写成已经支持。

## 提交内容

源码与必要构建配置、公开文档和品牌图标可以提交。不要提交 EXE / DLL / PDB、构建目录、IDE 缓存、用户配置、窗口导出、测试或研究资料、AI 提示词、内部记录、私有资产原图、签名材料和访问令牌。成品通过 Releases 分发。

请保持变更集中，附构建结果及可复现的验证方法。提交贡献即表示你有权提供相应内容，并同意按本项目 [Apache-2.0](LICENSE) 协议分发。

## English summary

Open an Issue with reproduction steps, expected / actual behavior, Windows version, architecture, privilege level, and capture backend. Keep patches small and preserve the local-only, native Win32, C++17, multilingual, DPI-aware design. Build with `tools/build.ps1`; do not commit binaries, user data, private assets, test / research material, prompts, or internal records. Contributions are distributed under Apache-2.0.
