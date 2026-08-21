using System.Buffers;
using System.Security.Cryptography;
using System.Text;
using PasteOrbit.Core;

namespace PasteOrbit.App;

/// <summary>
/// 导出和恢复本机数据库、设置及其完整性校验信息。
/// </summary>
internal sealed class LocalBackupService(string databasePath, string settingsPath)
{
    private const int FormatVersion = 1;
    private const int AuthenticationTagLength = 32;
    private static readonly byte[] Magic = Encoding.ASCII.GetBytes("PasteOrbit.Backup");

    private string DatabasePath { get; } = Path.GetFullPath(databasePath);

    private string SettingsPath { get; } = Path.GetFullPath(settingsPath);

    public async Task ExportAsync(string destinationPath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(destinationPath);
        if (!File.Exists(DatabasePath))
        {
            throw new FileNotFoundException(AppLocalization.GetString("HistoryDatabaseNotFound"), DatabasePath);
        }

        var targetPath = Path.GetFullPath(destinationPath);
        var temporaryPath = $"{targetPath}.tmp";
        var encryptionKey = RandomNumberGenerator.GetBytes(32);
        var authenticationKey = RandomNumberGenerator.GetBytes(32);
        var protectedKeys = UserDataProtector.Protect([.. encryptionKey, .. authenticationKey]);

        try
        {
            // 备份内容使用随机 AES 密钥加密，并用独立 HMAC 校验完整性。
            Directory.CreateDirectory(Path.GetDirectoryName(targetPath)!);
            await using (var output = new FileStream(
                             temporaryPath,
                             FileMode.Create,
                             FileAccess.ReadWrite,
                             FileShare.None,
                             81920,
                             FileOptions.Asynchronous | FileOptions.SequentialScan))
            {
                using (var writer = new BinaryWriter(output, Encoding.UTF8, leaveOpen: true))
                {
                    writer.Write(Magic.Length);
                    writer.Write(Magic);
                    writer.Write(FormatVersion);
                    writer.Write(protectedKeys.Length);
                    writer.Write(protectedKeys);
                }

                using var aes = Aes.Create();
                aes.Key = encryptionKey;
                aes.GenerateIV();
                await output.WriteAsync(aes.IV);

                await using (var encryptionStream = new CryptoStream(
                                 output,
                                 aes.CreateEncryptor(),
                                 CryptoStreamMode.Write,
                                 leaveOpen: true))
                {
                    using var payloadWriter = new BinaryWriter(encryptionStream, Encoding.UTF8, leaveOpen: true);
                    var databaseLength = new FileInfo(DatabasePath).Length;
                    var settingsLength = File.Exists(SettingsPath) ? new FileInfo(SettingsPath).Length : 0;
                    payloadWriter.Write(databaseLength);
                    payloadWriter.Write(settingsLength);
                    payloadWriter.Flush();
                    await CopyFileToStreamAsync(DatabasePath, encryptionStream);
                    if (settingsLength > 0)
                    {
                        await CopyFileToStreamAsync(SettingsPath, encryptionStream);
                    }

                    await encryptionStream.FlushFinalBlockAsync();
                }

                await output.FlushAsync();
            }

            var authenticationTag = await ComputeAuthenticationTagAsync(
                temporaryPath,
                authenticationKey,
                new FileInfo(temporaryPath).Length);
            await using (var output = new FileStream(temporaryPath, FileMode.Append, FileAccess.Write, FileShare.None, 4096, true))
            {
                await output.WriteAsync(authenticationTag);
                await output.FlushAsync();
            }

            File.Move(temporaryPath, targetPath, true);
        }
        finally
        {
            CryptographicOperations.ZeroMemory(encryptionKey);
            CryptographicOperations.ZeroMemory(authenticationKey);
            TryDelete(temporaryPath);
        }
    }

