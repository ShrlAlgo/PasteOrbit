# PasteOrbit WinUI 3 迁移规格

## 1. 目标

将 PasteOrbit 的 WPF 界面替换为 C# WinUI 3，使用 Windows App SDK 的 Fluent 控件与主题重做历史面板和设置页。应用保持 Windows 10/11 桌面定位，现有本机历史数据、SQLite 存储、DPAPI 加密和剪贴板功能不改变。

## 2. 技术边界

- UI 项目继续使用 .NET 8，目标框架为 `net8.0-windows10.0.26100.0`。
- 使用稳定版 Windows App SDK 2.3.1，项目设为非 MSIX（`WindowsPackageType=None`），Debug 可直接运行，发布时支持自包含运行时。
- 移除 WPF 与 Melskin 引用；保留核心项目及其 SQLite/ProtectedData 依赖。
- WinUI 3 负责布局、主题、控件和窗口生命周期；剪贴板监听、全局快捷键、托盘、前台窗口恢复继续通过 Windows API 实现。
- 不在本次迁移中实现云同步或局域网同步，只保留现有设置页入口和状态说明。

## 3. 功能保持

### 3.1 历史面板

- `Ctrl + Shift + V` 打开面板，并保存调用快捷键前的前台窗口作为粘贴目标。
- 面板默认紧凑尺寸，位置支持鼠标附近、屏幕中央和屏幕右下角。
- 失去焦点时自动隐藏；窗口置顶开启时不自动隐藏。
- 搜索、类型筛选、键盘上下选择、Enter 粘贴、Esc 隐藏全部保留。
- 左键点击记录直接写回对应格式的剪贴板并向原窗口发送 Ctrl+V。
- 每条记录提供置顶、预览展开/收起、删除按钮；按钮操作不触发整条记录粘贴。
- 文本显示摘要，图片显示缩略图，文件显示路径摘要和失效提示。

### 3.2 设置页

使用 WinUI `NavigationView` 分组展示常规、快捷键、历史记录、云同步、局域网同步和隐私与安全。保留现有 `AppSettings` 字段、默认值、恢复默认和保存行为。

### 3.3 托盘与生命周期

- 使用 `Shell_NotifyIcon` 创建原生托盘图标，菜单包含打开历史、设置和退出。
- 普通关闭按钮只隐藏窗口；托盘退出才释放监听、快捷键和托盘资源并结束进程。
- 窗口关闭、失焦、快捷键调出和托盘菜单操作均通过同一套显示/隐藏方法处理。

## 4. 实现切片

1. 更新 `PasteOrbit.App.csproj`，加入 Windows App SDK 和 WinUI 3 应用属性，删除 WPF/Melskin 依赖。
2. 重写 `App.xaml`、`App.xaml.cs`、`MainWindow.xaml/.cs` 和 `SettingsWindow.xaml/.cs`，使用 WinUI Fluent 控件。
3. 将 `HistoryListItem` 改为 WinUI 绑定模型，保留文本、图片、文件预览与状态字段。
4. 将 `ClipboardMonitor`、`ClipboardPlayback`、`GlobalHotKey` 改为基于 Win32 HWND 的实现，不再依赖 `HwndSource`、WPF `Clipboard` 或 WPF 图像类型。
5. 新增轻量 Win32 消息桥和托盘服务，统一处理 `WM_CLIPBOARDUPDATE`、`WM_HOTKEY`、托盘回调和窗口激活。
6. 编译 App 与 Core，运行 Core 单元测试；确认输出目录和启动方式。

## 5. 验收标准

- 解决原 WPF 面板的残留背景和布局过松问题，WinUI 面板显示为单一 Fluent 背景、紧凑间距和一致控件样式。
- 应用能够生成并启动 WinUI 3 窗口，托盘图标可见，`Ctrl + Shift + V` 可调出面板。
- 点击文本、图片、文件记录可恢复剪贴板并粘贴到调用快捷键前的窗口；粘贴失败时至少保留已恢复的剪贴板内容。
- 搜索、筛选、置顶、预览、删除、设置保存和自动隐藏行为与现有需求一致。
- Core 数据库和已有历史记录无需迁移即可继续读取。

## 6. 风险与处理

- WinUI 3 无内置托盘控件，因此使用 Win32 `Shell_NotifyIcon`，避免再次依赖 WPF 控件库。
- WinUI 3 的窗口句柄和自定义无边框行为依赖 Windows App SDK/Win32 互操作，先以非 MSIX Debug 构建验证，再处理发布自包含配置。
- 当前 Core 使用 DPAPI，继续保持 Windows-only，不把本次 UI 迁移扩大为跨平台存储重构。
