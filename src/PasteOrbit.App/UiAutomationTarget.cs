using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Automation;

namespace PasteOrbit.App;

/// <summary>
/// 保存快捷键触发前的自动化输入元素，并提供定位与焦点恢复。
/// </summary>
internal sealed class UiAutomationTarget
{
    private readonly AutomationElement _focusedElement;
    private readonly int _processId;

    private UiAutomationTarget(AutomationElement focusedElement, int processId, Rect anchorBounds)
    {
        _focusedElement = focusedElement;
        _processId = processId;
        AnchorBounds = anchorBounds;
    }

    public Rect AnchorBounds { get; }

    public static UiAutomationTarget? Capture(IntPtr targetWindow)
    {
        if (targetWindow == IntPtr.Zero)
        {
            return null;
        }

        GetWindowThreadProcessId(targetWindow, out var targetProcessId);
        if (targetProcessId == 0)
        {
            return null;
        }

        try
        {
            var focusedElement = AutomationElement.FocusedElement;
            if (focusedElement is null
                || focusedElement.Current.ProcessId != targetProcessId
                || !focusedElement.Current.HasKeyboardFocus
                || !IsInputControlType(focusedElement.Current.ControlType.Id))
            {
                return null;
            }

            var controlBounds = focusedElement.Current.BoundingRectangle;
            var anchorBounds = TryGetCaretBounds(focusedElement, out var caretBounds)
                ? caretBounds
                : controlBounds;
            return IsUsableBounds(anchorBounds)
                ? new UiAutomationTarget(focusedElement, (int)targetProcessId, anchorBounds)
                : null;
        }
        catch (Exception exception) when (exception is ElementNotAvailableException
            or InvalidOperationException
            or COMException)
        {
            return null;
        }
    }

    public bool TryRestoreFocus()
    {
        try
        {
            if (_focusedElement.Current.ProcessId != _processId)
            {
                return false;
            }

            _focusedElement.SetFocus();
            return _focusedElement.Current.HasKeyboardFocus;
        }
        catch (Exception exception) when (exception is ElementNotAvailableException
            or InvalidOperationException
            or COMException)
        {
            return false;
        }
    }

    private static bool TryGetCaretBounds(AutomationElement element, out Rect bounds)
    {
        bounds = Rect.Empty;
        try
        {
            if (!element.TryGetCurrentPattern(TextPattern.Pattern, out var patternObject)
                || patternObject is not TextPattern textPattern)
            {
                return false;
            }

            // 空选择同样表示当前插入符，优先使用其屏幕矩形定位面板。
            foreach (var range in textPattern.GetSelection())
            {
                var rectangles = range.GetBoundingRectangles();
                foreach (var candidate in rectangles)
                {
                    if (IsUsableBounds(candidate))
                    {
                        bounds = candidate;
                        return true;
                    }
                }
            }

            return false;
        }
        catch (Exception exception) when (exception is ElementNotAvailableException
            or InvalidOperationException
            or COMException)
        {
            return false;
        }
    }

    private static bool IsInputControlType(int controlTypeId)
    {
        // Java/Swing 与终端输入区可能以 Custom 或 Pane 类型暴露键盘焦点。
        return controlTypeId is 50003 or 50004 or 50020 or 50025 or 50030 or 50033;
    }

    private static bool IsUsableBounds(Rect bounds)
    {
        return !bounds.IsEmpty
            && !double.IsNaN(bounds.X)
            && !double.IsNaN(bounds.Y)
            && !double.IsInfinity(bounds.X)
            && !double.IsInfinity(bounds.Y)
            && bounds.Width > 0
            && bounds.Height > 0;
    }

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr windowHandle, out uint processId);
}
