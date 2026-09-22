using System.Net;
using System.Net.Sockets;
using System.Text;

namespace PasteOrbit.Core;

/// <summary>显式启动的局域网监听器，单连接处理、长度和超时限制避免后台无限分配。</summary>
public sealed class DirectTransferListener : IDisposable
{
    private readonly TcpListener _listener;
    private readonly CancellationTokenSource _stop = new();
    public Task Completion { get; }
    public int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;

    public DirectTransferListener(string key, Func<TransferMessage, CancellationToken, Task<TransferMessage>> receive, int port = DirectTransfer.Port)
    {
        _listener = new TcpListener(IPAddress.Any, port);
        _listener.Start(4);
        Completion = RunAsync(key, receive);
    }

    private async Task RunAsync(string key, Func<TransferMessage, CancellationToken, Task<TransferMessage>> receive)
    {
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                using var client = await _listener.AcceptTcpClientAsync(_stop.Token);
                if (client.Client.RemoteEndPoint is not IPEndPoint remote || !DirectTransfer.IsPrivateAddress(remote.Address)) continue;
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token);
                timeout.CancelAfter(TimeSpan.FromSeconds(25));
                var token = timeout.Token;
                await using var stream = client.GetStream();
                try
                {
                    // 只接受固定长度 POST，不支持代理、重定向、分块编码及连接复用。
                    var header = new List<byte>();
                    var one = new byte[1];
                    while (header.Count < 8192)
                    {
                        await stream.ReadExactlyAsync(one, token);
                        header.Add(one[0]);
                        if (header.Count >= 4 && header[^4] == 13 && header[^3] == 10 && header[^2] == 13 && header[^1] == 10) break;
                    }
                    if (header.Count < 4 || header[^4] != 13 || header[^3] != 10 || header[^2] != 13 || header[^1] != 10)
                        throw new InvalidDataException("请求头过长。");
                    var lines = Encoding.ASCII.GetString(header.ToArray()).Split("\r\n");
                    var lengths = lines.Where(s => s.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase)).ToArray();
                    if (lines[0] != "POST /v1/transfer HTTP/1.1" || lengths.Length != 1
                        || lines.Any(s => s.StartsWith("Transfer-Encoding:", StringComparison.OrdinalIgnoreCase))
                        || !int.TryParse(lengths[0][15..].Trim(), out var length) || length <= 0 || length > DirectTransfer.MaxWireBytes)
                        throw new InvalidDataException("不支持的请求。");
                    var wire = new byte[length];
                    await stream.ReadExactlyAsync(wire, token);
                    var request = DirectTransfer.Open(wire, key);
                    TransferMessage reply;
                    try { reply = await receive(request, token); }
                    catch (Exception) when (!token.IsCancellationRequested)
                    { reply = new TransferMessage { Kind = "error", Text = "接收失败：内容无效、重复、空间不足或收件箱已满。" }; }
                    var response = DirectTransfer.Seal(reply with { ReplyTo = request.Id, Time = DateTimeOffset.UtcNow.ToUnixTimeSeconds() }, key);
                    await stream.WriteAsync(Encoding.ASCII.GetBytes($"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {response.Length}\r\nConnection: close\r\n\r\n"), token);
                    await stream.WriteAsync(response, token);
                }
                catch (Exception) when (!_stop.IsCancellationRequested)
                {
                    // 畸形、未配对及超时连接直接断开，不向未认证客户端返回细节。
                }
            }
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
        catch (SocketException) when (_stop.IsCancellationRequested) { }
    }

    public void Dispose()
    {
        _stop.Cancel();
        _listener.Stop();
    }
}
