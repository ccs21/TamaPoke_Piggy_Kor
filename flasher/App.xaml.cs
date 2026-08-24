using System.Diagnostics;
using System.Windows;
using TamaPoke.Flasher.Services;

namespace TamaPoke.Flasher;

public partial class App : Application
{
    private void Application_Startup(object sender, StartupEventArgs e)
    {
        if (e.Args.Contains("--self-test", StringComparer.OrdinalIgnoreCase))
        {
            var exitCode = RunSelfTest(e.Args);
            Shutdown(exitCode);
            return;
        }

        new MainWindow().Show();
    }

    private static int RunSelfTest(string[] args)
    {
        var logPath = GetArgument(args, "--self-test-log")
                      ?? Path.Combine(Path.GetTempPath(), "tamapoke-flasher-self-test.log");
        try
        {
            var payload = new PayloadManager();
            var files = Task.Run(() => payload.PrepareToolsAsync(CancellationToken.None))
                .GetAwaiter().GetResult();
            if (!File.Exists(files.EsptoolPath) || !File.Exists(files.MkLittleFsPath) ||
                BoardCatalog.All.Any(board => !File.Exists(files.For(board).AppPath) ||
                    !File.Exists(files.For(board).BootloaderPath) ||
                    !File.Exists(files.For(board).PartitionsPath) ||
                    !File.Exists(files.For(board).BootAppPath)))
                throw new InvalidOperationException("내장 파일 추출 검사에 실패했습니다.");

            if (EspToolService.ParseFlashBytes("Detected flash size: 16MB") != 16 * 1024 * 1024)
                throw new InvalidOperationException("16MB 판별 검사에 실패했습니다.");
            if (EspToolService.ParseFlashBytes("Detected flash size: 32MB") != 32 * 1024 * 1024)
                throw new InvalidOperationException("32MB 판별 검사에 실패했습니다.");
            if (BoardCatalog.FromFlashBytes(16 * 1024 * 1024)?.Id != "1.75" ||
                BoardCatalog.FromFlashBytes(32 * 1024 * 1024)?.Id != "1.75C")
                throw new InvalidOperationException("보드 매핑 검사에 실패했습니다.");
            var saveInfo = FirmwareProbe.ParseSaveInfo(
                "fw=1.46.0-ko-175 save=loaded createdBoot=0 spec=25 level=3 egg=0 starter=0 age=240\nDONE\n");
            if (saveInfo is not { IsKorean: true, HasExistingSave: true } ||
                saveInfo.Version != "1.46.0-ko-175")
                throw new InvalidOperationException("한글판 저장 데이터 감지 검사에 실패했습니다.");
            if (EspToolService.NvsOffset != 0x9000 || EspToolService.NvsSize != 0x5000)
                throw new InvalidOperationException("저장 데이터 파티션 검사에 실패했습니다.");

            var start = new ProcessStartInfo(files.EsptoolPath, "version")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            using var process = Process.Start(start)
                                ?? throw new InvalidOperationException("esptool 실행 검사에 실패했습니다.");
            process.WaitForExit(15000);
            if (!process.HasExited || process.ExitCode != 0)
                throw new InvalidOperationException("esptool 자체 검사에 실패했습니다.");
            var version = process.StandardOutput.ReadToEnd().Trim();
            File.WriteAllText(logPath, $"PASS{Environment.NewLine}{version}{Environment.NewLine}");
            return 0;
        }
        catch (Exception ex)
        {
            File.WriteAllText(logPath, $"FAIL{Environment.NewLine}{ex}{Environment.NewLine}");
            return 1;
        }
    }

    private static string? GetArgument(IReadOnlyList<string> args, string name)
    {
        for (var i = 0; i + 1 < args.Count; ++i)
            if (args[i].Equals(name, StringComparison.OrdinalIgnoreCase)) return args[i + 1];
        return null;
    }
}
