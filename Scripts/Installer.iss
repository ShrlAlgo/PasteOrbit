; 安装包版本。发布新版本时应与 Publish.ps1 的 -Version 参数保持一致。
#ifndef AppVersion
#define AppVersion "1.0.1"
#endif
; 安装文件来源于 Publish.ps1 生成的完整 win-x64 发布目录。
#define ReleaseDir "..\artifacts\PasteOrbit-win-x64"

[Setup]
; AppId 必须在后续版本中保持不变，Inno Setup 才能识别升级安装。
AppId={{D6C9A5F7-5C7E-4C1D-9F2A-7C9D8B3E4A11}
AppName=PasteOrbit
AppVersion={#AppVersion}
AppVerName=PasteOrbit {#AppVersion}
AppPublisher=PasteOrbit
; 使用当前用户目录安装，无需管理员权限，也不会影响其他 Windows 用户。
DefaultDirName={localappdata}\Programs\PasteOrbit
DefaultGroupName=PasteOrbit
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
; 当前发布产物只包含 x64 程序。
ArchitecturesAllowed=x64compatible
; 安装程序与免安装 ZIP 统一输出到 artifacts 目录。
OutputDir=..\artifacts
OutputBaseFilename=PasteOrbit-{#AppVersion}-Setup
SetupIconFile=..\Assets\PasteOrbit.ico
UninstallDisplayIcon={app}\Assets\PasteOrbit.ico
; 使用固实 LZMA2 压缩减小安装包体积。
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; 升级或卸载时允许 Inno Setup 请求关闭正在运行的 PasteOrbit。
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#AppVersion}.0
VersionInfoProductVersion={#AppVersion}
VersionInfoDescription=PasteOrbit 安装程序
VersionInfoProductName=PasteOrbit

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
; 中文语言文件随安装脚本提供，不依赖 Inno Setup 安装目录中的可选翻译。
Name: "chinesesimp"; MessagesFile: "Languages\ChineseSimplified.isl"

[Tasks]
; 两个选项均默认不勾选，由用户在安装时决定。
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加快捷方式："; Flags: unchecked
Name: "autostart"; Description: "随 Windows 启动 PasteOrbit"; GroupDescription: "附加选项："; Flags: unchecked

[Files]
; 保留发布目录中的 Assets、Themes、XBF、PRI 和所有运行依赖的相对路径。
Source: "{#ReleaseDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; 开始菜单快捷方式始终创建，桌面快捷方式由 desktopicon 任务控制。
Name: "{group}\PasteOrbit"; Filename: "{app}\PasteOrbit.exe"; WorkingDir: "{app}"; IconFilename: "{app}\Assets\PasteOrbit.ico"
Name: "{userdesktop}\PasteOrbit"; Filename: "{app}\PasteOrbit.exe"; WorkingDir: "{app}"; IconFilename: "{app}\Assets\PasteOrbit.ico"; Tasks: desktopicon

[Registry]
; 用户选择开机启动时写入 HKCU；卸载时删除由安装程序创建的值。
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "PasteOrbit"; ValueData: """{app}\PasteOrbit.exe"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
; 非静默安装完成后显示“启动 PasteOrbit”选项，并且不等待应用退出。
Filename: "{app}\PasteOrbit.exe"; Description: "启动 PasteOrbit"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent
