using System.Globalization;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PasteOrbit.App;

/// <summary>
/// 查询 GitHub 最新 Release，并下载经过校验的安装包。
/// </summary>
public sealed record UpdateCheckResult(
    Version CurrentVersion,
    Version LatestVersion,
    string ReleaseTag,
    string ReleaseNotes,
    Uri ReleaseUri,
    Uri? InstallerUri,
    string? InstallerFileName,
    long? InstallerSize)
{
    public bool IsUpdateAvailable => LatestVersion > CurrentVersion;

    public bool CanAutoUpdate => InstallerUri is not null;
}

/// <summary>
/// 查询 GitHub 最新版本并按 Release 资产下载更新安装包。
/// </summary>
public sealed class UpdateCheckService : IDisposable
{
    private static readonly Uri LatestReleaseEndpoint = new(
        "https://api.github.com/repos/ShrlAlgo/PasteOrbit/releases/latest");

    private readonly HttpClient _httpClient;
    private bool _disposed;

    public UpdateCheckService()
    {
        CurrentVersion = ResolveCurrentVersion();
        _httpClient = new HttpClient
        {
            Timeout = TimeSpan.FromMinutes(2)
        };
        _httpClient.DefaultRequestHeaders.UserAgent.ParseAdd($"PasteOrbit/{CurrentVersion}");
        _httpClient.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
    }

    public Version CurrentVersion { get; }

    public async Task<UpdateCheckResult?> CheckAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        // 从 GitHub 最新 Release 获取版本和发布资源元数据。
        using var response = await _httpClient.GetAsync(
            LatestReleaseEndpoint,
            HttpCompletionOption.ResponseHeadersRead,
            cancellationToken);
        response.EnsureSuccessStatusCode();

        await using var responseStream = await response.Content.ReadAsStreamAsync(cancellationToken);
        var release = await JsonSerializer.DeserializeAsync<GitHubReleasePayload>(
            responseStream,
            cancellationToken: cancellationToken);
        if (release is null
            || !TryParseReleaseVersion(release.TagName, out var latestVersion)
            || !Uri.TryCreate(release.HtmlUrl, UriKind.Absolute, out var releaseUri)
            || !string.Equals(releaseUri.Scheme, Uri.UriSchemeHttps, StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        // 安装包缺失时返回不可自动更新状态，由界面提供“不再提示”操作。
        var installer = SelectInstallerAsset(release.Assets);

        return new UpdateCheckResult(
            CurrentVersion,
            latestVersion,
            release.TagName!,
            release.Body?.Trim() ?? string.Empty,
            releaseUri,
            installer?.Uri,
            installer?.Name,
            installer?.Size);
    }

    public async Task<string> DownloadInstallerAsync(
        UpdateCheckResult update,
        IProgress<double>? progress = null,
        CancellationToken cancellationToken = default)
    {
        // 先下载到临时文件并校验大小，关闭文件流后再交给独立更新器。
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (update.InstallerUri is null || string.IsNullOrWhiteSpace(update.InstallerFileName))
        {
            throw new InvalidOperationException("当前 Release 没有可用的安装包。");
        }

        var fileName = Path.GetFileName(update.InstallerFileName);
        if (!string.Equals(fileName, update.InstallerFileName, StringComparison.Ordinal)
            || !fileName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("更新安装包名称无效。");
        }

        var updateDirectory = Path.Combine(
            Path.GetTempPath(),
            "PasteOrbit",
            "Updates",
            update.ReleaseTag.TrimStart('v', 'V'));
        // 每个版本使用独立临时目录，避免半包覆盖已完成的下载。
        Directory.CreateDirectory(updateDirectory);

        var installerPath = Path.Combine(updateDirectory, fileName);
        var downloadPath = installerPath + ".download";
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, update.InstallerUri);
            request.Headers.Accept.ParseAdd("application/octet-stream");
            using var response = await _httpClient.SendAsync(
                request,
                HttpCompletionOption.ResponseHeadersRead,
                cancellationToken);
            response.EnsureSuccessStatusCode();

            // GitHub 资产大小用于完整性校验和下载进度计算。
            var expectedSize = update.InstallerSize ?? response.Content.Headers.ContentLength;
            await using var responseStream = await response.Content.ReadAsStreamAsync(cancellationToken);
            long totalBytes = 0;
            await using (var installerFile = new FileStream(
                             downloadPath,
                             FileMode.Create,
                             FileAccess.Write,
                             FileShare.None,
                             81920,
                             FileOptions.Asynchronous | FileOptions.SequentialScan))
            {
                var buffer = new byte[81920];
                progress?.Report(expectedSize is > 0 ? 0 : -1);
                int bytesRead;
                while ((bytesRead = await responseStream.ReadAsync(buffer, cancellationToken)) > 0)
                {
                    await installerFile.WriteAsync(buffer.AsMemory(0, bytesRead), cancellationToken);
                    totalBytes += bytesRead;
                    progress?.Report(expectedSize is > 0
                        ? Math.Min(1, (double)totalBytes / expectedSize.Value)
                        : -1);
                }

                await installerFile.FlushAsync(cancellationToken);
            }

            if (totalBytes == 0
                || update.InstallerSize is > 0 && totalBytes != update.InstallerSize.Value)
            {
                throw new IOException("更新安装包下载不完整。");
            }

            // 下载完整后才把临时文件改成正式文件名。
            File.Move(downloadPath, installerPath, overwrite: true);
            progress?.Report(1);
            return installerPath;
        }
        catch
        {
            TryDeleteFile(downloadPath);
            TryDeleteFile(installerPath);
            throw;
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _httpClient.Dispose();
    }

