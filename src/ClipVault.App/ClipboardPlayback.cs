using System.Collections.Specialized;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Media.Imaging;
using ClipVault.Core;

namespace ClipVault.App;

public static class ClipboardPlayback
{
    private const int RetryCount = 3;
    private const uint InputKeyboard = 1;
    private const uint KeyUp = 0x0002;
    private const ushort KeyControl = 0x11;
    private const ushort KeyV = 0x56;

    public static async Task<bool> PlayAsync(ClipboardItem item, IntPtr targetWindow, bool paste)
    {
        ArgumentNullException.ThrowIfNull(item);

        for (var attempt = 0; ; attempt++)
        {
            try
            {
                WriteClipboard(item);
                break;
            }
            catch (ExternalException) when (attempt < RetryCount - 1)
            {
                await Task.Delay(40 * (attempt + 1));
            }
        }

        if (!paste)
        {
            return true;
        }

        if (targetWindow == IntPtr.Zero || !SetForegroundWindow(targetWindow))
        {
            return false;
        }

        await Task.Delay(75);
        var inputs = new[]
        {
            CreateKeyInput(KeyControl, false),
            CreateKeyInput(KeyV, false),
            CreateKeyInput(KeyV, true),
            CreateKeyInput(KeyControl, true)
        };
        return SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>()) == inputs.Length;
    }

    private static void WriteClipboard(ClipboardItem item)
    {
        switch (item.Kind)
        {
            case ClipboardContentKind.Text:
                Clipboard.SetText(Encoding.UTF8.GetString(item.Content), TextDataFormat.UnicodeText);
                break;
            case ClipboardContentKind.Image:
                using (var stream = new MemoryStream(item.Content, false))
                {
                    var image = new BitmapImage();
                    image.BeginInit();
                    image.CacheOption = BitmapCacheOption.OnLoad;
                    image.StreamSource = stream;
                    image.EndInit();
                    image.Freeze();
                    Clipboard.SetImage(image);
                }
                break;
            case ClipboardContentKind.Files:
                var paths = JsonSerializer.Deserialize<string[]>(item.Content)
                    ?? throw new InvalidDataException("文件剪切板内容无效。");
                var collection = new StringCollection();
                collection.AddRange(paths);
                Clipboard.SetFileDropList(collection);
                break;
            default:
                throw new ArgumentOutOfRangeException(nameof(item), item.Kind, "不支持的剪切板内容类型。");
        }
    }

    private static Input CreateKeyInput(ushort key, bool keyUp)
    {
        return new Input
        {
            Type = InputKeyboard,
            Union = new InputUnion
            {
                Keyboard = new KeyboardInput
                {
                    VirtualKey = key,
                    Flags = keyUp ? KeyUp : 0
                }
            }
        };
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Input
    {
        public uint Type;
        public InputUnion Union;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct InputUnion
    {
        [FieldOffset(0)]
        public KeyboardInput Keyboard;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KeyboardInput
    {
        public ushort VirtualKey;
        public ushort ScanCode;
        public uint Flags;
        public uint Time;
        public UIntPtr ExtraInfo;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr hwnd);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint inputCount, Input[] inputs, int inputSize);
}
