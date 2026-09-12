using System.Globalization;
using System.Xml.Linq;

namespace PasteOrbit.App;

internal static class AppLocalization
{
    private static IReadOnlyDictionary<string, string> _resources = new Dictionary<string, string>();

    public static string CurrentLanguage { get; private set; } = "zh-CN";

    public static void SetLanguage(string? language)
    {
        CurrentLanguage = language is "en-US" or "zh-CN"
            ? language
            : CultureInfo.CurrentUICulture.Name.StartsWith("zh", StringComparison.OrdinalIgnoreCase) ? "zh-CN" : "en-US";
        var path = Path.Combine(AppContext.BaseDirectory, "Strings", CurrentLanguage, "Resources.resw");
        _resources = LoadResources(path);
    }

    public static string GetString(string key)
    {
        return _resources.TryGetValue(key, out var value) ? value : key;
    }

    public static string Format(string key, params object?[] arguments)
    {
        return string.Format(CultureInfo.CurrentCulture, GetString(key), arguments);
    }

    private static IReadOnlyDictionary<string, string> LoadResources(string path)
    {
        if (!File.Exists(path))
        {
            return new Dictionary<string, string>();
        }

        // 继续复用现有 RESW，WPF 与 WinUI 分支共用同一套翻译文本。
        return XDocument.Load(path)
            .Root?
            .Elements("data")
            .Where(element => element.Attribute("name") is not null)
            .ToDictionary(
                element => element.Attribute("name")!.Value,
                element => element.Element("value")?.Value ?? string.Empty,
                StringComparer.Ordinal)
            ?? new Dictionary<string, string>();
    }
}
