# PasteOrbit 局域网直连（首版）

Windows、Android 和 iPhone 在同一局域网内传输文字、图片和文件，不使用云端、中转或信令服务器。Windows 是发送/接收端；手机在前台主动领取电脑上的待发内容，不需要手机常驻监听。

## 使用

1. 在 Windows 历史面板顶部打开“设备直连”，开启“允许局域网设备连接”，选择与手机同网段的本机地址。
2. 复制本机配对码，通过可信渠道交给手机，在手机应用内粘贴并点击“配对电脑”。首次 iOS 配对需允许“本地网络”访问。
3. 手机发电脑：输入文字发送，或在其他应用选择系统“分享 → PasteOrbit → 发送”。电脑在设备窗口收件箱选择“保存到历史”或“保存并写入剪贴板”，不会自动向前台程序粘贴。
4. 电脑发手机：历史条目的更多菜单选择“发送到设备”。保持电脑接收服务开启，在手机应用点击“接收电脑发来的内容”，再手动复制或分享。
5. 电脑互发：双方分别导入对方的配对码；接收方保持服务开启。无需安装手机应用。

只连接私有 IPv4 地址，默认 TCP 48763；不支持互联网穿透、自动发现或手机互发。手机热点是否可用取决于热点设备的客户端隔离策略。Windows 防火墙若拦截，请仅对受信任的专用网络允许该程序/端口；应用不自动更改防火墙。IP 变化后重新导入配对码。电脑重启应用后需重新开启接收。

## 构建

### Windows

沿用仓库原有 WinUI 工程和工具链：

```powershell
dotnet build src/PasteOrbit.App/PasteOrbit.App.csproj
dotnet run --project tests/PasteOrbit.Core.Tests/PasteOrbit.Core.Tests.csproj
```

### Android

工程目录 `mobile/android`。需要 Android SDK 35、JDK 17 或 21、Gradle 8.9；Android Gradle Plugin 为 8.7.3。最低 Android 8（API 26）。可用 Android Studio 打开工程，配置本机 Gradle 8.9 和 SDK，或在该目录执行：

```powershell
gradle assembleDebug
```

产物在 `app/build/outputs/apk/debug/app-debug.apk`。仓库未附 Gradle wrapper 二进制；不要把本机 SDK 路径或签名密钥提交到仓库。Android 使用系统分享 Intent 和 FileProvider，只请求网络权限，不扫描存储或后台读取剪贴板。

### iOS

需要 macOS、Xcode 和 XcodeGen；最低 iOS 16。Windows 无法生成/签名 IPA。在 `mobile/ios` 执行：

```sh
xcodegen generate
open PasteOrbit.xcodeproj
```

在应用和 ShareExtension 两个 target 选择同一个签名 Team，启用同一个 App Group。示例标识为 `com.pasteorbit.mobile`、`com.pasteorbit.mobile.share` 和 `group.com.pasteorbit.mobile`；若需改为自己账号可注册的标识，同时修改 `project.yml` 和 `Shared/DeviceStore.swift` 内的 App Group。主应用配对成功后，系统分享菜单中启用 PasteOrbit 扩展。

iOS 文本、图片可复制到剪贴板；文件使用“分享 / 保存到文件”，不承诺其他应用能直接粘贴文件。部分来源应用不提供可读取的附件表示，会显示错误而不是自动发送路径。

## 限制与安全边界