    public async Task RestoreAsync(string sourcePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(sourcePath);
        var backupPath = Path.GetFullPath(sourcePath);
        var dataDirectory = Path.GetDirectoryName(DatabasePath)!;
        Directory.CreateDirectory(dataDirectory);
        var temporaryPayload = Path.Combine(dataDirectory, $"{Path.GetRandomFileName()}.payload");
        var temporaryDatabase = Path.Combine(dataDirectory, $"{Path.GetRandomFileName()}.db");
        var temporarySettings = Path.Combine(dataDirectory, $"{Path.GetRandomFileName()}.json");
        byte[]? keyMaterial = null;

        try
        {
            // 先验证文件头、密钥长度和 HMAC，再解密到临时文件。
            await using var input = new FileStream(backupPath, FileMode.Open, FileAccess.Read, FileShare.Read, 81920, true);
            using var reader = new BinaryReader(input, Encoding.UTF8, leaveOpen: true);
            var magicLength = reader.ReadInt32();
            if (magicLength != Magic.Length || !reader.ReadBytes(magicLength).AsSpan().SequenceEqual(Magic))
            {
                throw new InvalidDataException(AppLocalization.GetString("InvalidBackupFile"));
            }

            if (reader.ReadInt32() != FormatVersion)
            {
                throw new InvalidDataException(AppLocalization.GetString("UnsupportedBackupVersion"));
            }

            var protectedKeyLength = reader.ReadInt32();
            if (protectedKeyLength is <= 0 or > 4096)
            {
                throw new InvalidDataException(AppLocalization.GetString("InvalidBackupKeyInfo"));
            }

            keyMaterial = UserDataProtector.Unprotect(reader.ReadBytes(protectedKeyLength));
            if (keyMaterial.Length != 64)
            {
                throw new InvalidDataException(AppLocalization.GetString("InvalidBackupKeyLength"));
            }

            var initializationVector = reader.ReadBytes(16);
            if (initializationVector.Length != 16)
            {
                throw new InvalidDataException(AppLocalization.GetString("InvalidBackupInitializationVector"));
            }

            var payloadOffset = input.Position;
            var authenticatedLength = input.Length - AuthenticationTagLength;
            var encryptedLength = authenticatedLength - payloadOffset;
            if (encryptedLength <= 0)
            {
                throw new InvalidDataException(AppLocalization.GetString("BackupContentEmpty"));
            }

            input.Position = authenticatedLength;
            var storedTag = reader.ReadBytes(AuthenticationTagLength);
            var expectedTag = await ComputeAuthenticationTagAsync(backupPath, keyMaterial.AsSpan(32, 32).ToArray(), authenticatedLength);
            if (!CryptographicOperations.FixedTimeEquals(storedTag, expectedTag))
            {
                throw new CryptographicException(AppLocalization.GetString("BackupCorrupted"));
            }

            input.Position = payloadOffset;
            using var aes = Aes.Create();
            aes.Key = keyMaterial.AsSpan(0, 32).ToArray();
            aes.IV = initializationVector;
            await using (var boundedInput = new BoundedReadStream(input, encryptedLength))
            await using (var decryptionStream = new CryptoStream(boundedInput, aes.CreateDecryptor(), CryptoStreamMode.Read))
            await using (var payloadOutput = new FileStream(temporaryPayload, FileMode.CreateNew, FileAccess.Write, FileShare.None, 81920, true))
            {
                await decryptionStream.CopyToAsync(payloadOutput);
            }

            await ExtractPayloadAsync(temporaryPayload, temporaryDatabase, temporarySettings);
            if (!ClipboardRepository.IsCurrentSchema(temporaryDatabase))
            {
                throw new InvalidDataException(AppLocalization.GetString("UnsupportedBackupDatabaseVersion"));
            }

            ReplaceLocalData(temporaryDatabase, temporarySettings);
        }
        finally
        {
            if (keyMaterial is not null)
            {
                CryptographicOperations.ZeroMemory(keyMaterial);
            }

            TryDelete(temporaryPayload);
            TryDelete(temporaryDatabase);
            TryDelete(temporarySettings);
        }
    }

