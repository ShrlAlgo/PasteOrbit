[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+(?:\.\d+)?$')]
    [string]$Version,

    [switch]$NoArchive
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$projectPath = Join-Path $repositoryRoot 'src\PasteOrbit.App\PasteOrbit.App.csproj'
$buildOutputDirectory = Join-Path $repositoryRoot 'src\PasteOrbit.App\bin\Release\net8.0-windows10.0.26100.0'
$artifactsDirectory = Join-Path $repositoryRoot 'artifacts'
$publishDirectory = Join-Path $artifactsDirectory 'PasteOrbit-win-x64'
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

Remove-PackagePath -Path $publishDirectory
Remove-PackagePath -Path $archivePath
New-Item -ItemType Directory -Path $publishDirectory -Force | Out-Null

# 避免正在运行的程序锁定 Release 发布文件。
Get-Process -Name 'PasteOrbit' -ErrorAction SilentlyContinue | Stop-Process -Force

$publishArguments = @(
    'publish',
    $projectPath,
    '--configuration', 'Release',
    '--runtime', 'win-x64',
    '--self-contained', 'false',
    '--output', $publishDirectory,
    '-p:WindowsAppSDKSelfContained=false',
    '-p:DebugType=None',
    '-p:DebugSymbols=false'
)
if (-not [string]::IsNullOrWhiteSpace($Version)) {
    $publishArguments += "-p:Version=$Version"
}

& dotnet @publishArguments
if ($LASTEXITCODE -ne 0) {
    throw "dotnet publish 失败，退出代码：$LASTEXITCODE"
}

# dotnet publish 当前不会把 WinUI 生成的 XBF/PRI 资源复制到非打包应用的发布目录。
# 这些文件是运行时加载 XAML 和资源索引所必需的，必须保留原有相对路径。
$winUiResourcePaths = @(
    'App.xbf',
    'MainWindow.xbf',
    'SettingsWindow.xbf',
    'PasteOrbit.pri',
    'Themes\AppBrushes.xbf'
)

foreach ($relativePath in $winUiResourcePaths) {
    $sourcePath = Join-Path $buildOutputDirectory $relativePath
    $destinationPath = Join-Path $publishDirectory $relativePath

    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw "缺少 WinUI 发布资源：$sourcePath"
    }

    $destinationDirectory = Split-Path -Parent $destinationPath
    New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
}

foreach ($relativePath in $winUiResourcePaths) {
    $publishedPath = Join-Path $publishDirectory $relativePath
    if (-not (Test-Path -LiteralPath $publishedPath -PathType Leaf)) {
        throw "WinUI 发布资源复制失败：$publishedPath"
    }
}
Get-ChildItem -LiteralPath $publishDirectory -Filter '*.pdb' -File -Recurse |
    Remove-Item -Force

if (-not $NoArchive) {
    Compress-Archive -Path (Join-Path $publishDirectory '*') -DestinationPath $archivePath -CompressionLevel Optimal
}

Write-Host "发布目录：$publishDirectory"
if (-not $NoArchive) {
    Write-Host "压缩包：$archivePath"
}
