# PasteOrbit

[简体中文](README.zh-CN.md)

<img width="360" height="500" alt="PixPin_2026-10-07_09-00-09" src="https://github.com/user-attachments/assets/a42bc60b-c55e-460a-a24e-49d2e35cdcd0" />

PasteOrbit is a Windows desktop clipboard history manager.

## Features

- Stores text, rich text, images, and file records.
- Displays clipboard history near the active input position.
- Supports filtering by content type and full-text search.
- Supports pasting as original content, plain text, or files.
- Supports previews for text, images, and file records.
- Supports pinning, deleting, and clearing unpinned records from the current list.
- Supports number-key quick paste for records in the current list.
- Supports saving text and image records as files.
- Supports excluding clipboard records from specified applications.
- Supports system tray presence and monitoring pause.
- Supports encrypted local backup and restore.
- Supports Chinese and English interfaces, plus light and dark themes.
- Uses a compact layout by default and supports configurable shortcuts, retention days, and history limits.

## System Requirements

- Windows 10 version 1809 or later.
- x64 processor and operating system.
- No .NET or Windows App Runtime installation is required for the release package.

## Build from source

- Visual Studio 2026 requires Desktop development with C++ and CMake tools; Qt 6.11.1 MSVC x64 (including Qt Svg) resides in `C:\Qt`. First configuration fetches a pinned Qlementine 1.5 development revision.
- Open the repository folder in Visual Studio, select `windows-msvc` and the `PasteOrbitNative` startup target, then press F5.
- Configure with `cmake --preset windows-msvc`; build with `cmake --build --preset windows-debug` or `windows-release`.
- After configuration, `build\windows-msvc\PasteOrbitNative.slnx` can also be opened directly with Debug/Release and x64 selected.
- The executable and Qt dependencies reside in `build\windows-msvc\bin\Debug` or `bin\Release`.
- `Scripts\Publish.ps1` writes the unpacked release and ZIP to `dist`; the installer is written there too.
- Local publishing prefers `C:\Qt` and VS tools; CI retains its MinGW build path.

Sources in `src` are grouped into `app` (startup, settings, localization), `data` (history storage and model), `services` (backup and updates), `ui` (windows and cards), and `resources` (icons, resource manifest, and localized strings).
`build/windows-msvc` contains the CMake-generated Visual Studio solution, project files, and build output; these files are not source files and are not committed.

## GitHub Actions

- The build workflow runs only when started manually from GitHub Actions.
- Pushing a `v<major>.<minor>.<patch>` tag builds the installer and ZIP and generates release notes from commits between the current and previous tags.

## Data and Privacy

- Clipboard history and application settings remain on the local device.
- File records store file or folder paths without copying the original files.
- Applications can be excluded from clipboard monitoring.
- Local backups are protected by the current Windows user credentials.
