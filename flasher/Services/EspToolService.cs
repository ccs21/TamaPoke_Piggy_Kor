using System.Diagnostics;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace TamaPoke.Flasher.Services;

public sealed partial class EspToolService(string esptoolPath)
{
    public const int NvsOffset = 0x9000;
    public const int NvsSize = 0x5000;

    public async Task<int> DetectFlashSizeAsync(
        string portName,
        Action<string> log,
        CancellationToken cancellationToken)
    {
        var result = await RunAsync(portName, 115200, "hard-reset", ["flash-id"], log,
            null, 0, 0, cancellationToken);
        var bytes = ParseFlashBytes(result);
        if (bytes is null)
            throw new InvalidOperationException("기기의 플래시 용량을 확인하지 못했습니다. 기종을 직접 선택해 주세요.");
        return bytes.Value;
    }

    public async Task<string> BackupNvsAsync(
        string portName,
        string destination,
        Action<string> log,
        IProgress<ProgressUpdate>? progress,
        CancellationToken cancellationToken)
    {
        var directory = Path.GetDirectoryName(destination)
                        ?? throw new InvalidOperationException("백업 폴더 경로가 올바르지 않습니다.");
        Directory.CreateDirectory(directory);
        if (File.Exists(destination)) File.Delete(destination);

        progress?.Report(new ProgressUpdate(7, "저장 데이터 백업", "기기의 저장 데이터를 PC로 읽고 있습니다."));
        await RunAsync(portName, 460800, "no-reset",
            ["read-flash", "--no-progress", $"0x{NvsOffset:X}", $"0x{NvsSize:X}", destination],
            log, null, 0, 0, cancellationToken);

        var file = new FileInfo(destination);
        if (!file.Exists || file.Length != NvsSize)
            throw new InvalidDataException($"저장 데이터 백업 크기가 올바르지 않습니다: {file.Length}바이트");
        var bytes = await File.ReadAllBytesAsync(destination, cancellationToken);
        if (bytes.All(value => value == 0xFF))
            throw new InvalidDataException("기기에서 유효한 저장 데이터를 읽지 못했습니다.");

        var hash = Convert.ToHexString(SHA256.HashData(bytes));
        log($"저장 데이터 백업 완료: {destination}");
        log($"저장 데이터 SHA-256: {hash}");
        progress?.Report(new ProgressUpdate(12, "저장 데이터 백업 완료", "백업 파일을 검증했습니다."));
        return hash;
    }

    public async Task FlashAsync(
        string portName,
        BoardDefinition board,
        string firmwarePath,
        Action<string> log,
        IProgress<ProgressUpdate>? progress,
        double progressStart,
        double progressEnd,
        CancellationToken cancellationToken)
    {
        var length = new FileInfo(firmwarePath).Length;
        if (length <= 0 || length > board.FlashBytes)
            throw new InvalidDataException($"{board.DisplayName} 펌웨어 크기가 올바르지 않습니다.");

        progress?.Report(new ProgressUpdate(progressStart, "펌웨어 설치", $"{board.DisplayName} 전체 이미지를 기록하고 있습니다."));
        await RunAsync(portName, 460800, "hard-reset",
            [
                "write-flash",
                // ESP32-S3 ROM은 부트로더를 DIO로 읽은 뒤 부트로더가 QIO를 활성화한다.
                "--flash-mode", "dio",
                "--flash-freq", "80m",
                "--flash-size", board.FlashSizeArgument,
                "0x0", firmwarePath,
            ], log, progress, progressStart, progressEnd - progressStart, cancellationToken);
        progress?.Report(new ProgressUpdate(progressEnd, "펌웨어 설치 완료", "기록한 펌웨어를 검증했습니다."));
    }

    public async Task EraseFlashAsync(
        string portName,
        Action<string> log,
        IProgress<ProgressUpdate>? progress,
        CancellationToken cancellationToken)
    {
        progress?.Report(new ProgressUpdate(69, "기기 초기화", "새 펌웨어를 설치하기 전에 내부 플래시를 초기화하고 있습니다."));
        await RunAsync(portName, 460800, "no-reset", ["erase-flash"], log,
            null, 0, 0, cancellationToken);
        log("기기 전체 플래시 초기화 완료");
    }

