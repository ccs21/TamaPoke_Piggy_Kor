using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;

namespace TamaPoke.Flasher.Services;

public sealed class FirmwareBuildService(PreparedPayload payload)
{
    private const string BuildVersion = "public-firmware-image-v3";
    private readonly string _cacheRoot = AppStoragePaths.Under(
        "Cache", "firmware", "3.0.0");

    public async Task<string> BuildAsync(
        BoardDefinition board, SpriteBuildResult sprites, AdditionalAssetBuildResult additional,
        Action<string> log, IProgress<ProgressUpdate>? progress, CancellationToken token)
    {
        var keyText = $"{BuildVersion}|{board.Id}|{sprites.ManifestHash}|{additional.InputHash}";
        var key = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(keyText))).ToLowerInvariant()[..24];
        var buildDirectory = Path.Combine(_cacheRoot, board.Id, key);
        var fullImage = Path.Combine(buildDirectory, $"tamapoke-{board.Id}-custom-full.bin");
        if (File.Exists(fullImage) && new FileInfo(fullImage).Length > board.FileSystemOffset)
        {
            log($"완성 펌웨어 캐시 재사용: {fullImage}");
            progress?.Report(new ProgressUpdate(68, "맞춤 펌웨어 준비 완료", "이전에 만든 동일한 펌웨어를 재사용합니다."));
            return fullImage;
        }

        Directory.CreateDirectory(buildDirectory);
        var dataDirectory = Path.Combine(buildDirectory, "data");
        var monsDirectory = Path.Combine(dataDirectory, "mons");
        Directory.CreateDirectory(monsDirectory);
        progress?.Report(new ProgressUpdate(50, "맞춤 펌웨어 구성", "다운로드한 스프라이트와 사용자 추가 자산을 배치합니다."));
        await CopyDirectoryFilesAsync(sprites.DataDirectory, monsDirectory, token);
        await CopyDirectoryTreeAsync(additional.DataDirectory, dataDirectory, token);

        var fileSystemImage = Path.Combine(buildDirectory, $"tamapoke-{board.Id}-littlefs.bin");
        progress?.Report(new ProgressUpdate(56, "파일시스템 생성", "스프라이트와 사용자 자산을 설치 이미지로 만들고 있습니다."));
        await RunAsync(payload.MkLittleFsPath,
            ["-c", dataDirectory, "-b", "4096", "-p", "256", "-s", board.FileSystemSize.ToString(), fileSystemImage],
            buildDirectory, log, token);
        if (!File.Exists(fileSystemImage) || new FileInfo(fileSystemImage).Length != board.FileSystemSize)
            throw new InvalidDataException("LittleFS 설치 이미지 크기가 올바르지 않습니다.");

        var basePayload = payload.For(board);
        progress?.Report(new ProgressUpdate(63, "펌웨어 병합", $"{board.DisplayName}용 프로그램과 사용자 데이터를 결합합니다."));
        await RunAsync(payload.EsptoolPath,
        [
            "--chip", "esp32s3", "merge-bin", "--flash-mode", "dio", "--flash-freq", "80m",
            "--flash-size", board.FlashSizeArgument, "-o", fullImage,
            "0x0", basePayload.BootloaderPath,
            "0x8000", basePayload.PartitionsPath,
            "0xe000", basePayload.BootAppPath,
            "0x10000", basePayload.AppPath,
            $"0x{board.FileSystemOffset:X}", fileSystemImage,
        ], buildDirectory, log, token);
        if (!File.Exists(fullImage) || new FileInfo(fullImage).Length <= board.FileSystemOffset)
            throw new InvalidDataException("맞춤 펌웨어 병합 결과가 올바르지 않습니다.");
        log($"맞춤 펌웨어 생성 완료: {fullImage}");
        progress?.Report(new ProgressUpdate(68, "맞춤 펌웨어 준비 완료", "기기에 기록할 이미지를 완성했습니다."));
        return fullImage;
    }

    private static async Task CopyDirectoryFilesAsync(string source, string destination, CancellationToken token)
    {
        foreach (var path in Directory.EnumerateFiles(source))
        {
            token.ThrowIfCancellationRequested();
            var target = Path.Combine(destination, Path.GetFileName(path));
            await using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 65536, true);
            await using var output = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None, 65536, true);
            await input.CopyToAsync(output, token);
        }
    }

    private static async Task CopyDirectoryTreeAsync(string source, string destination, CancellationToken token)
    {
        var sourceRoot = Path.GetFullPath(source).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        foreach (var path in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
        {
            token.ThrowIfCancellationRequested();
            var full = Path.GetFullPath(path);
            if (!full.StartsWith(sourceRoot, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("추가 자산 경로가 캐시 폴더를 벗어났습니다.");
            var relative = Path.GetRelativePath(source, full);
            var target = Path.Combine(destination, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            await using var input = new FileStream(full, FileMode.Open, FileAccess.Read, FileShare.Read, 65536, true);
            await using var output = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None, 65536, true);
            await input.CopyToAsync(output, token);
        }
    }

    private static async Task RunAsync(string executable, IReadOnlyList<string> arguments, string workingDirectory,
        Action<string> log, CancellationToken token)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = executable, WorkingDirectory = workingDirectory, UseShellExecute = false,
            CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
        };
        foreach (var argument in arguments) startInfo.ArgumentList.Add(argument);
        log($"> {Path.GetFileName(executable)} {arguments[0]} …");
        using var process = new Process { StartInfo = startInfo };
        if (!process.Start()) throw new InvalidOperationException($"{Path.GetFileName(executable)} 실행에 실패했습니다.");
        var stdout = PumpAsync(process.StandardOutput, log, token);
        var stderr = PumpAsync(process.StandardError, log, token);
        try { await process.WaitForExitAsync(token); await Task.WhenAll(stdout, stderr); }
        catch (OperationCanceledException) { if (!process.HasExited) process.Kill(true); throw; }
        if (process.ExitCode != 0)
            throw new InvalidOperationException($"{Path.GetFileName(executable)} 종료 코드: {process.ExitCode}");
    }

    private static async Task PumpAsync(StreamReader reader, Action<string> log, CancellationToken token)
    {
        while (await reader.ReadLineAsync(token) is { } line) if (line.Length > 0) log(line);
    }
}
