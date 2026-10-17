# Repository Guidelines

## Project Structure & Module Organization
PasteOrbit is a Windows x64 clipboard manager using C++17 and Qt 6.
- `src/app/`: startup, settings, localization, and Windows resources.
- `src/data/`: history storage and list model.
- `src/services/`: backup and updates.
- `src/ui/`: windows, dialogs, and history rendering.
- `src/resources/`: icons, localized strings, and `resources.qrc`.
- `Scripts/`: publishing script and installer definition; `.github/workflows/`: build and release automation.

Register application sources in `src/CMakeLists.txt`. Generated output belongs in ignored `build/` and `dist/` directories.

## Build, Test, and Development Commands
Use CMake 3.21+, Visual Studio 2026 with C++/CMake tools, and Qt 6.11.1 MSVC x64 including Svg at `C:\Qt`. Configuration fetches pinned Qlementine sources.

Run from the repository root:
- `cmake --preset windows-msvc`: configure the x64 build.
- `cmake --build --preset windows-debug`: build for debugging.
- `cmake --build --preset windows-release`: build Release.
- `& .\build\windows-msvc\bin\Debug\PasteOrbit.exe`: launch locally in PowerShell.
- `& .\Scripts\Publish.ps1`: create deployment files and ZIP under `dist/`.

Visual Studio debugging uses the `PasteOrbitNative` target and F5.

## Coding Style & Naming Conventions
Use UTF-8, CRLF, four-space indentation, and opening braces on the declaration line. Follow existing C++ style: `snake_case.cpp/.h` filenames, PascalCase types, camelCase methods, and trailing underscores for private members. Add concise Chinese comments to important methods and complex flows. No formatter or linter is configured in the build.

## Testing Guidelines
No automated test framework, test target, naming convention, or coverage threshold is configured. Build the affected target and manually verify clipboard capture, history navigation, paste shortcuts, and changed behavior. Check UI changes in light/dark themes and both supported languages. Record verification results in the PR.

## Commit & Pull Request Guidelines
Recent commits commonly use `fix: ...`, alongside plain English and Chinese summaries. Write concise, action-focused subjects. PRs should explain the problem, resulting behavior, relevant issue, and verification; include screenshots for visible UI changes. Exclude generated files and personal clipboard data.

## Agent Instructions
Search before reading; inspect only relevant snippets and skip dependencies/generated files. Make minimal changes without unsolicited refactoring, renaming, or dependency upgrades. Explanation-only requests require no edits or commands. Never commit without permission. Run minimal checks; stop after two unsuccessful attempts on the same issue and report findings. Close applications/background processes started for the task.
