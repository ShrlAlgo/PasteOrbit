using System.Security.Cryptography;
using System.Text;

namespace PasteOrbit.Core;

/// <summary>
/// 使用当前 Windows 用户的 DPAPI 保护本地剪贴板数据。
/// </summary>
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
