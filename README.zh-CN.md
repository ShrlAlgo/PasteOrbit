# PasteOrbit

[English](README.md)

<img width="360" height="500" alt="PixPin_2026-10-07_08-59-08" src="https://github.com/user-attachments/assets/3226f4bc-8f9b-4bf3-857e-80d07348963c" />


PasteOrbit 是一款基于 Qt Widgets 的桌面剪切板历史工具，提供 Windows 版本及初步的 macOS 适配。

## 功能

- 保存文本、富文本、图片和文件记录。
- 在当前输入位置附近显示剪切板历史面板。
- 支持按内容类型、来源应用筛选和全文搜索。
- 显示纯文本、Markdown、HTML、RTF 等文本格式标签。
- 支持识别网页链接和路径，分别通过浏览器或文件管理器打开。
- 支持原始内容、纯文本和文件形式的粘贴。
- 支持文本、图片和文件记录预览。
- 支持在预览中编辑文本、粘贴编辑结果或替换原记录。
- 支持将识别出的表格导出为 CSV 或 TSV。
- 支持记录置顶、删除和按当前列表清除未置顶记录。
- 支持数字键快速粘贴当前列表中的记录。
- 支持保存文本和图片记录为文件。
- 支持排除指定应用的剪切板记录。
- 支持系统托盘常驻和监听暂停。
- 支持本地加密备份导出与恢复。
- 支持检查更新、下载安装包并覆盖升级。
- 支持中英文界面、浅色和深色主题。
- 默认采用紧凑布局，并支持自定义快捷键、保留天数和记录数量上限。

选中历史记录后，默认使用 `Enter` 粘贴、`Shift+Enter` 粘贴为纯文本、`Space` 切换预览、`Ctrl+O` 打开识别到的目标；存在多个目标时显示选择列表。

## 系统要求

- Windows 10 版本 1809 或更高版本。
- x64 处理器和操作系统。
- 安装版已包含运行所需组件，无需单独安装 .NET 或 Windows App Runtime。

## 从源码构建

- 使用 Visual Studio 2026 的“使用 C++ 的桌面开发”和 CMake 工具，Qt 6.11.1 MSVC x64（含 Qt Svg）安装在 `C:\Qt`；首次配置会下载固定版本的 Qlementine 1.5 开发版源码。
- 在 VS 中选择“文件 → 打开 → 文件夹”，打开项目根目录，选择 `windows-msvc` 配置和 `PasteOrbitNative` 启动目标，按 F5 调试。
- 命令行使用 `cmake --preset windows-msvc` 配置，使用 `cmake --build --preset windows-debug` 或 `windows-release` 构建。
- 配置后也可直接打开 `build\windows-msvc\PasteOrbitNative.slnx`，选择 Debug/Release、x64。
- 可运行程序及 Qt 依赖位于 `build\windows-msvc\bin\Debug` 或 `bin\Release`。
- `Scripts\Publish.ps1` 将发布文件写入 `dist\PasteOrbit-win-x64`，ZIP 输出到 `dist`。
- 使用 Inno Setup 6 编译 `Scripts\Installer.iss`，安装程序输出到 `dist`。
- 本机发布使用 `C:\Qt` 和 VS 工具，复用 `build/windows-msvc` 的 Release 构建；CI 保留 MinGW 构建路径。

源码位于 `src`：`app` 放入口、设置和本地化，`data` 放历史存储与模型，`services` 放备份与更新，`ui` 放窗口和卡片界面，`platform` 放数据保护、全局快捷键和目标窗口粘贴交互，`resources` 放图标、资源清单和中英文字符串。
平台实现分别位于 `src/platform/windows` 和 `src/platform/macos`。
`build/windows-msvc` 存放 CMake 生成的 Visual Studio 解决方案、项目文件和编译产物；这些文件不属于源码，也不需要提交。

## macOS 构建与使用

macOS 适配尚待实机验证，最低部署版本为 macOS 13。
在 Mac 上安装 Xcode Command Line Tools、CMake、Ninja 和 Qt 6.11.1（含 Qt Svg），然后运行：

```sh
cmake --preset macos -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.1/macos"
cmake --build --preset macos-release
ctest --test-dir build/macos --output-on-failure
open build/macos/bin/Release/PasteOrbit.app
```

`CMAKE_PREFIX_PATH` 指向实际 Qt 安装目录。
Apple Silicon 和 Intel 默认按当前机器架构构建，不能用 Windows Qt 或 MSVC 生成 Mac 应用。
自检覆盖系统密钥加解密、篡改拒绝、历史存储及备份恢复，测试文件位于构建目录的临时子目录。

自动粘贴需要在“系统设置 → 隐私与安全性 → 辅助功能”中允许贴环；未授权时内容仍写入剪贴板，可手动粘贴。
默认全局快捷键是 Option+V，设置中的 `Ctrl` 表示 Command（Qt 的 macOS 映射）。
macOS 面板使用普通激活交互，不提供 Win+V 接管和管理员运行选项。
剪贴板来源按复制时的前台应用尽力识别，后台应用写入剪贴板时可能不准确。
登录启动通过当前用户的 LaunchAgent 配置，启用后在下次登录时生效。
更新提示打开版本下载页，不运行 Windows 安装器。

分发前运行 `macdeployqt build/macos/bin/Release/PasteOrbit.app -always-overwrite -codesign=-`，将 Qt 运行库放入应用包。
仓库的 macOS 工作流分别构建 Apple Silicon 和 Intel ZIP，支持手动运行及版本标签触发。
工作流使用临时签名，未配置 Apple Developer ID 签名和公证。

## GitHub Actions

- 普通构建工作流仅支持在 GitHub Actions 页面手动运行。
- 推送 `v<主版本>.<次版本>.<补丁版本>` 标签会构建安装包和 ZIP，并根据当前标签与上一个标签之间的提交生成发布说明。

## 数据与隐私

- 剪切板历史和应用设置保存在本机。
- 文件记录保存文件或文件夹路径，不复制原文件内容。
- 应用支持排除密码管理器、远程桌面客户端等指定应用。
- Windows 使用 DPAPI 保护正文和备份密钥；macOS 使用系统钥匙串保存 AES/HMAC 密钥。
- 当前备份绑定系统用户密钥，不能在 Windows 与 macOS 之间直接迁移。
- 文本格式标签随记录保存，旧记录首次显示时补算并缓存。
- 支持的旧版历史数据库会在启动时自动升级，旧备份在恢复时自动升级。
- 旧版程序不能直接打开升级后的数据库；如需回退，请保留升级前导出的备份。

## 许可证

PasteOrbit 采用 [Apache License 2.0](LICENSE.txt) 许可证。
第三方依赖遵循各自的许可证。
