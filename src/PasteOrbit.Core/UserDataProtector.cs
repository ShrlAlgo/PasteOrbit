using System.Security.Cryptography;
using System.Text;

namespace PasteOrbit.Core;

public static class UserDataProtector
{
    public static byte[] Protect(byte[] value)
    {
        ArgumentNullException.ThrowIfNull(value);
        return ProtectedData.Protect(value, null, DataProtectionScope.CurrentUser);
    }

    public static byte[] Unprotect(byte[] value)
    {
        ArgumentNullException.ThrowIfNull(value);
        return ProtectedData.Unprotect(value, null, DataProtectionScope.CurrentUser);
    }

    public static byte[] ProtectText(string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        return Protect(Encoding.UTF8.GetBytes(value));
    }

    public static string UnprotectText(byte[] value)
    {
        return Encoding.UTF8.GetString(Unprotect(value));
    }
}
