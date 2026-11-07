# 命令行与 JSON · AkashaVeil 0.1.1

[文档导航](README.md) · [使用指南](USER_GUIDE.md) · [English overview](../README_EN.md#command-line)

同一个 EXE 同时提供 GUI 和 CLI。以下示例使用 x64 文件名；x86 / arm64 的参数一致。命令行操作不启动主界面，不配置高级水印或管理其他 GUI 实例的图层。

## 1. 命令

```text
AkashaVeil-x64.exe list [--json]
AkashaVeil-x64.exe protect --process <PID> [--json]
AkashaVeil-x64.exe protect --window <HWND> [--json]
AkashaVeil-x64.exe restore --process <PID> [--json]
AkashaVeil-x64.exe restore --window <HWND> [--json]
AkashaVeil-x64.exe --version [--json]
AkashaVeil-x64.exe --help
AkashaVeil-x64.exe --profile <文件>
```

| 名称 | 说明 |
|---|---|
| `list` / `windows` | 枚举当前顶层窗口，每条含所属进程信息 |
| `protect` / `hide` / `exclude` | 请求对指定目标应用捕获排除；旧系统按能力分级处理 |
| `restore` / `unhide` | 移除目标的系统捕获亲和性 |
| `--version` / `-v` / `version` | 版本与 EXE 架构 |
| `--help` / `-h` / `help` / `-?` / `/?` | 帮助；单独使用 |

`--process` / `--pid` 是正十进制 PID；`--window` / `--hwnd` 是非零**十六进制** HWND，可省略 `0x`。同一次保护 / 恢复只能指定一种目标，重复或冲突选项会报语法错误。

PID 命令处理当前可枚举到的该进程窗口，不会等待进程未来创建窗口；HWND 命令要求目标仍存在且是顶层窗口。提权目标通常需要同等权限。

## 2. 枚举结果

```powershell
.\AkashaVeil-x64.exe list --json
```

返回 JSON 数组；以下是说明字段的演示数据，不对应运行中的真实窗口：

```json
[
  {
    "handle": "0x0000000000123456",
    "pid": 1234,
    "tid": 5678,
    "title": "Demo window",
    "class_name": "DemoWindowClass",
    "path": "C:\\Windows\\notepad.exe",
    "architecture": "x64",
    "mode": "normal"
  }
]
```

`mode` 使用稳定英文标记：`normal`、`protected`、`transparent` 或 `unknown`。同一个 PID 可以出现多次。不可读取的路径 / 架构等字段应按实际返回处理，不要根据标题猜测目标身份。

## 3. 保护 / 恢复结果

```powershell
.\AkashaVeil-x64.exe protect --process 1234 --json
.\AkashaVeil-x64.exe restore --window 0x00123456 --json
```

找到目标并尝试执行后，JSON 汇总结构如下：

```json
{
  "action": "protect",
  "succeeded": 1,
  "failed": 0,
  "timeouts": 0,
  "targets": [
    {"handle": "0x0000000000123456", "pid": 1234, "ok": true, "error": ""}
  ]
}
```

- `action` 为 `protect` 或 `restore`，别名也归一为这两个值。
- 失败目标的 `ok` 为 `false`，`error` 为可读错误信息；不要把本地化错误文本当稳定错误码。
- **语法错误 / 没有目标时不保证输出 JSON**，错误会写到 stderr。始终先检查退出码，再解析可用的 stdout。
- `restore` 只修改捕获亲和性；若另一个 GUI 实例仍有水印图层，应在那个实例中执行恢复。

## 4. 退出码

| 退出码 | 含义 |
|---:|---|
| 0 | 成功；`list` 无窗口时也成功并返回空数组 |
| 2 | 语法或参数错误 |
| 3 | 没有可操作目标 |
| 4 | 部分目标成功，部分失败 |
| 5 | 所有目标失败 |
| 6 | 所有目标均超时 / 取消 |

## 5. 版本与配置

```powershell
.\AkashaVeil-x64.exe --version --json
```

```json
{"name":"AkashaVeil","version":"0.1.1","architecture":"x64"}
```

仅传 `--profile <文件>` 时启动 GUI 并使用该配置；与保护 / 恢复组合时，配置主要用于选择输出语言，不会自动加载 GUI 水印方案、执行自动保护或修改配置文件。显式配置读取失败会给出警告，并继续按默认语言处理目标。

```powershell
.\AkashaVeil-x64.exe --profile ".\profiles\demo.ini" protect --pid 1234 --json
```

## 6. 脚本示例

只读枚举，查找指定程序的窗口：

```powershell
$exe = '.\AkashaVeil-x64.exe'
$raw = & $exe list --json
if ($LASTEXITCODE -ne 0) { throw 'Window enumeration failed.' }
$windows = $raw | ConvertFrom-Json
$windows | Where-Object { $_.path -like '*\notepad.exe' } |
    Format-Table handle, pid, title, mode
```

得到当前目标后再保护。示例中的 `$targetHandle` 必须来自刚刚枚举出的目标，而不是长期保存的旧 HWND：

```powershell
$targetHandle = '0x00123456'
$raw = & $exe protect --window $targetHandle --json
$code = $LASTEXITCODE
if ($code -eq 0 -or $code -eq 4 -or $code -eq 5 -or $code -eq 6) {
    $result = $raw | ConvertFrom-Json
    $result.targets | Format-Table handle, pid, ok, error
}
if ($code -ne 0) { throw "Protect failed or only partially succeeded: $code" }
```

重定向 / 管道输出为 UTF-8；交互控制台使用 Unicode。应用不提供 HTTP / MCP 服务，脚本或 AI 直接调用上述本地 EXE 即可。
