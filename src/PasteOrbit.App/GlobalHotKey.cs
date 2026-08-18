using System.ComponentModel;
using System.Runtime.InteropServices;

namespace PasteOrbit.App;

/// <summary>
/// 注册并维护打开历史面板的系统级快捷键。
/// </summary>
public sealed class GlobalHotKey : IDisposable
{
    private const int HotKeyId = 0x4356;
    private const uint WmHotKey = 0x0312;
    private const uint ModAlt = 0x0001;
    private const uint ModControl = 0x0002;
    private const uint ModShift = 0x0004;
    private const uint ModWindows = 0x0008;
    private const uint ModNoRepeat = 0x4000;

    private Win32MessageBridge? _bridge;
    private HotKeyDefinition? _definition;

    public event Action<IntPtr>? Pressed;

    public void Start(Win32MessageBridge bridge, string shortcut)
    {
        ArgumentNullException.ThrowIfNull(bridge);
        if (_bridge is not null)
        {
            throw new InvalidOperationException(AppLocalization.GetString("GlobalHotKeyAlreadyRegistered"));
        }

        if (!TryParseShortcut(shortcut, out var definition, out var parseError))
        {
            throw new ArgumentException(parseError, nameof(shortcut));
        }

        // 只有注册成功后才保存桥接对象，失败时允许调用方继续使用旧配置。
        Register(bridge, definition);
        _bridge = bridge;
        _definition = definition;
        _bridge.Message += Bridge_Message;
    }

    public bool TryReconfigure(Win32MessageBridge bridge, string shortcut, out string error)
    {
        ArgumentNullException.ThrowIfNull(bridge);

        if (!TryParseShortcut(shortcut, out var definition, out error))
        {
            return false;
        }

        if (_bridge is null)
        {
            try
            {
                Start(bridge, shortcut);
                error = string.Empty;
                return true;
            }
            catch (ArgumentException exception)
            {
                error = exception.Message;
                return false;
            }
            catch (Win32Exception exception)
            {
                error = exception.Message;
                return false;
            }
        }

        if (!ReferenceEquals(_bridge, bridge))
        {
            throw new InvalidOperationException(AppLocalization.GetString("GlobalHotKeyWrongMessageBridge"));
        }

        var previousDefinition = _definition;
        // 先注销旧组合键，注册失败时立即恢复旧组合键。
        UnregisterHotKey(_bridge.Handle, HotKeyId);
        if (RegisterHotKey(_bridge.Handle, HotKeyId, definition.Modifiers | ModNoRepeat, definition.VirtualKey))
        {
            _definition = definition;
            error = string.Empty;
            return true;
        }

        var registrationError = new Win32Exception(Marshal.GetLastWin32Error()).Message;
        if (previousDefinition is HotKeyDefinition previous)
        {
            if (RegisterHotKey(_bridge.Handle, HotKeyId, previous.Modifiers | ModNoRepeat, previous.VirtualKey))
            {
                _definition = previous;
            }
            else
            {
                _definition = null;
            }
        }

        error = AppLocalization.Format("GlobalHotKeyRegistrationFailedWithReason", definition.DisplayText, registrationError);
        return false;
    }

    public void Dispose()
    {
        if (_bridge is null)
        {
            return;
        }

        _bridge.Message -= Bridge_Message;
        UnregisterHotKey(_bridge.Handle, HotKeyId);
        _bridge = null;
        _definition = null;
    }

    internal static bool TryFormatShortcut(
        uint virtualKey,
        bool control,
        bool alt,
        bool shift,
        bool windows,
        out string shortcut)
    {
        shortcut = string.Empty;
        var keyName = GetVirtualKeyName(virtualKey);
        if (keyName is null || (!control && !alt && !shift && !windows))
        {
            return false;
        }

        var parts = new List<string>(5);
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

        if (windows)
        {
            parts.Add("Win");
        }

        parts.Add(keyName);
        shortcut = string.Join(" + ", parts);
        return true;
    }

    internal static bool TryNormalizeShortcut(string shortcut, out string normalizedShortcut)
    {
        if (TryParseShortcut(shortcut, out var definition, out _))
        {
            normalizedShortcut = definition.DisplayText;
            return true;
        }

        normalizedShortcut = string.Empty;
        return false;
    }