    private static Version ResolveCurrentVersion()
    {
        // 优先读取发布流水线写入的版本号，再回退到程序集版本。
        var assembly = Assembly.GetEntryAssembly();
        var informationalVersion = assembly?
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?
            .InformationalVersion;
        if (TryParseVersionText(informationalVersion, out var version))
        {
            return version;
        }

        return NormalizeVersion(assembly?.GetName().Version ?? new Version(0, 0, 0));
    }

    private static bool TryParseReleaseVersion(string? tagName, out Version version)
    {
        // Release 标签必须使用 v主版本.次版本.修订版本格式。
        version = new Version(0, 0, 0);
        if (string.IsNullOrWhiteSpace(tagName)
            || tagName.Length < 2
            || (tagName[0] is not 'v' and not 'V'))
        {
            return false;
        }

        var parts = tagName[1..].Split('.');
        if (parts.Length is < 3 or > 4
            || parts.Any(part => part.Length == 0 || part.Any(character => character is < '0' or > '9'))
            || !parts.All(part => int.TryParse(part, NumberStyles.None, CultureInfo.InvariantCulture, out _)))
        {
            return false;
        }

        var numericParts = parts.Select(part => int.Parse(part, CultureInfo.InvariantCulture)).ToArray();
        version = numericParts.Length == 4
            ? new Version(numericParts[0], numericParts[1], numericParts[2], numericParts[3])
            : new Version(numericParts[0], numericParts[1], numericParts[2]);
        version = NormalizeVersion(version);
        return true;
    }

    private static bool TryParseVersionText(string? value, out Version version)
    {
        version = new Version(0, 0, 0);
        if (string.IsNullOrWhiteSpace(value))
        {
            return false;
        }

        var normalizedValue = value.Split(['+', '-'], 2)[0];
        if (!Version.TryParse(normalizedValue, out var parsedVersion))
        {
            return false;
        }

        version = NormalizeVersion(parsedVersion);
        return true;
    }

    private static Version NormalizeVersion(Version version)
    {
        return new Version(
            Math.Max(0, version.Major),
            Math.Max(0, version.Minor),
            Math.Max(0, version.Build));
    }

    private static InstallerAsset? SelectInstallerAsset(GitHubReleaseAssetPayload[]? assets)
    {
        // 仅选择 HTTPS 的 Inno Setup 安装包，不把 ZIP 当作自动更新目标。
        if (assets is null)
        {
            return null;
        }

        foreach (var asset in assets)
        {
            if (string.IsNullOrWhiteSpace(asset.Name)
                || !asset.Name.EndsWith("-Setup.exe", StringComparison.OrdinalIgnoreCase)
                || !Uri.TryCreate(asset.BrowserDownloadUrl, UriKind.Absolute, out var installerUri)
                || !string.Equals(installerUri.Scheme, Uri.UriSchemeHttps, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            return new InstallerAsset(installerUri, asset.Name, asset.Size);
        }

        return null;
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

    private sealed record InstallerAsset(Uri Uri, string Name, long Size);

    private sealed record GitHubReleasePayload(
        [property: JsonPropertyName("tag_name")] string? TagName,
        [property: JsonPropertyName("body")] string? Body,
        [property: JsonPropertyName("html_url")] string? HtmlUrl,
        [property: JsonPropertyName("assets")] GitHubReleaseAssetPayload[]? Assets);

    private sealed record GitHubReleaseAssetPayload(
        [property: JsonPropertyName("name")] string? Name,
        [property: JsonPropertyName("size")] long Size,
        [property: JsonPropertyName("browser_download_url")] string? BrowserDownloadUrl);
}
