using System.Collections.Specialized;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Media.Imaging;

using PasteOrbit.Core;

namespace PasteOrbit.App;

internal sealed class WpfClipboardPlayback(ClipboardRepository repository, WpfClipboardMonitor monitor) : IDisposable
{
    private const int ShowRestore = 9;

    public async Task PasteAsync(
        ClipboardHistoryEntry item,
        IntPtr targetWindow,
        bool plainTextOnly,
        Func<bool>? restoreAutomationFocus = null)
    {
        var content = repository.LoadContent(item.Id);
        monitor.SuspendCapture();
        try
        {
            SetClipboard(item, content, plainTextOnly);
            await Task.Delay(70);
            if (!ActivateTargetWindow(targetWindow))
            {
                throw new Win32Exception("无法恢复粘贴目标窗口。");
            }

            await Task.Delay(80);
            restoreAutomationFocus?.Invoke();
            await Task.Delay(100);

            SendPasteShortcut();
        }
        finally
        {
            await Task.Delay(180);
            monitor.ResumeCapture();
        }
    }

    public void Dispose()
    {
    }

    private static void SetClipboard(ClipboardHistoryEntry item, byte[] content, bool plainTextOnly)
    {
        var data = new DataObject();
        switch (item.Kind)
        {
            case ClipboardContentKind.Text:
                var text = ClipboardTextContent.Deserialize(content);
                data.SetText(text.Text, TextDataFormat.UnicodeText);
                if (!plainTextOnly && !string.IsNullOrEmpty(text.Html))
                {
                    data.SetText(text.Html, TextDataFormat.Html);
                }

                if (!plainTextOnly && !string.IsNullOrEmpty(text.Rtf))
                {
                    data.SetText(text.Rtf, TextDataFormat.Rtf);
                }

                break;
            case ClipboardContentKind.Image:
                using (var stream = new MemoryStream(content, writable: false))
                {
                    var bitmap = new BitmapImage();
                    bitmap.BeginInit();
                    bitmap.CacheOption = BitmapCacheOption.OnLoad;
                    bitmap.StreamSource = stream;
                    bitmap.EndInit();
                    data.SetImage(bitmap);
                }

                break;
            case ClipboardContentKind.Files:
                var paths = JsonSerializer.Deserialize<string[]>(content) ?? [];
                var files = new StringCollection();
                files.AddRange(paths.Where(path => File.Exists(path) || Directory.Exists(path)).ToArray());
                if (files.Count == 0)
                {
                    throw new FileNotFoundException(AppLocalization.GetString("FileClipboardExpired"));
                }

                data.SetFileDropList(files);
                break;
        }

        Clipboard.SetDataObject(data, true);
    }

    private static void SendPasteShortcut()
    {
        Input[] inputs =
        [
            new() { Type = 1, Union = new InputUnion { Keyboard = new KeyboardInput { VirtualKey = 0x11 } } },
            new() { Type = 1, Union = new InputUnion { Keyboard = new KeyboardInput { VirtualKey = 0x56 } } },
            new() { Type = 1, Union = new InputUnion { Keyboard = new KeyboardInput { VirtualKey = 0x56, Flags = 0x0002 } } },
            new() { Type = 1, Union = new InputUnion { Keyboard = new KeyboardInput { VirtualKey = 0x11, Flags = 0x0002 } } }
        ];
        var sentCount = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>());
        if (sentCount != inputs.Length)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法向目标输入框发送粘贴快捷键。");
        }
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

        // 临时连接目标输入线程，允许从历史面板恢复原前台窗口。
        var currentThreadId = GetCurrentThreadId();
        var targetThreadId = GetWindowThreadProcessId(targetWindow, IntPtr.Zero);
        var attached = targetThreadId != 0
            && targetThreadId != currentThreadId
            && AttachThreadInput(currentThreadId, targetThreadId, true);
        try
        {
            BringWindowToTop(targetWindow);
            return SetForegroundWindow(targetWindow);
        }
        finally
        {
            if (attached)
            {
                AttachThreadInput(currentThreadId, targetThreadId, false);
            }
        }
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr windowHandle);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr windowHandle);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsIconic(IntPtr windowHandle);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShowWindowAsync(IntPtr windowHandle, int command);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool BringWindowToTop(IntPtr windowHandle);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AttachThreadInput(uint sourceThreadId, uint targetThreadId, bool attach);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr windowHandle, IntPtr processId);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint inputCount, Input[] inputs, int size);

    [StructLayout(LayoutKind.Sequential)]
    private struct Input
    {
        public uint Type;
        public InputUnion Union;
    }

    [StructLayout(LayoutKind.Explicit, Size = 32)]
    private struct InputUnion
    {
        [FieldOffset(0)] public KeyboardInput Keyboard;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KeyboardInput
    {
        public ushort VirtualKey;
        public ushort ScanCode;
        public uint Flags;
        public uint Time;
        public IntPtr ExtraInfo;
    }
}