    private static bool TryParseShortcut(
        string shortcut,
        out HotKeyDefinition definition,
        out string error)
    {
        definition = default;
        error = string.Empty;
        if (string.IsNullOrWhiteSpace(shortcut))
        {
            error = AppLocalization.GetString("GlobalHotKeyEmpty");
            return false;
        }

        var parts = shortcut.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (parts.Length < 2)
        {
            error = AppLocalization.GetString("GlobalHotKeyNeedsModifierAndKey");
            return false;
        }

        uint modifiers = 0;
        // 修饰键必须位于最后一个主键之前，且不能重复。
        foreach (var part in parts[..^1])
        {
            var modifier = NormalizeToken(part);
            var modifierFlag = modifier switch
            {
                "CTRL" or "CONTROL" => ModControl,
                "ALT" => ModAlt,
                "SHIFT" => ModShift,
                "WIN" or "WINDOWS" => ModWindows,
                _ => 0u
            };

            if (modifierFlag == 0)
            {
                error = AppLocalization.Format("UnsupportedModifier", part);
                return false;
            }

            if ((modifiers & modifierFlag) != 0)
            {
                error = AppLocalization.Format("DuplicateModifier", part);
                return false;
            }

            modifiers |= modifierFlag;
        }

        if (!TryParseVirtualKey(parts[^1], out var virtualKey))
        {
            error = AppLocalization.Format("UnsupportedKey", parts[^1]);
            return false;
        }

        var keyName = GetVirtualKeyName(virtualKey)!;
        var displayParts = new List<string>(5);
        if ((modifiers & ModControl) != 0)
        {
            displayParts.Add("Ctrl");
        }

        if ((modifiers & ModAlt) != 0)
        {
            displayParts.Add("Alt");
        }

        if ((modifiers & ModShift) != 0)
        {
            displayParts.Add("Shift");
        }

        if ((modifiers & ModWindows) != 0)
        {
            displayParts.Add("Win");
        }

        displayParts.Add(keyName);
        definition = new HotKeyDefinition(modifiers, virtualKey, string.Join(" + ", displayParts));
        return true;
    }

    private static bool TryParseVirtualKey(string value, out uint virtualKey)
    {
        var normalized = NormalizeToken(value);
        if (normalized.Length == 1)
        {
            var character = normalized[0];
            if (character is >= 'A' and <= 'Z' or >= '0' and <= '9')
            {
                virtualKey = character;
                return true;
            }
        }

        if (normalized.StartsWith('F')
            && int.TryParse(normalized[1..], out var functionNumber)
            && functionNumber is >= 1 and <= 24)
        {
            virtualKey = (uint)(0x6F + functionNumber);
            return true;
        }

        virtualKey = normalized switch
        {
            "BACK" or "BACKSPACE" => 0x08,
            "TAB" => 0x09,
            "ENTER" or "RETURN" => 0x0D,
            "ESC" or "ESCAPE" => 0x1B,
            "SPACE" => 0x20,
            "PAGEUP" => 0x21,
            "PAGEDOWN" => 0x22,
            "END" => 0x23,
            "HOME" => 0x24,
            "LEFT" => 0x25,
            "UP" => 0x26,
            "RIGHT" => 0x27,
            "DOWN" => 0x28,
            "INSERT" => 0x2D,
            "DELETE" => 0x2E,
            _ => 0
        };
        return virtualKey != 0;
    }

    private static string? GetVirtualKeyName(uint virtualKey)
    {
        if (virtualKey is >= 0x41 and <= 0x5A or >= 0x30 and <= 0x39)
        {
            return ((char)virtualKey).ToString();
        }

        if (virtualKey is >= 0x70 and <= 0x87)
        {
            return $"F{virtualKey - 0x6F}";
        }

        return virtualKey switch
        {
            0x08 => "Backspace",
            0x09 => "Tab",
            0x0D => "Enter",
            0x1B => "Esc",
            0x20 => "Space",
            0x21 => "PageUp",
            0x22 => "PageDown",
            0x23 => "End",
            0x24 => "Home",
            0x25 => "Left",
            0x26 => "Up",
            0x27 => "Right",
            0x28 => "Down",
            0x2D => "Insert",
            0x2E => "Delete",
            _ => null
        };
    }

    private static string NormalizeToken(string token)
    {
        return token.Trim().Replace(" ", string.Empty, StringComparison.Ordinal).ToUpperInvariant();
    }

    private static void Register(Win32MessageBridge bridge, HotKeyDefinition definition)
    {
        if (RegisterHotKey(bridge.Handle, HotKeyId, definition.Modifiers | ModNoRepeat, definition.VirtualKey))
        {
            return;
        }

        var errorCode = Marshal.GetLastWin32Error();
        throw new Win32Exception(
            errorCode,
            AppLocalization.Format("GlobalHotKeyRegistrationFailed", definition.DisplayText));
    }

    private void Bridge_Message(uint message, IntPtr wParam, IntPtr lParam)
    {
        if (message == WmHotKey && wParam.ToInt32() == HotKeyId)
        {
            Pressed?.Invoke(GetForegroundWindow());
        }
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool RegisterHotKey(IntPtr windowHandle, int identifier, uint modifiers, uint virtualKey);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnregisterHotKey(IntPtr windowHandle, int identifier);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    private readonly record struct HotKeyDefinition(uint Modifiers, uint VirtualKey, string DisplayText);
}
