using System.Runtime.InteropServices;

using Microsoft.UI.Xaml.Input;

using Windows.System;

namespace PasteOrbit.App;

/// <summary>
/// 历史面板内部快捷键的解析、格式化和按键状态匹配器。
/// </summary>
internal readonly record struct PanelShortcut(VirtualKey Key, bool Control, bool Alt, bool Shift)
{
    private const int VirtualKeyLeftShift = 0xA0;
    private const int VirtualKeyRightShift = 0xA1;
    private const int VirtualKeyLeftControl = 0xA2;
    private const int VirtualKeyRightControl = 0xA3;
    private const int VirtualKeyLeftAlt = 0xA4;
    private const int VirtualKeyRightAlt = 0xA5;

    public static bool Matches(KeyRoutedEventArgs eventArgs, string shortcut)
    {
        return TryParse(shortcut, out var definition)
            && eventArgs.Key == definition.Key
            && IsKeyDown(VirtualKey.Control) == definition.Control
            && IsKeyDown(VirtualKey.Menu) == definition.Alt
            && IsKeyDown(VirtualKey.Shift) == definition.Shift;
    }

    public static bool TryFormat(
        VirtualKey key,
        bool control,
        bool alt,
        bool shift,
        out string shortcut)
    {
        shortcut = string.Empty;
        if (key is VirtualKey.Control or VirtualKey.Menu or VirtualKey.Shift
            or VirtualKey.LeftWindows or VirtualKey.RightWindows or VirtualKey.None)
        {
            return false;
        }

        var parts = new List<string>(4);
        if (control)
        {
            parts.Add("Ctrl");
        }

        if (alt)
        {
            parts.Add("Alt");
        }

        if (shift)
        {
            parts.Add("Shift");
        }

        parts.Add(FormatKey(key));
        shortcut = string.Join(" + ", parts);
        return true;
    }

    public static string NormalizeOrDefault(string? shortcut, string defaultShortcut)
    {
        return TryParse(shortcut, out var definition)
            ? definition.ToString()
            : defaultShortcut;
    }

    public static bool HasAnyModifierDown()
    {
        return IsKeyDown(VirtualKey.Control)
            || IsKeyDown(VirtualKey.Menu)
            || IsKeyDown(VirtualKey.Shift)
            || IsKeyDown(VirtualKey.LeftWindows)
            || IsKeyDown(VirtualKey.RightWindows);
    }

    public override string ToString()
    {
        TryFormat(Key, Control, Alt, Shift, out var shortcut);
        return shortcut;
    }

    private static bool TryParse(string? shortcut, out PanelShortcut definition)
    {
        // 最后一个 token 是主键，其余 token 必须是唯一修饰键。
        definition = default;
        if (string.IsNullOrWhiteSpace(shortcut))
        {
            return false;
        }

        var parts = shortcut.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (parts.Length == 0 || !TryParseKey(parts[^1], out var key))
        {
            return false;
        }

        var control = false;
        var alt = false;
        var shift = false;
        foreach (var part in parts[..^1])
        {
            switch (part.ToUpperInvariant())
            {
                case "CTRL" or "CONTROL" when !control:
                    control = true;
                    break;
                case "ALT" when !alt:
                    alt = true;
                    break;
                case "SHIFT" when !shift:
                    shift = true;
                    break;
                default:
                    return false;
            }
        }

        definition = new PanelShortcut(key, control, alt, shift);
        return true;
    }

    private static bool TryParseKey(string value, out VirtualKey key)
    {
        if (value.Length == 1 && char.IsLetter(value[0]))
        {
            return Enum.TryParse(value.ToUpperInvariant(), out key);
        }

        return Enum.TryParse(value, true, out key) && key != VirtualKey.None;
    }

    private static string FormatKey(VirtualKey key)
    {
        return key.ToString();
    }

    private static bool IsKeyDown(VirtualKey key)
    {
        if (IsNativeKeyDown((int)key))
        {
            return true;
        }

        return key switch
        {
            VirtualKey.Shift => IsNativeKeyDown(VirtualKeyLeftShift) || IsNativeKeyDown(VirtualKeyRightShift),
            VirtualKey.Control => IsNativeKeyDown(VirtualKeyLeftControl) || IsNativeKeyDown(VirtualKeyRightControl),
            VirtualKey.Menu => IsNativeKeyDown(VirtualKeyLeftAlt) || IsNativeKeyDown(VirtualKeyRightAlt),
            _ => false
        };
    }

    private static bool IsNativeKeyDown(int virtualKey) => (GetKeyState(virtualKey) & 0x8000) != 0;

    [DllImport("user32.dll")]
    private static extern short GetKeyState(int virtualKey);
}
