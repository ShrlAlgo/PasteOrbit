using System.Text.Json.Serialization;

namespace PasteOrbit.App;

[JsonSerializable(typeof(ClipboardTextContent))]
[JsonSerializable(typeof(string[]))]
internal sealed partial class AppJsonSerializerContext : JsonSerializerContext
{
}
