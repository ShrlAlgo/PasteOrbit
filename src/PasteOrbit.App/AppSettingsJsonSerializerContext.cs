using System.Text.Json.Serialization;

namespace PasteOrbit.App;

[JsonSourceGenerationOptions(WriteIndented = true)]
[JsonSerializable(typeof(AppSettings))]
internal sealed partial class AppSettingsJsonSerializerContext : JsonSerializerContext
{
}
