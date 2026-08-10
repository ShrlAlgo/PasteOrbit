# PasteOrbit

PasteOrbit 是一款基于 WinUI 3 的 Windows 本地剪切板历史工具。

应用记录文本、富文本、图片和文件路径，并在当前输入位置附近提供搜索、预览和快速粘贴面板。

## 功能

- 记录文本、HTML、RTF、图片和文件路径。
- 在当前活动输入框或文本光标附近显示历史面板。
- 按类型筛选并搜索当前剪切板历史。
- 直接点击卡片粘贴原始内容。
- 将富文本粘贴为纯文本。
- 将文本或图片保存为文件。
- 在卡片内展开文本、富文本、图片和图片文件预览。
- 置顶常用记录并保留置顶记录。
- 使用数字键快速粘贴当前列表中的前九条未置顶记录。
- 按当前筛选结果清空未置顶记录。
- 排除指定应用的剪切板记录。
- 从托盘暂停监听十分钟或恢复监听。
- 导出和恢复本地加密备份。
- 配置主题、界面密度、保留天数、记录上限和功能快捷键。

## 系统要求

- Windows 10 版本 1809 或更高版本。
- x64 处理器和操作系统。
- [.NET 8 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/8.0)。
- [Windows App Runtime 2.3](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads)。

当前发布包采用框架依赖模式，并且不包含完整的 .NET 与 Windows App Runtime。

## 运行

1. 下载 `PasteOrbit-win-x64` 发布压缩包。
2. 将压缩包完整解压到可写目录。
3. 运行 `PasteOrbit.exe`。
4. 使用 `Alt + V` 打开剪切板历史面板。

主面板关闭后应用继续在系统托盘运行。

退出应用需要使用托盘菜单中的“退出”。

## 默认快捷键

| 操作 | 快捷键 |
|---|---|
| 打开剪切板历史 | `Alt + V` |
| 粘贴选中记录 | `Enter` |
| 粘贴为纯文本 | `Shift + Enter` |
| 展开或收起预览 | `Space` |
| 置顶或取消置顶 | `Ctrl + P` |
| 删除选中记录 | `Delete` |
| 粘贴为文件 | `Ctrl + Shift + S` |
| 隐藏面板 | `Esc` |

新配置使用以上默认值。

已有配置继续使用设置文件中保存的快捷键。

快捷键可以在“设置 > 快捷键”中修改。

## 数字键快速粘贴

面板打开后，卡片右下角的 `1` 至 `9` 表示数字键快速粘贴位置。

置顶记录不参与数字编号，也不占用数字位置。

数字编号以当前筛选后的可见列表为准。

搜索框处于编辑状态时，数字作为搜索内容输入。

## 托盘菜单

托盘菜单提供打开面板、暂停监听、打开设置和退出应用。

“暂停 10 分钟”在计时结束后自动恢复监听。

暂停期间不会新增剪切板历史记录。

## 数据与隐私

应用数据保存在 `%LOCALAPPDATA%\PasteOrbit`。

| 文件 | 内容 |
|---|---|
| `history.db` | 剪切板历史和本地内容 |
| `settings.json` | 应用设置和快捷键 |

文件类型记录只保存文件或文件夹路径，不复制原文件内容。

源文件移动、重命名或删除后，对应历史记录可能失效。

应用排除规则使用进程名，并以分号分隔多个进程。

默认排除规则包含常见密码管理器和远程桌面客户端。

## 本地备份

“设置 > 隐私与安全”提供备份导出和恢复功能。

备份包含历史数据库和应用设置。

备份正文使用 AES 加密，并包含完整性校验。

备份密钥受当前 Windows 用户凭据保护。

备份文件只能由能够解密该 Windows 用户凭据的环境恢复。

## 本地构建

构建环境需要 Windows、.NET 8 SDK 和可用的 NuGet 源。

```powershell
dotnet build .\src\PasteOrbit.App\PasteOrbit.App.csproj -c Release
```

生成结果位于 `src\PasteOrbit.App\bin\Release\net8.0-windows10.0.26100.0`。

## 本地打包

[发布脚本](Scripts/Publish.ps1)生成免安装目录和 ZIP 压缩包。

```powershell
.\Scripts\Publish.ps1 -Version 1.0.0
```

建议在 PowerShell 终端中运行，避免双击脚本后窗口自动关闭：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Publish.ps1 -Version 1.0.0
```

发布结果位于 `artifacts`：

- `artifacts\PasteOrbit-win-x64` 包含可直接运行的完整目录。
- `artifacts\PasteOrbit-1.0.0-win-x64.zip` 包含分发压缩包。

脚本会结束正在运行的 PasteOrbit 进程，并清理同名旧发布结果。

不需要 ZIP 时可以使用 `-NoArchive`：

```powershell
.\Scripts\Publish.ps1 -NoArchive
```

## GitHub Actions

[打包工作流](.github/workflows/package.yml)在 Windows Runner 上调用相同的发布脚本。

工作流支持以下触发方式：

- 在 GitHub Actions 页面手动运行并填写可选版本号。
- 推送名称以 `v` 开头的标签，例如 `v1.0.0`。

生成的 `PasteOrbit-win-x64` Artifact 保留十四天。

```powershell
git tag v1.0.0
git push origin v1.0.0
```

## 项目结构

| 路径 | 内容 |
|---|---|
| `src/PasteOrbit.App` | WinUI 3 桌面应用、剪切板监听和界面 |
| `src/PasteOrbit.Core` | 历史记录模型、排序和本地存储 |
| `tests/PasteOrbit.Core.Tests` | 核心逻辑测试 |
| `Scripts/Publish.ps1` | 本地发布和压缩脚本 |
| `.github/workflows/package.yml` | GitHub Actions 打包工作流 |

## 故障排查

全局快捷键无法注册时，需要在设置中更换已被其他应用占用的组合键。

面板无法自动粘贴时，目标应用可能以更高权限运行。

应用与目标程序需要处于相同权限级别，或者 PasteOrbit 需要以对应权限运行。

图片或文件预览失效时，需要确认历史记录引用的内容仍然存在。
