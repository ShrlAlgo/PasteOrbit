using System.Windows;
using ClipVault.Core;

namespace ClipVault.App;

public partial class MainWindow : Window
{
    private const int MaxHistoryEntries = 5000;
    private static readonly TimeSpan RetentionPeriod = TimeSpan.FromDays(30);
    private readonly ClipboardHistory _history;
    private readonly GlobalHotKey _hotKey = new();
    private readonly ClipboardMonitor _monitor = new();
    private readonly ClipboardRepository _repository;
    private bool _storageAvailable = true;
    private string? _startupError;

    public MainWindow()
    {
        InitializeComponent();

        var dataDirectory = System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "ClipVault");
        _repository = new ClipboardRepository(System.IO.Path.Combine(dataDirectory, "history.db"));

        try
        {
            _history = new ClipboardHistory(_repository.InitializeAndLoad());
            var removedIds = _history.Cleanup(DateTimeOffset.UtcNow - RetentionPeriod, MaxHistoryEntries);
            _repository.Delete(removedIds);
        }
        catch (Exception)
        {
            _history = new ClipboardHistory();
            _storageAvailable = false;
            _startupError = "本地存储初始化失败，剪切板监听未启动";
        }

        _monitor.Captured += Monitor_Captured;
        _monitor.CaptureFailed += Monitor_CaptureFailed;
        _hotKey.Pressed += HotKey_Pressed;
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);

        if (!_storageAvailable)
        {
            StatusText.Text = _startupError;
            return;
        }

        _monitor.Start(this);
        _hotKey.Start(this);
        StatusText.Text = _repository.LastRecoveryPath is null
            ? $"正在监听剪切板 · {_history.GetSnapshot().Count} 条记录"
            : "检测到损坏数据库，原文件已保留并创建新库";
    }

    protected override void OnClosed(EventArgs e)
    {
        _monitor.Dispose();
        _hotKey.Dispose();
        base.OnClosed(e);
    }

    private void HotKey_Pressed(IntPtr foregroundWindow)
    {
        if (!IsVisible)
        {
            Show();
        }

        Activate();
        StatusText.Focus();
    }

    private void Monitor_Captured(ClipboardCapture capture)
    {
        if (!_storageAvailable)
        {
            return;
        }

        try
        {
            var item = _history.AddOrUpdate(capture);
            _repository.Upsert(item);
            var removedIds = _history.Cleanup(DateTimeOffset.UtcNow - RetentionPeriod, MaxHistoryEntries);
            _repository.Delete(removedIds);
            StatusText.Text = $"已保存 {_history.GetSnapshot().Count} 条剪切板记录";
        }
        catch (Exception)
        {
            // 持久化不可靠时停止接收新内容，避免界面状态与磁盘继续分叉。
            _storageAvailable = false;
            _monitor.Dispose();
            StatusText.Text = "本地存储异常，剪切板监听已停止";
        }
    }

    private void Monitor_CaptureFailed(Exception exception)
    {
        StatusText.Text = "本次剪切板内容读取失败，监听仍在继续";
    }
}
