using System.Text.Json;

namespace PasteOrbit.App;

/// <summary>
/// PasteOrbit 的持久化设置模型。
/// </summary>
public sealed class AppSettings
{
    public string Language { get; set; } = string.Empty;

    public string GlobalHotKey { get; set; } = "Alt + V";

    public string PasteShortcut { get; set; } = "Enter";

    public string PlainTextPasteShortcut { get; set; } = "Shift + Enter";

    public string PreviewShortcut { get; set; } = "Space";

    public string PinShortcut { get; set; } = "Ctrl + P";

    public string DeleteShortcut { get; set; } = "Delete";

    public string PasteAsFileShortcut { get; set; } = "Ctrl + Shift + S";

    public bool StartWithWindows { get; set; }

    public bool AutoHideOnDeactivate { get; set; } = true;

    public bool MonitorText { get; set; } = true;

    public bool MonitorImages { get; set; } = true;

    public bool EnableImageOcr { get; set; } = true;

    public bool MonitorFiles { get; set; } = true;

    public string ExcludedApplications { get; set; } = "1Password; Bitwarden; KeePass; KeePassXC; mstsc; msrdc; Windows365";

    public string ThemeMode { get; set; } = "System";

    public int RetentionDays { get; set; } = 30;

    public int MaxHistoryEntries { get; set; } = 5000;

    public string? SkippedUpdateVersion { get; set; }
}

public sealed class AppSettingsStore
{
    private static readonly JsonSerializerOptions SerializerOptions = new() { WriteIndented = true };

    public AppSettingsStore(string path)
    {
        Path = path;
    }

    public string Path { get; }

    public AppSettings Load()
    {
        try
        {
            // 读取失败时使用默认设置，避免配置文件阻止应用启动。
            var settings = File.Exists(Path)
                ? JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(Path)) ?? new AppSettings()
                : new AppSettings();
            settings.ThemeMode = settings.ThemeMode switch
            {
                "跟随系统" => "System",
                "浅色" => "Light",
                "深色" => "Dark",
                _ => settings.ThemeMode
            };
            settings.Language = settings.Language is "zh-CN" or "en-US"
                ? settings.Language
                : string.Empty;
            return settings;
        }
        catch (JsonException)
        {
            return new AppSettings();
        }
    }

    public void Save(AppSettings settings)
    {
        // 先写入临时文件，再原子替换正式配置，避免中断时留下半份 JSON。
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
        var temporaryPath = $"{Path}.tmp";
        File.WriteAllText(temporaryPath, JsonSerializer.Serialize(settings, SerializerOptions));
        File.Move(temporaryPath, Path, true);
    }
}
