using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Security;
using System.Text;
using Microsoft.CSharp.RuntimeBinder;
using PasteOrbit.Core;
using Windows.Graphics.Imaging;
using Windows.Storage.Streams;

namespace PasteOrbit.App;

internal static class ExplorerFilePaste
{
    private const int ShcneUpdatedir = 0x00001000;
    private const uint ShcnfPathW = 0x0005;

    public static async Task<bool> TrySaveAsync(
        ClipboardHistoryEntry item,
        byte[] content,
        IntPtr targetWindow)
    {
        ArgumentNullException.ThrowIfNull(item);
        ArgumentNullException.ThrowIfNull(content);

        if (targetWindow == IntPtr.Zero)
        {
            return false;
        }

        if (item.Kind is not (ClipboardContentKind.Text or ClipboardContentKind.Image))
        {
            return false;
        }

        var folderPath = TryGetCurrentFolderPath(targetWindow);
        if (folderPath is null)
        {
            return false;
        }

        var extension = item.Kind == ClipboardContentKind.Text ? ".txt" : ".png";
        var filePath = CreateUniqueFilePath(folderPath, extension);
        try
        {
            if (item.Kind == ClipboardContentKind.Text)
            {
                var text = Encoding.UTF8.GetString(content);
                await File.WriteAllTextAsync(
                    filePath,
                    text,
                    new UTF8Encoding(encoderShouldEmitUTF8Identifier: true),
                    CancellationToken.None);
            }
            else
            {
                var pngContent = await ConvertImageToPngAsync(content);
                await File.WriteAllBytesAsync(filePath, pngContent, CancellationToken.None);
            }

            SHChangeNotify(ShcneUpdatedir, ShcnfPathW, folderPath, IntPtr.Zero);
            return true;
        }
        catch (Exception exception) when (exception is IOException
            or UnauthorizedAccessException
            or SecurityException
            or InvalidDataException
            or InvalidOperationException
            or ArgumentException
            or NotSupportedException
            or COMException)
        {
            Debug.WriteLine($"资源管理器文件粘贴失败：{exception}");
            return false;
        }
    }

    private static string? TryGetCurrentFolderPath(IntPtr targetWindow)
    {
        if (!IsExplorerWindow(targetWindow))
        {
            return null;
        }

        object? shellObject = null;
        object? windowsObject = null;
        try
        {
            var shellType = Type.GetTypeFromProgID("Shell.Application");
            if (shellType is null)
            {
                return null;
            }

            shellObject = Activator.CreateInstance(shellType);
            if (shellObject is null)
            {
                return null;
            }

            dynamic shell = shellObject;
            windowsObject = shell.Windows();
            if (windowsObject is null)
            {
                return null;
            }

            dynamic windows = windowsObject;
            var windowCount = Convert.ToInt32((object)windows.Count, CultureInfo.InvariantCulture);
            for (var index = 0; index < windowCount; index++)
            {
                object? browserObject = null;
                try
                {
                    browserObject = windows.Item(index);
                    if (browserObject is null)
                    {
                        continue;
                    }

                    if (browserObject is not IWebBrowserApp browser)
                    {
                        continue;
                    }

                    if (browser.Hwnd != targetWindow.ToInt64())
                    {
                        continue;
                    }

                    dynamic document = browser.Document;
                    dynamic folder = document.Folder;
                    dynamic self = folder.Self;
                    var path = Convert.ToString((object)self.Path, CultureInfo.InvariantCulture);
                    if (!string.IsNullOrWhiteSpace(path) && Directory.Exists(path))
                    {
                        return path;
                    }
                }
                catch (Exception exception) when (exception is COMException
                    or InvalidOperationException
                    or ArgumentException
                    or Win32Exception
                    or RuntimeBinderException
                    or MissingMemberException)
                {
                    Debug.WriteLine($"资源管理器窗口条目读取失败：{exception}");
                }
                finally
                {
                    ReleaseComObject(browserObject);
                }
            }
        }
        catch (Exception exception) when (exception is COMException
            or InvalidOperationException
            or ArgumentException
            or Win32Exception
            or RuntimeBinderException)
        {
            Debug.WriteLine($"资源管理器路径读取失败：{exception}");
        }
        finally
        {
            ReleaseComObject(windowsObject);
            ReleaseComObject(shellObject);
        }

        return null;
    }

    private static bool IsExplorerWindow(IntPtr targetWindow)
    {
        if (!IsWindow(targetWindow))
        {
            return false;
        }

        var threadId = GetWindowThreadProcessId(targetWindow, out var processId);
        if (threadId == 0 || processId == 0)
        {
            return false;
        }

        try
        {
            using var process = Process.GetProcessById((int)processId);
            return string.Equals(process.ProcessName, "explorer", StringComparison.OrdinalIgnoreCase);
        }
        catch (ArgumentException)
        {
            return false;
        }
        catch (InvalidOperationException)
        {
            return false;
        }
    }

    private static string CreateUniqueFilePath(string folderPath, string extension)
    {
        var baseName = $"PasteOrbit_{DateTime.Now:yyyyMMdd_HHmmss}";
        var filePath = Path.Combine(folderPath, baseName + extension);
        var suffix = 1;
        while (File.Exists(filePath))
        {
            filePath = Path.Combine(folderPath, $"{baseName} ({suffix++}){extension}");
        }

        return filePath;
    }

    private static async Task<byte[]> ConvertImageToPngAsync(byte[] content)
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

    private static void ReleaseComObject(object? value)
    {
        if (value is not null && Marshal.IsComObject(value))
        {
            Marshal.FinalReleaseComObject(value);
        }
    }

    [ComImport]
    [Guid("0002DF05-0000-0000-C000-000000000046")]
    [InterfaceType(ComInterfaceType.InterfaceIsIDispatch)]
    private interface IWebBrowserApp
    {
        [DispId(203)]
        object Document { get; }

        [DispId(-515)]
        long Hwnd { get; }
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern void SHChangeNotify(
        int eventId,
        uint flags,
        [MarshalAs(UnmanagedType.LPWStr)] string path,
        IntPtr item2);
}
