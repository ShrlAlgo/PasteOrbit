using System.IO;
using System.Text.Json;

namespace PasteOrbit.App;

public sealed class AppSettings
{
    public string GlobalHotKey { get; set; } = "Alt + V";

    public string PasteShortcut { get; set; } = "Enter";

    public string PlainTextPasteShortcut { get; set; } = "Shift + Enter";

    public string PreviewShortcut { get; set; } = "Space";

    public string PinShortcut { get; set; } = "Ctrl + P";

    public string DeleteShortcut { get; set; } = "Delete";

    public string PasteAsFileShortcut { get; set; } = "Ctrl + Shift + S";

    public string FocusSearchShortcut { get; set; } = "Ctrl + F";

    public bool StartWithWindows { get; set; }

    public bool AutoHideOnDeactivate { get; set; } = true;

    public bool MonitorText { get; set; } = true;

    public bool MonitorImages { get; set; } = true;

    public bool MonitorFiles { get; set; } = true;

    public string ThemeMode { get; set; } = "跟随系统";

    public string Density { get; set; } = "紧凑";

    public int RetentionDays { get; set; } = 30;

    public int MaxHistoryEntries { get; set; } = 5000;
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
            return File.Exists(Path)
                ? JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(Path)) ?? new AppSettings()
                : new AppSettings();
        }
        catch (JsonException)
        {
            return new AppSettings();
        }
    }

    public void Save(AppSettings settings)
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
        var temporaryPath = $"{Path}.tmp";
        File.WriteAllText(temporaryPath, JsonSerializer.Serialize(settings, SerializerOptions));
        File.Move(temporaryPath, Path, true);
    }
}
