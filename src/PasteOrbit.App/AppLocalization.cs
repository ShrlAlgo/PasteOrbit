using Microsoft.Windows.ApplicationModel.Resources;

namespace PasteOrbit.App;

internal static class AppLocalization
{
    private static readonly ResourceLoader ResourceLoader = new();

    public static string GetString(string resourceName)
    {
        return ResourceLoader.GetString(resourceName);
    }

    public static string Format(string resourceName, params object[] arguments)
    {
        return string.Format(GetString(resourceName), arguments);
    }
}
