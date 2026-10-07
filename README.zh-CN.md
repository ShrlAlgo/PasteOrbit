# PasteOrbit

[English](README.md)

<img width="360" height="500" alt="PixPin_2026-10-07_08-59-08" src="https://github.com/user-attachments/assets/3226f4bc-8f9b-4bf3-857e-80d07348963c" />


PasteOrbit 是一款 Windows 桌面剪切板历史工具。

## 功能

- 保存文本、富文本、图片和文件记录。
- 在当前输入位置附近显示剪切板历史面板。
- 支持按类型筛选和全文搜索。
- 支持原始内容、纯文本和文件形式的粘贴。
- 支持文本、图片和文件记录预览。
- 支持记录置顶、删除和按当前列表清除未置顶记录。
- 支持数字键快速粘贴当前列表中的记录。
- 支持保存文本和图片记录为文件。
- 支持排除指定应用的剪切板记录。
- 支持系统托盘常驻和监听暂停。
- 支持本地加密备份导出与恢复。
- 支持中英文界面、浅色和深色主题。
- 默认采用紧凑布局，并支持自定义快捷键、保留天数和记录数量上限。

## 系统要求

- Windows 10 版本 1809 或更高版本。
- x64 处理器和操作系统。
- 安装版已包含运行所需组件，无需单独安装 .NET 或 Windows App Runtime。

## 从源码构建

- 使用 Visual Studio 2026 的“使用 C++ 的桌面开发”和 CMake 工具，Qt 6.11.1 MSVC x64（含 Qt Svg）安装在 `C:\Qt`；首次配置会下载固定版本的 Qlementine 1.5 开发版源码。
- 在 VS 中选择“文件 → 打开 → 文件夹”，打开项目根目录，选择 `windows-msvc` 配置和 `PasteOrbitNative` 启动目标，按 F5 调试。
- 命令行使用 `cmake --preset windows-msvc` 配置，使用 `cmake --build --preset windows-debug` 或 `windows-release` 构建。
- 配置后也可直接打开 `build\windows-msvc\PasteOrbitNative.slnx`，选择 Debug/Release、x64。
- 可运行程序及 Qt 依赖位于 `build\windows-msvc\bin\Debug` 或 `bin\Release`；`Scripts\Publish.ps1` 将发布目录和 ZIP 写入 `dist`，安装程序也输出到该目录。
- 本机发布优先使用 `C:\Qt` 和 VS 工具；CI 保留 MinGW 构建路径。

源码位于 `src`：`app` 放入口、设置和本地化，`data` 放历史存储与模型，`services` 放备份与更新，`ui` 放窗口和卡片界面，`resources` 放图标、资源清单和中英文字符串。
`build/windows-msvc` 存放 CMake 生成的 Visual Studio 解决方案、项目文件和编译产物；这些文件不属于源码，也不需要提交。

## GitHub Actions

- 普通构建工作流仅支持在 GitHub Actions 页面手动运行。
- 推送 `v<主版本>.<次版本>.<补丁版本>` 标签会构建安装包和 ZIP，并根据当前标签与上一个标签之间的提交生成发布说明。

## 数据与隐私

- 剪切板历史和应用设置保存在本机。
- 文件记录保存文件或文件夹路径，不复制原文件内容。
- 应用支持排除密码管理器、远程桌面客户端等指定应用。
- 本地备份受当前 Windows 用户凭据保护。