- 文字、图片和文件不设固定总字节上限，按最多 256 KiB 一块加密、传输和落盘，实际受磁盘空间、文件系统和来源应用限制。每次仍最多 32 个文件，不支持文件夹，富文本只发送纯文字。Windows 最多 16 台配对设备、10 个待处理收件，每台手机最多 1 条待领取内容；手机收件箱最多 30 条。
- 传输上限与剪贴板/解码预算分开：Windows 历史仍使用内存对象，超过 8 MiB 的文字、图片保存为完整文件条目；小图片也需满足 3200 万像素解码预算，否则保留为原文件。iOS 大文字和大图片使用文件分享；Android 接收的文字超过 128 KiB UTF-8 时保留为文本文件，避免 Binder 大小限制。上述情况不截断、不拒收原始内容。
- 发送/接收前检查可用磁盘，写入每块时继续检查，并保留约 64 MiB 空闲空间。Windows 队列按块 DPAPI 加密；手机附件先流式暂存，发送结束或失败后清理临时文件。暂存、队列及导出可能同时占用空间，不代表只需要一份原始内容大小。
- Windows 和 Android 提供传输进度和取消；iOS 提供取消。取消在当前块或系统附件准备返回后生效。失败的未完成手机收件会清理，电脑待领取内容保留；上传失败尝试发送 abort。进程被强制结束或断网导致 abort 无法到达时，电脑上超过一小时未活动的未完成上传在下一次 begin 时清理，同一时间最多保留两个未完成上传。
- 配对码包含 32 字节随机密钥，等同访问凭证。只通过可信渠道交付，勿放入公共群/日志。Windows 复制按钮会跳过本应用历史，并禁用系统历史/漫游，但不能阻止其他剪贴板软件读取。
- 网络采用 HTTP 承载 AES-256-GCM 密文，密钥不在网络请求中发送；地址、时序和流量大小仍可见。两端时钟误差必须小于 5 分钟，Windows 持久保存短期重放记录。
- 首版是共享接收密钥的可信设备组，不是每台设备独立身份认证，也无前向保密。任何持有码的设备都属于可信组；撤销设备请在 Windows 点击“撤销全部配对”，会轮换密钥并清除所有待发件，保留已收内容。
- Windows 配对和队列使用 DPAPI；Android 配对使用 Android Keystore 加密；iOS 配对位于受文件保护的 App Group。手机收件、导出附件及 Windows 保存到历史的接收文件是本地文件，不承诺额外应用层加密。文件名不隐藏。
- Windows 收到的文件保存在数据库旁的 `DirectShare/files`。历史记录引用这些文件；删除历史不会自动删除文件，需自行管理已保存文件。待处理收件可在收件箱删除。
- 手机收到内容先落盘再确认，丢失确认后可重新领取同一条；发送成功但响应丢失时，不保证再次手动发送不产生重复内容。
- 无后台自动同步、自动粘贴或自动打开文件。接收来源仍需信任，应用不会执行附件。监听器有连接、大小和超时限制，但不应暴露到公网。

## 协议 v1

配对 JSON：`version: 1`、`name`、`endpoint`、`key`（32 字节 Base64）。

请求 `POST /v1/transfer`，固定 Content-Length，不支持 chunked/重定向。JSON 信封含 Base64 `nonce`（12 字节）、`data`（密文）和 `tag`（16 字节）。AES-GCM 附加认证数据为 UTF-8 `PasteOrbit.Direct/1`。

明文 JSON 含 `id`（UUID）、`time`（Unix 秒）、`kind`、`text`、`files`（`name`、Base64 `data`）、`deviceId`、`name`、`replyTo`。内容类型为 `text/image/files`；手机用 `hello` 注册，用 `poll` 领取，用 `ack` 的 `text` 携带已落盘内容 ID 确认。响应的 `replyTo` 必须匹配请求 ID，响应类型可为内容、`ok/empty/error`。领取时保留内容 ID，刷新响应时间。

### 分块扩展（电脑和手机需一起更新）

配对码、HTTP 路径和 AES-GCM 信封不变；旧端不支持新的分块指令，不会自动降级成整文件传输。单个控制请求仍有 16 MiB 信封上限，这不是内容总大小上限。

- `begin`：`text` 是 JSON 清单 `{kind,parts:[{name,length}]}`，长度为原始字节数（非负 Int64）；纯文字作为唯一 UTF-8 文本附件。响应 `ok.deviceId` 是服务器创建的随机上传 ID。元数据有独立大小限制。
- `part`：`deviceId` 为上传 ID，`text` 为 `附件索引:字节偏移`，`files[0].data` 为本块（1～262144 字节）。接收端校验连续偏移及清单长度，不接受错序、重复或超长块。
- `commit`：全部长度吻合后才进入收件箱；`abort` 只删除未完成上传。每块单独生成随机 GCM nonce，并由现有信封认证其 ID、偏移和数据。
- `poll` 新响应为 `bundle`，`text` 携带清单，`deviceId` 指向已完成队列。`read` 的 `text` 是 `附件索引:块序号`（不是字节偏移），返回 `chunk.files[0].data`。手机根据清单长度逐块读取、完整持久化后才 `ack`。
- 控制消息保留短期重放记录；`part` 由持久化偏移防重复，`read` 幂等，因此大文件不会耗尽原有 2048 条重放表。分块没有断点续传承诺，失败后可重新发送/领取。

## 验证范围

Core 测试覆盖加解密、错误密钥、篡改、过期、非法地址/路径、回环 HTTP、手机队列确认、重放及撤销配对；新增 17.25 MiB 文件 SHA256 校验、超过 8 MiB 的 UTF-8 文字往返、零字节文件、错序/重复块、不完整提交、取消清理和图片原始字节保留。Android/iOS 工程尚未在各自 SDK 和真机上编译/联调，不能将协议单元测试视为手机兼容性验证；交付前还需实际测试系统分享、局域网权限和不同附件来源。