    public async Task RestoreNvsAsync(
        string portName,
        string backupPath,
        string expectedHash,
        Action<string> log,
        IProgress<ProgressUpdate>? progress,
        CancellationToken cancellationToken)
    {
        var bytes = await File.ReadAllBytesAsync(backupPath, cancellationToken);
        if (bytes.Length != NvsSize)
            throw new InvalidDataException("복원할 저장 데이터 백업의 크기가 올바르지 않습니다.");
        var currentHash = Convert.ToHexString(SHA256.HashData(bytes));
        if (!currentHash.Equals(expectedHash, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("PC에 보관한 저장 데이터 백업의 무결성 검증에 실패했습니다.");

        progress?.Report(new ProgressUpdate(91, "저장 데이터 복원", "백업한 포켓몬과 게임 기록을 기기에 복원하고 있습니다."));
        await RunAsync(portName, 460800, "no-reset",
            [
                "write-flash", "--flash-mode", "dio", "--flash-freq", "80m",
                $"0x{NvsOffset:X}", backupPath,
            ], log, progress, 91, 5, cancellationToken);

        progress?.Report(new ProgressUpdate(97, "저장 데이터 검증", "복원된 저장 데이터가 백업과 같은지 확인하고 있습니다."));
        await RunAsync(portName, 460800, "hard-reset",
            ["verify-flash", $"0x{NvsOffset:X}", backupPath], log, null, 0, 0, cancellationToken);
        log("저장 데이터 복원 및 플래시 검증 완료");
        progress?.Report(new ProgressUpdate(99, "재시작", "저장 데이터를 복원하고 기기를 자동으로 재시작했습니다."));
    }

    public static int? ParseFlashBytes(string output)
    {
        var match = FlashSizeRegex().Match(output);
        if (!match.Success || !int.TryParse(match.Groups[1].Value, out var value)) return null;
        var unit = match.Groups[2].Value.ToUpperInvariant();
        return unit switch
        {
            "MB" => checked(value * 1024 * 1024),
            "KB" => checked(value * 1024),
            _ => value,
        };
    }

    private async Task<string> RunAsync(
        string portName,
        int baud,
        string after,
        IReadOnlyList<string> command,
        Action<string> log,
        IProgress<ProgressUpdate>? progress,
        double progressStart,
        double progressSpan,
        CancellationToken cancellationToken)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = esptoolPath,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(esptoolPath)!,
        };
        foreach (var arg in new[]
                 {
                     "--chip", "esp32s3", "--port", portName, "--baud", baud.ToString(CultureInfo.InvariantCulture),
                     "--before", "default-reset", "--after", after,
                 })
            startInfo.ArgumentList.Add(arg);
        foreach (var arg in command) startInfo.ArgumentList.Add(arg);

        log($"> esptool --chip esp32s3 --port {portName} … {command[0]}");
        using var process = new Process { StartInfo = startInfo, EnableRaisingEvents = true };
        if (!process.Start()) throw new InvalidOperationException("플래싱 엔진을 실행하지 못했습니다.");

        var combined = new StringBuilder();
        try
        {
            var stdout = PumpAsync(process.StandardOutput, combined, log, progress,
                progressStart, progressSpan, cancellationToken);
            var stderr = PumpAsync(process.StandardError, combined, log, progress,
                progressStart, progressSpan, cancellationToken);
            await process.WaitForExitAsync(cancellationToken);
            await Task.WhenAll(stdout, stderr);
        }
        catch (OperationCanceledException)
        {
            if (!process.HasExited) process.Kill(entireProcessTree: true);
            throw;
        }

        if (process.ExitCode != 0)
            throw new InvalidOperationException($"기기와 통신하지 못했습니다. esptool 종료 코드: {process.ExitCode}");
        return combined.ToString();
    }

    private static async Task PumpAsync(
        StreamReader reader,
        StringBuilder combined,
        Action<string> log,
        IProgress<ProgressUpdate>? progress,
        double progressStart,
        double progressSpan,
        CancellationToken cancellationToken)
    {
        while (await reader.ReadLineAsync(cancellationToken) is { } line)
        {
            if (line.Length == 0) continue;
            lock (combined) combined.AppendLine(line);
            log(line);
            var match = PercentRegex().Match(line);
            if (!match.Success || progress is null ||
                !double.TryParse(match.Groups[1].Value, NumberStyles.Float,
                    CultureInfo.InvariantCulture, out var percent)) continue;
            var mapped = progressStart + Math.Clamp(percent, 0, 100) / 100.0 * progressSpan;
            progress.Report(new ProgressUpdate(mapped, "펌웨어 설치", line));
        }
    }

    [GeneratedRegex(@"(?im)(?:Detected\s+flash\s+size|Flash\s+size)\s*:\s*(\d+)\s*(MB|KB|B)")]
    private static partial Regex FlashSizeRegex();

    [GeneratedRegex(@"(\d+(?:\.\d+)?)\s*%")]
    private static partial Regex PercentRegex();
}
