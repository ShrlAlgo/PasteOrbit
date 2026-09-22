using System.ComponentModel;
using System.Runtime.InteropServices;

namespace PasteOrbit.App;

/// <summary>
/// 拦截 Windows 剪切板历史快捷键，并将请求转发给应用。
/// </summary>
internal sealed class WindowsClipboardShortcut : IDisposable
{
    private const int LowLevelKeyboardHook = 13;
    private const int HookAction = 0;
    private const uint WmKeyDown = 0x0100;
    private const uint WmKeyUp = 0x0101;
    private const uint WmSystemKeyDown = 0x0104;
    private const uint WmSystemKeyUp = 0x0105;
    private const uint WmQuit = 0x0012;
    private const uint VirtualKeyLeftShift = 0xA0;
    private const uint VirtualKeyRightShift = 0xA1;
    private const uint VirtualKeyLeftControl = 0xA2;
    private const uint VirtualKeyRightControl = 0xA3;
    private const uint VirtualKeyLeftMenu = 0xA4;
    private const uint VirtualKeyRightMenu = 0xA5;
    private const uint VirtualKeyV = 0x56;
    private const uint VirtualKeyLeftWindows = 0x5B;
    private const uint VirtualKeyRightWindows = 0x5C;
    private const ushort VirtualKeyNone = 0xFC;
    private const uint LowLevelKeyboardInjected = 0x10;
    private const uint InputKeyboard = 1;
    private const uint KeyEventKeyUp = 0x0002;
    private const uint PeekMessageNoRemove = 0x0000;
    private const int KeyPressedMask = 0x8000;

    private readonly LowLevelKeyboardProcedure _hookProcedure;
    private readonly object _lifecycleLock = new();
    private Thread? _hookThread;
    private IntPtr _hook;
    private uint _hookThreadId;
    private string _startError = string.Empty;
    private bool _leftWindowsKeyDown;
    private bool _rightWindowsKeyDown;
    private uint _additionalModifiers;
    private bool _suppressVKey;
    private bool _disposed;

    public WindowsClipboardShortcut()
    {
        _hookProcedure = KeyboardHookProcedure;
    }

    public event Action<IntPtr>? Pressed;

