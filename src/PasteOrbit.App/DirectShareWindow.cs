using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PasteOrbit.Core;

namespace PasteOrbit.App;

internal sealed class DirectShareWindow : Window
{
    private readonly DirectShareService _service;
    private readonly Func<TransferMessage, bool, CancellationToken, Task> _accept;
    private readonly ComboBox _addresses = new() { Header = "本机局域网地址", MinWidth = 420 };
    private readonly TextBox _pairing = new() { Header = "本机配对码（含密钥，仅交给可信设备）", IsReadOnly = true, TextWrapping = TextWrapping.Wrap };
    private readonly TextBox _remote = new() { Header = "另一台电脑的配对码", AcceptsReturn = true, TextWrapping = TextWrapping.Wrap };
    private readonly ListView _inbox = new() { Height = 150 };
    private readonly ListView _peers = new() { Height = 120 };
    private readonly TextBlock _status = new() { TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock _preview = new() { TextWrapping = TextWrapping.Wrap, MaxHeight = 100 };
    private readonly ToggleSwitch _enabled = new() { Header = "允许局域网设备连接（本次运行）" };
    private bool _accepting;
    private CancellationTokenSource? _receiveCancellation;

    public DirectShareWindow(DirectShareService service, Func<TransferMessage, bool, CancellationToken, Task> accept, Action<string> copyPairing)
    {
        _service = service;
        _accept = accept;
        Title = "PasteOrbit · 设备直连";
        var panel = new StackPanel { Spacing = 12, Padding = new Thickness(20) };
        Content = new ScrollViewer { Content = panel };
        panel.Children.Add(new TextBlock { Text = "同一局域网 · 无中间服务器 · 分块加密\n不设内容总大小上限，受可用磁盘空间约束。大文字和图片优先完整保存为文件。手机需打开配套应用接收；文件夹暂不支持。", TextWrapping = TextWrapping.Wrap });
        panel.Children.Add(_enabled);
        _enabled.IsOn = service.Listening;
        _enabled.Toggled += (_, _) => { Guard(() => service.SetListening(_enabled.IsOn)); _enabled.IsOn = service.Listening; };
        _addresses.ItemsSource = DirectShareService.Addresses;
        _addresses.SelectionChanged += (_, _) => Guard(() => _pairing.Text = service.PairingCode((string)_addresses.SelectedItem));
        panel.Children.Add(_addresses);
        panel.Children.Add(_pairing);
        panel.Children.Add(Button("复制本机配对码", () => Guard(() =>
        {
            if (_pairing.Text.Length == 0) throw new InvalidOperationException("请先选择局域网地址。");
            copyPairing(_pairing.Text);
            _status.Text = "配对码已复制，请勿发送到公共群组。";
        })));
        panel.Children.Add(_remote);
        panel.Children.Add(Button("添加电脑", () => Guard(() => { service.AddPeer(_remote.Text); _remote.Text = ""; })));
        panel.Children.Add(new TextBlock { Text = "已配对设备（发送入口在历史条目的更多菜单）" });
        panel.Children.Add(_peers);
        panel.Children.Add(new TextBlock { Text = "收件箱：选择内容后保存或复制，不自动粘贴到当前应用" });
        panel.Children.Add(_inbox);
        panel.Children.Add(_preview);
        _inbox.SelectionChanged += async (_, _) =>
        {
            var path = _inbox.SelectedItem as string;
            _preview.Text = "";
            if (path is null) return;
            try
            {
                var message = await Task.Run(() => service.ReadIncoming(path));
                if (_inbox.SelectedItem as string == path)
                    _preview.Text = message.Kind == "bundle" ? $"{DirectTransfer.ParseManifest(message.Text).Kind}：{string.Join("、", DirectTransfer.ParseManifest(message.Text).Parts.Select(p => $"{p.Name} ({p.Length / 1048576.0:F1} MiB)"))}"
                        : message.Kind == "text" ? message.Text[..Math.Min(300, message.Text.Length)]
                        : $"{(message.Kind == "image" ? "图片" : "文件")}：{string.Join("、", message.Files.Select(f => f.Name))}";
            }
            catch (Exception e) { if (_inbox.SelectedItem as string == path) _preview.Text = e.Message; }
        };
        panel.Children.Add(Button("保存到历史", () => _ = AcceptAsync(false)));
        panel.Children.Add(Button("保存并写入剪贴板", () => _ = AcceptAsync(true)));
        panel.Children.Add(Button("取消正在保存的内容", () => _receiveCancellation?.Cancel()));
        panel.Children.Add(Button("删除选中收件", () => Guard(() => { if (_inbox.SelectedItem is string path) service.DeleteIncoming(path); })));
        panel.Children.Add(Button("撤销全部配对并关闭接收", () => _ = ResetAsync()));
        panel.Children.Add(_status);
        service.Changed += OnChanged;
        Closed += (_, _) => { service.Changed -= OnChanged; _receiveCancellation?.Cancel(); };
        if (_addresses.Items.Count > 0) _addresses.SelectedIndex = 0;
        Refresh();
        AppWindow.Resize(new Windows.Graphics.SizeInt32(620, 860));
    }

    private static Button Button(string text, Action action)
    {
        var button = new Button { Content = text };
        button.Click += (_, _) => action();
        return button;
    }
    private void OnChanged() => DispatcherQueue.TryEnqueue(Refresh);
    private void Refresh() { _peers.ItemsSource = _service.Peers; _inbox.ItemsSource = _service.Incoming; }
    private void Guard(Action action)
    {
        try { action(); }
        catch (Exception e) { _status.Text = e.Message; }
    }
    private async Task AcceptAsync(bool clipboard)
    {
        if (_accepting) return;
        _accepting = true;
        using var cancellation = new CancellationTokenSource();
        _receiveCancellation = cancellation;
        try
        {
            if (_inbox.SelectedItem is not string path) return;
            var message = await Task.Run(() => _service.ReadIncoming(path));
            _status.Text = "正在完整保存内容，可取消；大内容会保留为文件。";
            await _accept(message, clipboard, cancellation.Token);
            _service.DeleteIncoming(path);
            _status.Text = clipboard ? "已保存并写入剪贴板。" : "已保存到历史。";
        }
        catch (Exception e) { _status.Text = e.Message; }
        finally { _accepting = false; _receiveCancellation = null; }
    }
    private async Task ResetAsync()
    {
        var dialog = new ContentDialog { XamlRoot = Content.XamlRoot, Title = "撤销全部配对？", Content = "旧配对码失效，待手机接收的内容会删除；已接收内容保留。", PrimaryButtonText = "撤销", CloseButtonText = "取消" };
        if (await dialog.ShowAsync() == ContentDialogResult.Primary)
            Guard(() => { _service.ResetPairing(); _enabled.IsOn = false; if (_addresses.SelectedItem is string endpoint) _pairing.Text = _service.PairingCode(endpoint); });
    }
}
