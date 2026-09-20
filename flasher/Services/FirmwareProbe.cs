using System.Diagnostics;
using System.IO.Ports;
using System.Text;
using System.Text.RegularExpressions;

namespace TamaPoke.Flasher.Services;

public sealed partial class FirmwareProbe
{
    public async Task<FirmwareSaveInfo?> ProbeAsync(
        string portName,
        Action<string> log,
        CancellationToken cancellationToken)
    {
        try
        {
            using var port = new SerialPort(portName, 115200)
            {
                DtrEnable = false,
                RtsEnable = false,
                ReadTimeout = 300,
                WriteTimeout = 1000,
                NewLine = "\n",
                Encoding = Encoding.UTF8,
            };
            port.Open();
            // USB CDC can still be settling immediately after wake/reconnect.
            // Give it time, then retry instead of treating one missed command
            // as proof that no Korean save exists.
            await Task.Delay(500, cancellationToken);
            port.DiscardInBuffer();
            port.DiscardOutBuffer();

            var command = Encoding.ASCII.GetBytes("\nSAVEINFO\n");
            var response = new StringBuilder();
            var buffer = new byte[512];
            const int maxAttempts = 5;
            for (var attempt = 1; attempt <= maxAttempts && !SaveInfoRegex().IsMatch(response.ToString()); attempt++)
            {
                log($"저장 데이터 확인 시도 {attempt}/{maxAttempts}");
                cancellationToken.ThrowIfCancellationRequested();
                port.Write(command, 0, command.Length);
                var attemptTimer = Stopwatch.StartNew();
                while (attemptTimer.Elapsed < TimeSpan.FromSeconds(2))
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    var available = port.BytesToRead;
                    if (available > 0)
                    {
                        var read = port.Read(buffer, 0, Math.Min(buffer.Length, available));
                        if (read > 0)
                        {
                            response.Append(Encoding.UTF8.GetString(buffer, 0, read));
                            if (response.ToString().Contains("DONE", StringComparison.Ordinal) ||
                                SaveInfoRegex().IsMatch(response.ToString()))
                                break;
                        }
                    }
                    await Task.Delay(50, cancellationToken);
                }
                if (!SaveInfoRegex().IsMatch(response.ToString()) && attempt < maxAttempts)
                    await Task.Delay(300, cancellationToken);
            }

            var raw = response.ToString();
            var match = SaveInfoRegex().Match(raw);
            if (!match.Success)
            {
                log("실행 중인 한글판 응답을 확인하지 못했습니다.");
                return null;
            }

            var version = match.Groups["version"].Value;
            var hasSave = match.Groups["save"].Value.Equals("loaded", StringComparison.OrdinalIgnoreCase);
            var isKorean = version.Contains("-ko", StringComparison.OrdinalIgnoreCase);
            log($"실행 펌웨어 확인: {version} / 저장 데이터 {(hasSave ? "있음" : "새로 생성됨")}");
            return new FirmwareSaveInfo(version, isKorean, hasSave, raw);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex) when (ex is IOException or InvalidOperationException or
                                   UnauthorizedAccessException or TimeoutException)
        {
            log($"실행 펌웨어 확인 생략: {ex.Message}");
            return null;
        }
    }

    public static FirmwareSaveInfo? ParseSaveInfo(string response)
    {
        var match = SaveInfoRegex().Match(response);
        if (!match.Success) return null;
        var version = match.Groups["version"].Value;
        return new FirmwareSaveInfo(
            version,
            version.Contains("-ko", StringComparison.OrdinalIgnoreCase),
            match.Groups["save"].Value.Equals("loaded", StringComparison.OrdinalIgnoreCase),
            response);
    }

    [GeneratedRegex(@"fw=(?<version>\S+)\s+save=(?<save>loaded|created)", RegexOptions.IgnoreCase)]
    private static partial Regex SaveInfoRegex();

}
