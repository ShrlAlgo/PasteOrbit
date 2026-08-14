using System.Runtime.InteropServices;
using System.Text.Json;

using PasteOrbit.Core;

using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Streams;

namespace PasteOrbit.App;

public static class ClipboardPlayback
{
    private const int RetryCount = 3;
    private const int ShowRestore = 9;
    private const uint InputKeyboard = 1;
    private const uint KeyUp = 0x0002;
    private const ushort KeyControl = 0x11;
    private const ushort KeyV = 0x56;
    private static IRandomAccessStream? _clipboardStream;

    public static async Task<bool> PlayAsync(
        ClipboardHistoryEntry item,
        byte[] content,
        IntPtr targetWindow,
        Func<bool>? restoreInputFocus = null,
        bool plainTextOnly = false)
    {
        ArgumentNullException.ThrowIfNull(item);
        ArgumentNullException.ThrowIfNull(content);

        for (var attempt = 0; ; attempt++)
        {
            try
            {
                await WriteClipboardAsync(item, content, plainTextOnly);
                break;
            }
            catch (COMException) when (attempt < RetryCount - 1)
            {
                await Task.Delay(40 * (attempt + 1));
            }
        }

        if (targetWindow == IntPtr.Zero)
        {
            return false;
        }

        if (!ActivateTargetWindow(targetWindow))
        {
            return false;
        }

        await Task.Delay(80);
        if (restoreInputFocus is not null)
        {
            // UI Automation 不一定能操作 Java/Swing 等自绘输入控件。激活目标窗口后，系统通常会恢复其原有子控件焦点。
            _ = restoreInputFocus();
        }

        await Task.Delay(100);
        var inputs = new[]
        {
            CreateKeyInput(KeyControl, false),
            CreateKeyInput(KeyV, false),
            CreateKeyInput(KeyV, true),
            CreateKeyInput(KeyControl, true)
        };
        return SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>()) == inputs.Length;
    }

    public static async Task<bool> SaveAsFileAsync(
        ClipboardHistoryEntry item,
        byte[] content,
        IntPtr targetWindow)
    {
        ArgumentNullException.ThrowIfNull(item);
        ArgumentNullException.ThrowIfNull(content);

        if (targetWindow == IntPtr.Zero || !ActivateTargetWindow(targetWindow))
        {
            return false;
        }

        await Task.Delay(40);
        return await ExplorerFilePaste.TrySaveAsync(item, content, targetWindow);
    }

    private static async Task WriteClipboardAsync(
        ClipboardHistoryEntry item,
        byte[] content,
        bool plainTextOnly)
    {
        if (item.Kind != ClipboardContentKind.Image)
        {
            _clipboardStream?.Dispose();
            _clipboardStream = null;
        }

        var package = new DataPackage { RequestedOperation = DataPackageOperation.Copy };
        switch (item.Kind)
        {
            case ClipboardContentKind.Text:
                var textContent = ClipboardTextContent.Deserialize(content);
                package.SetText(textContent.Text);
                if (!plainTextOnly && !string.IsNullOrEmpty(textContent.Html))
                {
                    package.SetHtmlFormat(textContent.Html);
                }

                if (!plainTextOnly && !string.IsNullOrEmpty(textContent.Rtf))
                {
                    package.SetRtf(textContent.Rtf);
                }

                break;
            case ClipboardContentKind.Image:
                _clipboardStream?.Dispose();
                _clipboardStream = new InMemoryRandomAccessStream();
                using (var output = _clipboardStream.GetOutputStreamAt(0))
                using (var writer = new DataWriter(output))
                {
                    writer.WriteBytes(content);
                    await writer.StoreAsync();
                    await writer.FlushAsync();
                }

                _clipboardStream.Seek(0);
                package.SetBitmap(RandomAccessStreamReference.CreateFromStream(_clipboardStream));
                break;
            case ClipboardContentKind.Files:
                var paths = JsonSerializer.Deserialize<string[]>(content)
                    ?? throw new InvalidDataException(AppLocalization.GetString("FileClipboardInvalid"));
                var storageItems = new List<IStorageItem>();
                foreach (var path in paths)
                {
                    if (File.Exists(path))
                    {
                        storageItems.Add(await StorageFile.GetFileFromPathAsync(path));
                    }
                    else if (Directory.Exists(path))
                    {
                        storageItems.Add(await StorageFolder.GetFolderFromPathAsync(path));
                    }
                }

                if (storageItems.Count == 0)
                {
                    throw new FileNotFoundException(AppLocalization.GetString("FileClipboardExpired"));
                }

                package.SetStorageItems(storageItems);
                break;
            default:
                throw new ArgumentOutOfRangeException(
                    nameof(item),
                    item.Kind,
                    AppLocalization.GetString("UnsupportedClipboardContentType"));
        }

        Clipboard.SetContent(package);
        Clipboard.Flush();
    }

    private static bool ActivateTargetWindow(IntPtr targetWindow)
    {
        if (targetWindow == IntPtr.Zero || !IsWindow(targetWindow))
        {
            return false;
        }

        if (IsIconic(targetWindow))
        {
            ShowWindowAsync(targetWindow, ShowRestore);
        }

        var currentThreadId = GetCurrentThreadId();
        var targetThreadId = GetWindowThreadProcessId(targetWindow, IntPtr.Zero);
        var attached = targetThreadId != 0
            && targetThreadId != currentThreadId
            && AttachThreadInput(currentThreadId, targetThreadId, true);
        try
        {
            BringWindowToTop(targetWindow);
            if (!SetForegroundWindow(targetWindow))
            {
                return false;
            }

            return true;
        }
        finally
        {
            if (attached)
            {
                AttachThreadInput(currentThreadId, targetThreadId, false);
            }
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
        public MouseInput Mouse;

        [FieldOffset(0)]
        public KeyboardInput Keyboard;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MouseInput
    {
        public int DeltaX;
        public int DeltaY;
        public uint MouseData;
        public uint Flags;
        public uint Time;
        public UIntPtr ExtraInfo;
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
    private static extern bool IsWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsIconic(IntPtr hwnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShowWindowAsync(IntPtr hwnd, int command);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool BringWindowToTop(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, IntPtr processId);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AttachThreadInput(uint attachThreadId, uint attachToThreadId, bool attach);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern IntPtr SetFocus(IntPtr hwnd);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint inputCount, Input[] inputs, int inputSize);
}
