using System.Text.Json;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PasteOrbit.Core;
using Windows.Storage.Streams;

namespace PasteOrbit.App;

public sealed partial class MainWindow
{
    private DirectShareService? _directShare;
    private DirectShareWindow? _directShareWindow;

    private DirectShareService GetDirectShare() => _directShare ??= new DirectShareService(
        Path.Combine(Path.GetDirectoryName(_repository.DatabasePath)!, "DirectShare"));

    private void DirectShareButton_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            if (_directShareWindow is null)
            {
                _directShareWindow = new DirectShareWindow(GetDirectShare(), AcceptTransferAsync, CopyPairingCode);
                _directShareWindow.Closed += (_, _) => _directShareWindow = null;
            }
            _directShareWindow.Activate();
        }
        catch (Exception exception) { StatusText.Text = exception.Message; }
    }

    private void CopyPairingCode(string code)
    {
        // 配对码含设备密钥，不加入本应用历史，也不允许系统漫游。
        _monitor.SuspendCapture();
        try
        {
            var data = new Windows.ApplicationModel.DataTransfer.DataPackage();
            data.SetText(code);
            if (!Windows.ApplicationModel.DataTransfer.Clipboard.SetContentWithOptions(data,
                new Windows.ApplicationModel.DataTransfer.ClipboardContentOptions { IsAllowedInHistory = false, IsRoamable = false }))
                throw new InvalidOperationException("剪贴板被占用，请重试复制配对码。");
            Windows.ApplicationModel.DataTransfer.Clipboard.Flush();
        }
        finally { _monitor.ResumeCapture(); }
    }

    private async void RecordSendDevice_Click(object sender, RoutedEventArgs e)
    {
        if (GetHistoryListItem(sender) is not { } selected) return;
        var keepOpen = _settingsWindowOpen;
        _settingsWindowOpen = true;
        try
        {
            var service = GetDirectShare();
            var peers = new ComboBox { ItemsSource = service.Peers, MinWidth = 320 };
            if (peers.Items.Count > 0) peers.SelectedIndex = 0;
            var dialog = new ContentDialog { XamlRoot = RootGrid.XamlRoot, Title = "发送到设备", Content = peers,
                PrimaryButtonText = "发送", CloseButtonText = "取消", IsPrimaryButtonEnabled = peers.Items.Count > 0 };
            if (peers.Items.Count == 0)
            {
                DirectShareButton_Click(sender, e);
                return;
            }
            if (await dialog.ShowAsync() != ContentDialogResult.Primary || peers.SelectedItem is not SharePeer peer) return;
            var message = await Task.Run(() => BuildTransfer(selected.Item));
            using var cancellation = new CancellationTokenSource();
            var status = new TextBlock { Text = "准备传输…" };
            var transferring = new ContentDialog { XamlRoot = RootGrid.XamlRoot, Title = "正在发送", Content = status, CloseButtonText = "取消" };
            transferring.CloseButtonClick += (_, _) => cancellation.Cancel();
            var shown = transferring.ShowAsync();
            try { await service.SendAsync(peer, message, cancellation.Token, new Progress<long>(bytes => status.Text = $"已处理 {bytes / 1048576.0:F1} MiB")); }
            finally { transferring.Hide(); await shown; }
            var done = new ContentDialog { XamlRoot = RootGrid.XamlRoot, Title = "发送完成", Content = peer.Pairing is null
                ? "已加入待发件。请保持电脑接收服务开启，在手机应用中点击接收。" : "内容已到达对方收件箱。", CloseButtonText = "确定" };
            await done.ShowAsync();
        }
        catch (OperationCanceledException) { }
        catch (Exception exception)
        {
            var error = new ContentDialog { XamlRoot = RootGrid.XamlRoot, Title = "发送失败", Content = exception.Message, CloseButtonText = "确定" };
            await error.ShowAsync();
        }
        finally { _settingsWindowOpen = keepOpen; }
    }

    private TransferMessage BuildTransfer(ClipboardHistoryEntry entry)
    {
        var bytes = _repository.LoadContent(entry.Id);
        TransferMessage message;
        switch (entry.Kind)
        {
            case ClipboardContentKind.Text:
                message = new TransferMessage { Text = ClipboardTextContent.Deserialize(bytes).Text };
                break;
            case ClipboardContentKind.Image:
                // 历史图片保留原始编码，不能把 JPEG 等内容一律命名为 PNG。
                var extension = bytes.AsSpan().StartsWith(new byte[] { 137, 80, 78, 71 }) ? "png"
                    : bytes.AsSpan().StartsWith(new byte[] { 255, 216, 255 }) ? "jpg"
                    : bytes.AsSpan().StartsWith("GIF8"u8) ? "gif"
                    : bytes.AsSpan().StartsWith("BM"u8) ? "bmp" : "bin";
                message = new TransferMessage { Kind = "image", Files = [new($"image.{extension}", bytes)] };
                break;
            case ClipboardContentKind.Files:
                var paths = JsonSerializer.Deserialize<string[]>(bytes) ?? [];
                if (paths.Length > 32) throw new InvalidDataException("每次最多 32 个文件。");
                var files = new List<TransferFile>();
                foreach (var path in paths)
                {
                    if (Directory.Exists(path)) throw new InvalidDataException("暂不支持文件夹，请先压缩为文件。");
                    files.Add(new TransferFile(Path.GetFileName(path), []) { LocalPath = path });
                }
                message = new TransferMessage { Kind = "files", Files = files.ToArray() };
                break;
            default: throw new InvalidDataException("不支持此内容类型。");
        }
        DirectTransfer.ValidateContent(message);
        return message;
    }

    private async Task AcceptTransferAsync(TransferMessage message, bool clipboard, CancellationToken token)
    {
        if (!_storageAvailable) throw new InvalidOperationException("历史数据库不可用。");
        if (message.Kind == "bundle")
        {
            var manifest = DirectTransfer.ParseManifest(message.Text);
            var exported = await Task.Run(() => GetDirectShare().Export(message, token), token);
            // 大内容先完整保存，历史的 byte[] 模型不强行加载；以文件条目保留并支持再次分享。
            const long inlineBudget = 8L * 1024 * 1024;
            if (manifest.Kind == "text" && manifest.Parts[0].Length <= inlineBudget)
            {
                using var reader = new StreamReader(File.OpenRead(exported[0]), new System.Text.UTF8Encoding(false, true), false);
                try { message = new TransferMessage { Text = await reader.ReadToEndAsync(token) }; }
                catch (System.Text.DecoderFallbackException) { /* 非法 UTF-8 仍按原文件保存。 */ }
            }
            else if (manifest.Kind == "image" && manifest.Parts[0].Length <= inlineBudget)
            {
                using var stream = File.OpenRead(exported[0]).AsRandomAccessStream();
                try
                {
                    var decoder = await Windows.Graphics.Imaging.BitmapDecoder.CreateAsync(stream);
                    if ((long)decoder.PixelWidth * decoder.PixelHeight <= 32_000_000)
                        message = new TransferMessage { Kind = "image", Files = [new(manifest.Parts[0].Name, await File.ReadAllBytesAsync(exported[0]))] };
                }
                catch (Exception) { /* 无法解码时仍保留原始文件，不丢弃内容。 */ }
            }
            if (message.Kind == "bundle")
            {
                var fileCapture = new ClipboardCapture(ClipboardContentKind.Files, string.Join(Environment.NewLine, manifest.Parts.Select(p => p.Name)),
                    JsonSerializer.SerializeToUtf8Bytes(exported), "PasteOrbit Direct");
                await SaveTransferCaptureAsync(fileCapture, clipboard);
                return;
            }
        }
        DirectTransfer.ValidateContent(message);
        ClipboardCapture capture;
        switch (message.Kind)
        {
            case "text":
                capture = new ClipboardCapture(ClipboardContentKind.Text, message.Text,
                    new ClipboardTextContent(message.Text, null, null).Serialize(), "PasteOrbit Direct");
                break;
            case "image":
                using (var stream = new MemoryStream(message.Files[0].Data, false).AsRandomAccessStream())
                {
                    var decoder = await Windows.Graphics.Imaging.BitmapDecoder.CreateAsync(stream);
                    if ((long)decoder.PixelWidth * decoder.PixelHeight > 32_000_000)
                        throw new InvalidDataException("图片像素过多，请缩小后再发送。");
                    var thumbnail = await ClipboardMonitor.CreateThumbnailAsync(stream);
                    capture = new ClipboardCapture(ClipboardContentKind.Image, AppLocalization.GetString("ImageContent"),
                        message.Files[0].Data, "PasteOrbit Direct", thumbnail);
                }
                break;
            default:
                // 接收文件隔离到独立目录，不使用远端路径，也不自动执行文件。
                var folder = Path.Combine(GetDirectShare().ReceivedDirectory, Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(folder);
                var paths = new List<string>();
                for (var i = 0; i < message.Files.Length; i++)
                {
                    var file = message.Files[i];
                    var path = Path.Combine(folder, $"{i + 1}-{file.Name}");
                    await File.WriteAllBytesAsync(path, file.Data);
                    paths.Add(path);
                }
                capture = new ClipboardCapture(ClipboardContentKind.Files, string.Join(Environment.NewLine, message.Files.Select(f => f.Name)),
                    JsonSerializer.SerializeToUtf8Bytes(paths), "PasteOrbit Direct");
                break;
        }
        await SaveTransferCaptureAsync(capture, clipboard);
    }

    private async Task SaveTransferCaptureAsync(ClipboardCapture capture, bool clipboard)
    {
        var item = await Task.Run(() => _history.AddOrUpdate(capture));
        RefreshHistory();
        if (clipboard)
        {
            _monitor.SuspendCapture();
            try
            {
                // 目标句柄为零只写入剪贴板，绝不向当前前台窗口发送 Ctrl+V。
                await ClipboardPlayback.PlayAsync(item, capture.Content, IntPtr.Zero);
            }
            finally { _monitor.ResumeCapture(capturePending: true); }
        }
    }
}
