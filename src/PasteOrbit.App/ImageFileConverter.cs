using Windows.Graphics.Imaging;
using Windows.Storage.Streams;

namespace PasteOrbit.App;

/// <summary>
/// 将剪贴板图片规范化为“粘贴为文件”使用的 PNG 字节。
/// </summary>
internal static class ImageFileConverter
{
    private static readonly SemaphoreSlim ConversionGate = new(1, 1);

    /// <summary>
    /// 将图片解码并重新编码为 PNG，确保历史卡片和文件导出统计同一份数据。
    /// </summary>
    public static async Task<byte[]> ConvertToPngAsync(byte[] content)
    {
        ArgumentNullException.ThrowIfNull(content);

        // 限制全局图片转换并发，避免缩略图和文件导出同时解码大图。
        await ConversionGate.WaitAsync();
        try
        {
            using var sourceStream = new InMemoryRandomAccessStream();
            using (var output = sourceStream.GetOutputStreamAt(0))
            using (var writer = new DataWriter(output))
            {
                writer.WriteBytes(content);
                await writer.StoreAsync();
                await writer.FlushAsync();
            }

            sourceStream.Seek(0);
            var decoder = await BitmapDecoder.CreateAsync(sourceStream);
            using var softwareBitmap = await decoder.GetSoftwareBitmapAsync(
                BitmapPixelFormat.Bgra8,
                BitmapAlphaMode.Premultiplied);
            using var destinationStream = new InMemoryRandomAccessStream();
            var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, destinationStream);
            encoder.SetSoftwareBitmap(softwareBitmap);
            await encoder.FlushAsync();
            return await ReadBytesAsync(destinationStream);
        }
        finally
        {
            ConversionGate.Release();
        }
    }

    private static async Task<byte[]> ReadBytesAsync(IRandomAccessStream stream)
    {
        stream.Seek(0);
        var size = checked((int)stream.Size);
        using var input = stream.GetInputStreamAt(0);
        using var reader = new DataReader(input);
        await reader.LoadAsync((uint)size);
        var bytes = new byte[size];
        reader.ReadBytes(bytes);
        return bytes;
    }
}
