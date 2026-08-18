using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace PasteOrbit.App;

/// <summary>
/// 启动独立 PowerShell 更新器，等待主进程退出后完成安装和重启。
/// </summary>
internal static class UpdateInstaller
{
    private const string UpdaterScript = """
        param(
            [Parameter(Mandatory=$true)][int]$ProcessId,
            [Parameter(Mandatory=$true)][string]$InstallerPath,
            [Parameter(Mandatory=$true)][string]$ApplicationPath,
            [Parameter(Mandatory=$true)][string]$ApplicationDirectory
        )

        $ErrorActionPreference = 'Stop'

        try {
            try {
                Wait-Process -Id $ProcessId -Timeout 60 -ErrorAction Stop
            }
            catch {
            }

            $installerArguments = @(
                '/VERYSILENT'
                '/SUPPRESSMSGBOXES'
                '/NORESTART'
                '/CLOSEAPPLICATIONS'
                "/DIR=`"$ApplicationDirectory`""
            )
            $installer = Start-Process `
                -FilePath $InstallerPath `
                -ArgumentList $installerArguments `
                -Wait `
                -PassThru
            if ($installer.ExitCode -ne 0) {
                throw "安装程序退出代码：$($installer.ExitCode)"
            }

            if (Test-Path -LiteralPath $ApplicationPath -PathType Leaf) {
                Start-Process -FilePath $ApplicationPath -WorkingDirectory $ApplicationDirectory
            }
        }
        catch {
            try {
                if (Test-Path -LiteralPath $ApplicationPath -PathType Leaf) {
                    Start-Process -FilePath $ApplicationPath -WorkingDirectory $ApplicationDirectory
                }
            }
            catch {
            }
        }
        finally {
            Remove-Item -LiteralPath $InstallerPath -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue
        }
        """;

    public static bool TryStart(string installerPath, int processId, string applicationPath)
    {
        // 更新器必须独立于主进程运行，等待主程序退出后才能替换其文件。
        // 安装包和当前程序都必须存在，更新器才有可恢复的目标。
        if (!File.Exists(installerPath) || !File.Exists(applicationPath))
        {
            return false;
        }

        var applicationDirectory = Path.GetDirectoryName(applicationPath);
        if (string.IsNullOrWhiteSpace(applicationDirectory))
        {
            return false;
        }

        var scriptPath = Path.Combine(
            Path.GetTempPath(),
            $"PasteOrbit-update-{Guid.NewGuid():N}.ps1");
        try
        {
            // 等待、安装和重启逻辑由独立脚本执行，避免主进程锁定自身文件。
            File.WriteAllText(scriptPath, UpdaterScript, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            var startInfo = new ProcessStartInfo
            {
                FileName = "powershell.exe",
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden
            };
            startInfo.ArgumentList.Add("-NoLogo");
            startInfo.ArgumentList.Add("-NoProfile");
            startInfo.ArgumentList.Add("-ExecutionPolicy");
            startInfo.ArgumentList.Add("Bypass");
            startInfo.ArgumentList.Add("-File");
            startInfo.ArgumentList.Add(scriptPath);
            startInfo.ArgumentList.Add("-ProcessId");
            startInfo.ArgumentList.Add(processId.ToString(CultureInfo.InvariantCulture));
            startInfo.ArgumentList.Add("-InstallerPath");
            startInfo.ArgumentList.Add(installerPath);
            startInfo.ArgumentList.Add("-ApplicationPath");
            startInfo.ArgumentList.Add(applicationPath);
            startInfo.ArgumentList.Add("-ApplicationDirectory");
            startInfo.ArgumentList.Add(applicationDirectory);

            // 更新器在隐藏窗口中运行，不阻塞主线程。
            return Process.Start(startInfo) is not null;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or InvalidOperationException)
        {
            // 启动失败时删除临时脚本，调用方负责保留当前程序并提示错误。
            TryDeleteFile(scriptPath);
            return false;
        }
    }

    private static void TryDeleteFile(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