    private static async Task ExtractPayloadAsync(string payloadPath, string databasePath, string settingsPath)
    {
        await using var payload = new FileStream(payloadPath, FileMode.Open, FileAccess.Read, FileShare.Read, 81920, true);
        using var reader = new BinaryReader(payload, Encoding.UTF8, leaveOpen: true);
        var databaseLength = reader.ReadInt64();
        var settingsLength = reader.ReadInt64();
        var remainingLength = payload.Length - payload.Position;
        if (databaseLength <= 0 || settingsLength < 0 || databaseLength + settingsLength != remainingLength)
        {
            throw new InvalidDataException(AppLocalization.GetString("InvalidBackupContentLength"));
        }

        await using (var database = new FileStream(databasePath, FileMode.CreateNew, FileAccess.Write, FileShare.None, 81920, true))
        {
            await CopyExactlyAsync(payload, database, databaseLength);
        }

        if (settingsLength > 0)
        {
            await using var settings = new FileStream(settingsPath, FileMode.CreateNew, FileAccess.Write, FileShare.None, 81920, true);
            await CopyExactlyAsync(payload, settings, settingsLength);
        }
    }

    private void ReplaceLocalData(string temporaryDatabase, string temporarySettings)
    {
        // 替换前保留恢复前副本，替换失败时回滚数据库和设置文件。
        var stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
        var databaseRecovery = $"{DatabasePath}.before-restore-{stamp}";
        var settingsRecovery = $"{SettingsPath}.before-restore-{stamp}";
        var hadDatabase = File.Exists(DatabasePath);
        var hadSettings = File.Exists(SettingsPath);
        if (hadDatabase)
        {
            File.Copy(DatabasePath, databaseRecovery, true);
        }

        if (hadSettings)
        {
            File.Copy(SettingsPath, settingsRecovery, true);
        }

        try
        {
            File.Move(temporaryDatabase, DatabasePath, true);
            TryDelete($"{DatabasePath}-wal");
            TryDelete($"{DatabasePath}-shm");
            if (File.Exists(temporarySettings))
            {
                File.Move(temporarySettings, SettingsPath, true);
            }
        }
        catch
        {
            if (hadDatabase && File.Exists(databaseRecovery))
            {
                File.Copy(databaseRecovery, DatabasePath, true);
            }

            if (hadSettings && File.Exists(settingsRecovery))
            {
                File.Copy(settingsRecovery, SettingsPath, true);
            }

            throw;
        }
    }

    private static async Task CopyFileToStreamAsync(string path, Stream destination)
    {
        await using var source = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 81920, true);
        await source.CopyToAsync(destination);
    }

    private static async Task CopyExactlyAsync(Stream source, Stream destination, long count)
    {
        // 按声明长度复制，短读直接视为备份损坏。
        var buffer = ArrayPool<byte>.Shared.Rent(81920);
        try
        {
            while (count > 0)
            {
                var read = await source.ReadAsync(buffer.AsMemory(0, (int)Math.Min(buffer.Length, count)));
                if (read == 0)
                {
                    throw new EndOfStreamException(AppLocalization.GetString("BackupContentIncomplete"));
                }

                await destination.WriteAsync(buffer.AsMemory(0, read));
                count -= read;
            }
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(buffer);
        }
    }

    private static async Task<byte[]> ComputeAuthenticationTagAsync(string path, byte[] key, long count)
    {
        using var hmac = new HMACSHA256(key);
        var buffer = ArrayPool<byte>.Shared.Rent(81920);
        try
        {
            await using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 81920, true);
            while (count > 0)
            {
                var read = await input.ReadAsync(buffer.AsMemory(0, (int)Math.Min(buffer.Length, count)));
                if (read == 0)
                {
                    throw new EndOfStreamException(AppLocalization.GetString("BackupFileIncomplete"));
                }

                hmac.TransformBlock(buffer, 0, read, null, 0);
                count -= read;
            }

            hmac.TransformFinalBlock([], 0, 0);
            return hmac.Hash!;
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(buffer);
        }
    }

    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private sealed class BoundedReadStream(Stream source, long remaining) : Stream
    {
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => remaining;
        public override long Position { get => 0; set => throw new NotSupportedException(); }

        public override int Read(byte[] buffer, int offset, int count)
        {
            if (remaining <= 0)
            {
                return 0;
            }

            var read = source.Read(buffer, offset, (int)Math.Min(count, remaining));
            remaining -= read;
            return read;
        }

        public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
        {
            if (remaining <= 0)
            {
                return 0;
            }

            var read = await source.ReadAsync(buffer[..(int)Math.Min(buffer.Length, remaining)], cancellationToken);
            remaining -= read;
            return read;
        }

        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }
}
