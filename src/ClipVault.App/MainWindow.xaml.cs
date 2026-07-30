using System.Text;
using System.Windows;
using ClipVault.Core;

namespace ClipVault.App;

/// <summary>
/// Interaction logic for MainWindow.xaml
/// </summary>
public partial class MainWindow : Window
{
    private readonly ClipboardHistory _history = new();
    private readonly ClipboardMonitor _monitor = new();

    public MainWindow()
    {
        InitializeComponent();
        _monitor.Captured += Monitor_Captured;
        _monitor.CaptureFailed += Monitor_CaptureFailed;
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        _monitor.Start(this);
        StatusText.Text = "正在监听剪切板";
    }

    protected override void OnClosed(EventArgs e)
    {
        _monitor.Dispose();
        base.OnClosed(e);
    }

    private void Monitor_Captured(ClipboardCapture capture)
    {
        _history.AddOrUpdate(capture);
        StatusText.Text = $"已保存 {_history.GetSnapshot().Count} 条剪切板记录";
    }

    private void Monitor_CaptureFailed(Exception exception)
    {
        StatusText.Text = "本次剪切板内容读取失败，监听仍在继续";
    }
}