    public bool TryStart(out string error)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        lock (_lifecycleLock)
        {
            if (_hookThread is not null)
            {
                error = string.Empty;
                return true;
            }

            var startCompleted = new ManualResetEventSlim();
            _startError = string.Empty;
            _hookThread = new Thread(() => RunHookMessageLoop(startCompleted))
            {
                IsBackground = true,
                Name = "PasteOrbit Win+V hook"
            };
            _hookThread.Start();
            startCompleted.Wait();
            startCompleted.Dispose();

            if (_hook != IntPtr.Zero)
            {
                error = string.Empty;
                return true;
            }

            _hookThread = null;
            error = _startError;
            return false;
        }
    }

    public void Stop()
    {
        Thread? hookThread;
        uint hookThreadId;
        lock (_lifecycleLock)
        {
            hookThread = _hookThread;
            hookThreadId = _hookThreadId;
        }

        if (hookThread is null)
        {
            return;
        }

        if (hookThreadId != 0)
        {
            PostThreadMessage(hookThreadId, WmQuit, IntPtr.Zero, IntPtr.Zero);
        }

        if (!ReferenceEquals(hookThread, Thread.CurrentThread))
        {
            hookThread.Join(TimeSpan.FromSeconds(2));
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        Stop();
        Pressed = null;
        GC.KeepAlive(_hookProcedure);
    }

    private void RunHookMessageLoop(ManualResetEventSlim startCompleted)
    {
        _hookThreadId = GetCurrentThreadId();
        PeekMessage(out _, IntPtr.Zero, 0, 0, PeekMessageNoRemove);
        _hook = SetWindowsHookEx(LowLevelKeyboardHook, _hookProcedure, GetModuleHandle(null), 0);
        if (_hook == IntPtr.Zero)
        {
            _startError = new Win32Exception(Marshal.GetLastWin32Error()).Message;
            startCompleted.Set();
            return;
        }

        startCompleted.Set();
        try
        {
            while (GetMessage(out _, IntPtr.Zero, 0, 0) > 0)
            {
            }
        }
        finally
        {
            UnhookWindowsHookEx(_hook);
            _hook = IntPtr.Zero;
            _hookThreadId = 0;
            _leftWindowsKeyDown = false;
            _rightWindowsKeyDown = false;
            _additionalModifiers = 0;
            _suppressVKey = false;
            lock (_lifecycleLock)
            {
                _hookThread = null;
            }
        }
    }

    private IntPtr KeyboardHookProcedure(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code < HookAction || lParam == IntPtr.Zero)
        {
            return CallNextHookEx(_hook, code, wParam, lParam);
        }

        var keyboardData = Marshal.PtrToStructure<LowLevelKeyboardData>(lParam);
        if ((keyboardData.Flags & LowLevelKeyboardInjected) != 0)
        {
            return CallNextHookEx(_hook, code, wParam, lParam);
        }

        var message = unchecked((uint)wParam.ToInt64());
        var isKeyDown = message is WmKeyDown or WmSystemKeyDown;
        var isKeyUp = message is WmKeyUp or WmSystemKeyUp;

        if (keyboardData.VirtualKey is VirtualKeyLeftShift
            or VirtualKeyRightShift
            or VirtualKeyLeftControl
            or VirtualKeyRightControl
            or VirtualKeyLeftMenu
            or VirtualKeyRightMenu)
        {
            // 左右修饰键分别记录，释放其中一个不会清除另一个的按下状态。
            var modifierBit = 1u << (int)(keyboardData.VirtualKey - VirtualKeyLeftShift);
            if (isKeyDown)
            {
                _additionalModifiers |= modifierBit;
            }
            else if (isKeyUp)
            {
                _additionalModifiers &= ~modifierBit;
            }

            return CallNextHookEx(_hook, code, wParam, lParam);
        }

        if (keyboardData.VirtualKey is VirtualKeyLeftWindows or VirtualKeyRightWindows)
        {
            if (keyboardData.VirtualKey == VirtualKeyLeftWindows)
            {
                if (isKeyDown)
                {
                    _leftWindowsKeyDown = true;
                }
                else if (isKeyUp)
                {
                    _leftWindowsKeyDown = false;
                }
            }
            else
            {
                if (isKeyDown)
                {
                    _rightWindowsKeyDown = true;
                }
                else if (isKeyUp)
                {
                    _rightWindowsKeyDown = false;
                }
            }

            return CallNextHookEx(_hook, code, wParam, lParam);
        }

        if (keyboardData.VirtualKey != VirtualKeyV)
        {
            return CallNextHookEx(_hook, code, wParam, lParam);
        }

        // 远程输入可能漏掉 Win 键抬起事件，处理 V 前用系统状态清除残留标记。
        _leftWindowsKeyDown &= IsKeyPressed(VirtualKeyLeftWindows);
        _rightWindowsKeyDown &= IsKeyPressed(VirtualKeyRightWindows);

        if (isKeyUp && _suppressVKey)
        {
            _suppressVKey = false;
            return new IntPtr(1);
        }

        if (isKeyDown && _suppressVKey)
        {
            return new IntPtr(1);
        }

        if (!isKeyDown
            || (!_leftWindowsKeyDown && !_rightWindowsKeyDown)
            || _additionalModifiers != 0)
        {
            return CallNextHookEx(_hook, code, wParam, lParam);
        }

        if (!_suppressVKey)
        {
            _suppressVKey = true;
            SendNeutralKeyStroke();
            try
            {
                Pressed?.Invoke(GetForegroundWindow());
            }
            catch (Exception exception)
            {
                System.Diagnostics.Debug.WriteLine($"Win + V 处理失败：{exception}");
            }
        }

        return new IntPtr(1);
    }

    private static bool IsKeyPressed(uint virtualKey)
    {
        return (GetAsyncKeyState((int)virtualKey) & KeyPressedMask) != 0;
    }

    private static void SendNeutralKeyStroke()
    {
        // Shell 需要看到另一个按键，否则释放 Win 键时会误打开开始菜单。
        KeyboardInput[] inputs =
        [
            KeyboardInput.Create(VirtualKeyNone, 0),
            KeyboardInput.Create(VirtualKeyNone, KeyEventKeyUp)
        ];
        if (SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<KeyboardInput>()) != inputs.Length)
        {
            System.Diagnostics.Debug.WriteLine(
                $"抑制开始菜单的按键发送失败：{Marshal.GetLastWin32Error()}");
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private readonly struct LowLevelKeyboardData
    {
        public readonly uint VirtualKey;
        public readonly uint ScanCode;
        public readonly uint Flags;
        public readonly uint Time;
        public readonly UIntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KeyboardInput
    {
        public uint Type;
        public KeyboardInputUnion Data;

        public static KeyboardInput Create(ushort virtualKey, uint flags)
        {
            return new KeyboardInput
            {
                Type = InputKeyboard,
                Data = new KeyboardInputUnion
                {
                    Keyboard = new KeyboardInputData
                    {
                        VirtualKey = virtualKey,
                        Flags = flags
                    }
                }
            };
        }
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct KeyboardInputUnion
    {
        [FieldOffset(0)]
        public KeyboardInputData Keyboard;

        // INPUT 联合体由最大的 MOUSEINPUT 决定大小，x64 下整个 INPUT 为 40 字节。
        [FieldOffset(0)]
        public MouseInputData Mouse;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MouseInputData
    {
        public int X;
        public int Y;
        public uint MouseData;
        public uint Flags;
        public uint Time;
        public UIntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KeyboardInputData
    {
        public ushort VirtualKey;
        public ushort ScanCode;
        public uint Flags;
        public uint Time;
        public UIntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeMessage
    {
        public IntPtr WindowHandle;
        public uint Message;
        public UIntPtr WParam;
        public IntPtr LParam;
        public uint Time;
        public NativePoint Point;
        public uint Private;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint
    {
        public int X;
        public int Y;
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate IntPtr LowLevelKeyboardProcedure(int code, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetWindowsHookEx(
        int hookIdentifier,
        LowLevelKeyboardProcedure hookProcedure,
        IntPtr moduleHandle,
        uint threadIdentifier);

    [DllImport("user32.dll")]
    private static extern IntPtr CallNextHookEx(
        IntPtr hook,
        int code,
        IntPtr wParam,
        IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnhookWindowsHookEx(IntPtr hook);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int virtualKey);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(
        uint inputCount,
        [In] KeyboardInput[] inputs,
        int inputSize);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int GetMessage(
        out NativeMessage message,
        IntPtr windowHandle,
        uint minimumMessage,
        uint maximumMessage);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PeekMessage(
        out NativeMessage message,
        IntPtr windowHandle,
        uint minimumMessage,
        uint maximumMessage,
        uint removeMessage);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostThreadMessage(
        uint threadIdentifier,
        uint message,
        IntPtr wParam,
        IntPtr lParam);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandle(string? moduleName);
}
