using Microsoft.Windows.ApplicationModel.Resources;
using Microsoft.Windows.Globalization;

namespace PasteOrbit.App;

internal static class AppLocalization
{
    private static ResourceLoader _resourceLoader = new();

    public static string GetString(string resourceName)
    {
        return _resourceLoader.GetString(resourceName);
    }

    public static string Format(string resourceName, params object[] arguments)
    {
        return string.Format(GetString(resourceName), arguments);
    }

    public static void SetLanguage(string language)
    {
        var requestedLanguage = language switch
        {
            "zh-CN" => "zh-CN",
            "en-US" => "en-US",
            _ => App.SystemLanguage
        };

        if (!string.Equals(ApplicationLanguages.PrimaryLanguageOverride, requestedLanguage, StringComparison.OrdinalIgnoreCase))
        {
            ApplicationLanguages.PrimaryLanguageOverride = requestedLanguage;
        }

        // ResourceLoader 会缓存当前资源上下文，语言切换后重新创建才能让后续代码读取新语言。
        _resourceLoader = new ResourceLoader();
    }
}
