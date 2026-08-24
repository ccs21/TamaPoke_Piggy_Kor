using System.IO.Compression;
using System.Net.Http;
using System.Reflection;
using System.Security.Cryptography;

namespace TamaPoke.Flasher.Services;

public sealed class PayloadManager
{
    private const string ResourcePrefix = "TamaPoke.Payload.";
    private const string EsptoolUrl = "https://github.com/espressif/esptool/releases/download/v5.3.1/esptool-v5.3.1-windows-amd64.zip";
    private const string EsptoolSha256 = "2b4a73c45db27426685896f64ce3e557f63a64f43cc100cb65c0cc3486af96d3";
    private const string MkLittleFsUrl = "https://github.com/earlephilhower/mklittlefs/releases/download/4.0.2/x86_64-w64-mingw32-mklittlefs-db0513a.zip";
    private const string MkLittleFsSha256 = "e99dbfcf2b808a2020254764f06e336aa6a4d253ab09bcabe01399fcd95d9ab8";
    private static readonly HttpClient Http = CreateClient();
    private readonly string _payloadDirectory = AppStoragePaths.Under(
        "Payload", "3.0.0");

    public async Task<PreparedPayload> PrepareToolsAsync(CancellationToken cancellationToken)
    {
        Directory.CreateDirectory(_payloadDirectory);
        var esptool = await PrepareDownloadedToolAsync(
            EsptoolUrl, EsptoolSha256, "esptool-v5.3.1.zip", "esptool.exe", "esptool", cancellationToken);
        var mklittlefs = await PrepareDownloadedToolAsync(
            MkLittleFsUrl, MkLittleFsSha256, "mklittlefs-4.0.2.zip", "mklittlefs.exe", "mklittlefs", cancellationToken,
            ["libwinpthread-1.dll"]);

        var boards = new Dictionary<string, BoardBasePayload>(StringComparer.OrdinalIgnoreCase);
        foreach (var board in BoardCatalog.All)
        {
            var stem = board.ResourceStem;
            boards[board.Id] = new(
                await ExtractAsync($"{stem}-app.bin", $"{stem}-app.bin", cancellationToken),
                await ExtractAsync($"{stem}-bootloader.bin", $"{stem}-bootloader.bin", cancellationToken),
                await ExtractAsync($"{stem}-partitions.bin", $"{stem}-partitions.bin", cancellationToken),
                await ExtractAsync($"{stem}-boot_app0.bin", $"{stem}-boot_app0.bin", cancellationToken));
        }
        return new(esptool, mklittlefs, boards);
    }

    private async Task<string> PrepareDownloadedToolAsync(
        string url, string expectedHash, string archiveName, string executableName,
        string directoryName, CancellationToken token, IReadOnlyCollection<string>? companions = null)
    {
        var downloads = Path.Combine(_payloadDirectory, "downloads");
        Directory.CreateDirectory(downloads);
        var archivePath = Path.Combine(downloads, archiveName);
        if (!await HasExpectedHashAsync(archivePath, expectedHash, token))
        {
            var temporary = archivePath + ".part";
            using var response = await Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, token);
            response.EnsureSuccessStatusCode();
            await using (var input = await response.Content.ReadAsStreamAsync(token))
            await using (var output = new FileStream(temporary, FileMode.Create, FileAccess.Write,
                             FileShare.None, 1024 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan))
                await input.CopyToAsync(output, 1024 * 1024, token);
            if (!await HasExpectedHashAsync(temporary, expectedHash, token))
                throw new InvalidDataException($"공식 도구의 SHA-256 검증에 실패했습니다: {archiveName}");
            File.Move(temporary, archivePath, true);
        }

        var toolDirectory = Path.Combine(_payloadDirectory, directoryName);
        Directory.CreateDirectory(toolDirectory);
        using var archive = ZipFile.OpenRead(archivePath);
        var wanted = new[] { executableName }.Concat(companions ?? []).ToArray();
        foreach (var name in wanted)
        {
            var matches = archive.Entries.Where(entry =>
                entry.Name.Equals(name, StringComparison.OrdinalIgnoreCase)).ToArray();
            if (matches.Length != 1)
                throw new InvalidDataException($"공식 도구 ZIP에서 {name} 파일을 하나만 찾을 수 없습니다.");
            var destination = Path.Combine(toolDirectory, name);
            var temporary = destination + ".new";
            await using (var input = matches[0].Open())
            await using (var output = new FileStream(temporary, FileMode.Create, FileAccess.Write,
                             FileShare.None, 1024 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan))
                await input.CopyToAsync(output, 1024 * 1024, token);
            File.Move(temporary, destination, true);
        }
        return Path.Combine(toolDirectory, executableName);
    }

    private static async Task<bool> HasExpectedHashAsync(string path, string expectedHash, CancellationToken token)
    {
        if (!File.Exists(path)) return false;
        await using var stream = File.OpenRead(path);
        var actual = Convert.ToHexString(await SHA256.HashDataAsync(stream, token)).ToLowerInvariant();
        return actual.Equals(expectedHash, StringComparison.OrdinalIgnoreCase);
    }

    private async Task<string> ExtractAsync(string resourceFileName, string relativePath, CancellationToken token)
    {
        var resourceName = ResourcePrefix + resourceFileName;
        var assembly = Assembly.GetExecutingAssembly();
        await using var resource = assembly.GetManifestResourceStream(resourceName)
                                   ?? throw new InvalidDataException($"내장 설치 파일을 찾을 수 없습니다: {resourceFileName}");
        var destination = Path.Combine(_payloadDirectory, relativePath);
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        if (File.Exists(destination) && new FileInfo(destination).Length == resource.Length)
        {
            var resourceHash = await SHA256.HashDataAsync(resource, token);
            await using var current = File.OpenRead(destination);
            var currentHash = await SHA256.HashDataAsync(current, token);
            if (CryptographicOperations.FixedTimeEquals(resourceHash, currentHash)) return destination;
            resource.Position = 0;
        }
        var temporary = destination + ".new";
        await using (var output = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None,
                         1024 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan))
            await resource.CopyToAsync(output, 1024 * 1024, token);
        File.Move(temporary, destination, true);
        return destination;
    }

    private static HttpClient CreateClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd(
            "TamaPoke-Korean-Flasher/2.1 (+https://github.com/ccs21/TamaPoke_Piggy_Kor)");
        return client;
    }
}
