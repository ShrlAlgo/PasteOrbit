[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+(?:\.\d+)?$')]
    [string]$Version,

    [switch]$NoArchive
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$msys2Root = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
$ucrtPrefix = Join-Path $msys2Root 'ucrt64'
$toolchainDirectory = Join-Path $ucrtPrefix 'bin'
$cmakePath = Join-Path $toolchainDirectory 'cmake.exe'
$deployToolPath = Join-Path $toolchainDirectory 'windeployqt.exe'
$dependencyToolPath = Join-Path $toolchainDirectory 'objdump.exe'
# 本机优先使用 VS/MSVC；保留现有 CI 的 MinGW 发布路径。
$useMsvc = (-not $env:MSYS2_ROOT) -and (Test-Path -LiteralPath 'C:\Qt\bin\windeployqt.exe')
if ($useMsvc) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsDirectory = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
    if (-not $vsDirectory) { throw '未找到 Visual Studio C++ 和 CMake 组件。' }
    Import-Module (Join-Path $vsDirectory 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsDirectory -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    $toolchainDirectory = 'C:\Qt\bin'
    $cmakePath = Join-Path $vsDirectory 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $deployToolPath = Join-Path $toolchainDirectory 'windeployqt.exe'
}
$buildDirectory = Join-Path $env:TEMP "PasteOrbit-native-release-$PID"
$deploymentDirectory = Join-Path $env:TEMP "PasteOrbit-native-deploy-$PID"
$artifactsDirectory = Join-Path $repositoryRoot 'dist'
$publishDirectory = Join-Path $artifactsDirectory 'PasteOrbit-win-x64'
$executablePath = Join-Path $publishDirectory 'PasteOrbit.exe'
$archiveName = if ([string]::IsNullOrWhiteSpace($Version)) {
    'PasteOrbit-win-x64.zip'
}
else {
    "PasteOrbit-$Version-win-x64.zip"
}
$archivePath = Join-Path $artifactsDirectory $archiveName

function Remove-PackagePath {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    $resolvedArtifacts = [System.IO.Path]::GetFullPath($artifactsDirectory)
    $resolvedTarget = [System.IO.Path]::GetFullPath($Path)
    if (-not $resolvedTarget.StartsWith($resolvedArtifacts + [System.IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝清理 artifacts 目录之外的路径：$resolvedTarget"
    }

    Remove-Item -LiteralPath $resolvedTarget -Recurse -Force
}

function Copy-ToolchainRuntimeDependencies {
    param([Parameter(Mandatory)][string]$Directory)

    $bundledNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $pendingBinaries = [System.Collections.Generic.Queue[string]]::new()
    foreach ($binary in Get-ChildItem -LiteralPath $Directory -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' }) {
        [void]$bundledNames.Add($binary.Name)
        $pendingBinaries.Enqueue($binary.FullName)
    }

    # 递归补齐 Qt/MinGW 的动态依赖，确保发布包脱离 MSYS2 PATH 也能启动。
    while ($pendingBinaries.Count -gt 0) {
        $binaryPath = $pendingBinaries.Dequeue()
        $imports = & $dependencyToolPath -p $binaryPath 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "无法读取运行库依赖：$binaryPath"
        }

        foreach ($line in $imports) {
            if ("$line" -notmatch '^\s*DLL Name:\s*(.+)$') {
                continue
            }

            $dependencyName = $Matches[1].Trim()
            if ($dependencyName -match '^(api-ms-|ext-ms-)' -or $bundledNames.Contains($dependencyName)) {
                continue
            }

            if (Test-Path -LiteralPath (Join-Path $env:WINDIR "System32\$dependencyName") -PathType Leaf) {
                continue
            }

            $dependencySource = Join-Path $toolchainDirectory $dependencyName
            if (-not (Test-Path -LiteralPath $dependencySource -PathType Leaf)) {
                throw "未找到运行依赖：$dependencyName（来自 $binaryPath）"
            }

            $dependencyTarget = Join-Path $Directory $dependencyName
            Copy-Item -LiteralPath $dependencySource -Destination $dependencyTarget -Force
            [void]$bundledNames.Add($dependencyName)
            $pendingBinaries.Enqueue($dependencyTarget)
        }
    }
}

Remove-PackagePath -Path $publishDirectory
Remove-PackagePath -Path $archivePath
New-Item -ItemType Directory -Path $publishDirectory -Force | Out-Null

if ((-not (Test-Path -LiteralPath $cmakePath -PathType Leaf)) -or (-not (Test-Path -LiteralPath $deployToolPath -PathType Leaf)) -or ((-not $useMsvc) -and (-not (Test-Path -LiteralPath $dependencyToolPath -PathType Leaf)))) {
    throw '未找到 MSYS2 UCRT64 的 CMake/Qt 工具。请先安装项目要求的 Qt 6 和 MinGW 工具链。'
}
$env:PATH = "$toolchainDirectory;$env:PATH"

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = '2.4.6'
}
$configureArguments = @(
    '-S', $repositoryRoot,
    '-B', $buildDirectory,
    '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DPASTEORBIT_VERSION=$Version"
)
if ($useMsvc) { $configureArguments += '-DCMAKE_PREFIX_PATH=C:/Qt', '-DCMAKE_CXX_COMPILER=cl' }
else { $configureArguments += "-DCMAKE_PREFIX_PATH=$ucrtPrefix", "-DCMAKE_CXX_COMPILER=$(Join-Path $toolchainDirectory 'g++.exe')" }
& $cmakePath @configureArguments
if ($LASTEXITCODE -ne 0) {
    throw "CMake 配置失败，退出代码：$LASTEXITCODE"
}

& $cmakePath --build $buildDirectory --parallel 4
if ($LASTEXITCODE -ne 0) {
    throw "Qt Release 构建失败，退出代码：$LASTEXITCODE"
}

$builtExecutable = Join-Path $buildDirectory 'bin\Release\PasteOrbit.exe'
if (-not (Test-Path -LiteralPath $builtExecutable -PathType Leaf)) {
    throw "未找到 Qt 构建产物：$builtExecutable"
}
if (Test-Path -LiteralPath $deploymentDirectory) {
    throw "部署临时目录已存在，拒绝覆盖：$deploymentDirectory"
}
New-Item -ItemType Directory -Path $deploymentDirectory | Out-Null
$stagedExecutable = Join-Path $deploymentDirectory 'PasteOrbit.exe'
Copy-Item -LiteralPath $builtExecutable -Destination $stagedExecutable
# 使用 Windows 原生网络后端；不部署 TUIO、GLib 和应用未使用的数据库驱动。
# 必须先筛选插件，再收集动态依赖，避免把 GLib 的整条 DLL 依赖链带入发布包。
& $deployToolPath --release --no-translations --no-compiler-runtime --no-system-dxc-compiler `
    --skip-plugin-types generic,styles `
    --exclude-plugins qglib,qsqlibase,qsqlmimer,qsqloci,qsqlmysql,qsqlodbc,qsqlpsql `
    --dir $deploymentDirectory $stagedExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Qt 运行库部署失败，退出代码：$LASTEXITCODE"
}
if ($useMsvc) {
    # 将当前 MSVC 工具集的发布版 CRT 放在应用目录，避免捆绑未执行的完整安装器。
    $redistRoot = Join-Path $env:VCToolsRedistDir 'x64'
    $crtDirectory = Get-ChildItem -LiteralPath $redistRoot -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
    if (-not $crtDirectory) { throw "未找到 MSVC 发布版运行库：$redistRoot" }
    foreach ($runtimeFile in (Get-ChildItem -LiteralPath $crtDirectory.FullName -File -Filter '*.dll')) {
        Copy-Item -LiteralPath $runtimeFile.FullName -Destination $deploymentDirectory -Force
    }
}
# MSYS2 的 windeployqt 不会稳定复制 MinGW 与 SQLite 插件的动态依赖，先显式带上基础运行库。
foreach ($runtimeName in $(if ($useMsvc) { @() } else { @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'libsqlite3-0.dll') })) {
    $runtimePath = Join-Path $toolchainDirectory $runtimeName
    if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
        throw "缺少 Qt 运行依赖：$runtimePath"
    }
    Copy-Item -LiteralPath $runtimePath -Destination (Join-Path $deploymentDirectory $runtimeName) -Force
}
# 应用只使用 SQLite，移除未用驱动及其额外依赖，缩小发布包并避免无关插件探测。
foreach ($driverName in @('qsqlibase.dll', 'qsqlmysql.dll', 'qsqlodbc.dll', 'qsqlpsql.dll')) {
    Remove-Item -LiteralPath (Join-Path $deploymentDirectory "sqldrivers\$driverName") -Force -ErrorAction SilentlyContinue
}
if (-not $useMsvc) { Copy-ToolchainRuntimeDependencies -Directory $deploymentDirectory }
New-Item -ItemType Directory -Path (Join-Path $deploymentDirectory 'Assets') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'src\resources\icons\PasteOrbit.ico') `
    -Destination (Join-Path $deploymentDirectory 'Assets\PasteOrbit.ico') -Force
Copy-Item -Path (Join-Path $deploymentDirectory '*') -Destination $publishDirectory -Recurse -Force

if (-not $NoArchive) {
    Compress-Archive -Path (Join-Path $publishDirectory '*') -DestinationPath $archivePath -CompressionLevel Optimal
}

Write-Host "发布目录：$publishDirectory"
if (-not $NoArchive) {
    Write-Host "压缩包：$archivePath"
}
