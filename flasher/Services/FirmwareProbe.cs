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
            };
            port.Open();
            await Task.Delay(180, cancellationToken);
            port.DiscardInBuffer();
            port.DiscardOutBuffer();

            var command = Encoding.ASCII.GetBytes("\nSAVEINFO\n");
            await port.BaseStream.WriteAsync(command, cancellationToken);
            await port.BaseStream.FlushAsync(cancellationToken);

            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(3));
            var response = new StringBuilder();
            var buffer = new byte[512];
            while (!timeout.IsCancellationRequested)
            {
                int read;
                try
                {
                    read = await port.BaseStream.ReadAsync(buffer, timeout.Token);
                }
                catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
                {
                    break;
                }
                if (read <= 0) break;
                response.Append(Encoding.UTF8.GetString(buffer, 0, read));
                if (response.ToString().Contains("DONE", StringComparison.Ordinal)) break;
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
        catch (Exception ex) when (ex is IOException or InvalidOperationException or UnauthorizedAccessException)
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
