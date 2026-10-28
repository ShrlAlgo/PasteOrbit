[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [ValidatePattern('^\d+\.\d+\.\d+(?:\.\d+)?$')]
    [string]$Version
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw '未找到 Visual Studio Installer，请安装 Visual Studio 2026 C++ 和 CMake 工具。'
}
$vsDirectory = & $vswhere -latest -version '[18.0,19.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vsDirectory) {
    throw '未找到 Visual Studio 2026 C++ 和 CMake 工具。'
}
$cmakePath = Join-Path $vsDirectory 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmakePath -PathType Leaf)) {
    throw "未找到 CMake：$cmakePath"
}
$executablePath = Join-Path $repositoryRoot "build\windows-msvc\bin\$Configuration\PasteOrbit.exe"

# 只停止本次输出路径对应的程序，不按进程名关闭其他安装目录里的实例。
$running = Get-CimInstance Win32_Process -Filter "Name = 'PasteOrbit.exe'" | Where-Object {
    [string]::Equals($_.ExecutablePath, $executablePath, [StringComparison]::OrdinalIgnoreCase)
}
foreach ($process in $running) {
    Write-Host "停止正在占用构建目标的进程：$($process.ProcessId)"
    Stop-Process -Id $process.ProcessId -Force
    Wait-Process -Id $process.ProcessId -Timeout 10 -ErrorAction SilentlyContinue
    if (Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue) {
        throw "进程未退出，无法覆盖构建目标：$executablePath"
    }
}

# 配置和构建都禁用节点复用，退出后恢复调用方的环境和工作目录。
$previousNodeReuse = $env:MSBUILDDISABLENODEREUSE
Push-Location -LiteralPath $repositoryRoot
try {
    $env:MSBUILDDISABLENODEREUSE = '1'
    $configureArguments = @('--preset', 'windows-msvc')
    if ($Version) { $configureArguments += "-DPASTEORBIT_VERSION=$Version" }
    & $cmakePath @configureArguments
    if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败，退出代码：$LASTEXITCODE" }
    & $cmakePath --build --preset "windows-$($Configuration.ToLowerInvariant())" -- /nodeReuse:false
    if ($LASTEXITCODE -ne 0) { throw "构建失败，退出代码：$LASTEXITCODE" }
} finally {
    $env:MSBUILDDISABLENODEREUSE = $previousNodeReuse
    Pop-Location
}

Write-Host "构建成功：$executablePath"
