using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using TamaPoke.Flasher.Services;

namespace TamaPoke.Flasher;

public partial class MainWindow : Window
{
    private readonly DeviceDetector _deviceDetector = new();
    private readonly FirmwareProbe _firmwareProbe = new();
    private readonly ObservableCollection<SerialDeviceInfo> _devices = [];
    private readonly PayloadManager _payloadManager = new();
    private readonly AdditionalAssetsService _additionalAssets = new();
    private readonly SpritePipelineService _spritePipeline = new();
    private readonly string _logFilePath = CreateLogFilePath();
    private CancellationTokenSource? _operationCancellation;
    private bool _busy;

    public MainWindow()
    {
        InitializeComponent();
        DeviceComboBox.ItemsSource = _devices;
        BoardComboBox.ItemsSource = new BoardChoice[]
        {
            new("자동 선택 (권장)", null),
            new("1.75 (16MB)", BoardCatalog.Board175),
            new("1.75C (32MB)", BoardCatalog.Board175C),
        };
        BoardComboBox.SelectedIndex = 0;
    }

    private async void Window_Loaded(object sender, RoutedEventArgs e)
    {
        AppendLog("타마포케 배포용 플래셔 2.2.0 시작");
        AppendLog($"로그 파일: {_logFilePath}");
        RefreshAssetStatus();
        await RefreshDevicesAsync();
    }

    private void RefreshAssetsButton_Click(object sender, RoutedEventArgs e) => RefreshAssetStatus();

    private AdditionalAssetStatus RefreshAssetStatus()
    {
        var status = _additionalAssets.Scan();
        AssetStatusText.Text = status.CompactText;
        AssetStatusText.Foreground = !status.IsValid
            ? new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(255, 138, 125))
            : new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(216, 232, 246));
        return status;
    }

    private async Task RefreshDevicesAsync(string? selectPort = null)
    {
        await Task.Yield();
        var found = _deviceDetector.FindAll();
        _devices.Clear();
        foreach (var device in found) _devices.Add(device);

        SerialDeviceInfo? selection = null;
        if (!string.IsNullOrWhiteSpace(selectPort))
            selection = _devices.FirstOrDefault(device =>
                device.PortName.Equals(selectPort, StringComparison.OrdinalIgnoreCase));
        selection ??= _devices.FirstOrDefault(device => device.IsEspressif) ??
                     (_devices.Count == 1 ? _devices[0] : null);
        DeviceComboBox.SelectedItem = selection;
        ConnectionStatusText.Text = selection is null
            ? "기기를 찾지 못했습니다. USB 데이터 케이블과 포트를 확인한 뒤 다시 찾아 주세요."
            : selection.IsEspressif
                ? $"ESP32-S3로 보이는 {selection.PortName} 포트를 자동 선택했습니다."
                : $"{selection.PortName} 포트를 선택했습니다. 타마포케가 맞는지 확인하세요.";
    }

    private async void RefreshButton_Click(object sender, RoutedEventArgs e) =>
        await RefreshDevicesAsync((DeviceComboBox.SelectedItem as SerialDeviceInfo)?.PortName);

    private void DeviceComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (DeviceComboBox.SelectedItem is not SerialDeviceInfo device) return;
        ConnectionStatusText.Text = device.IsEspressif
            ? $"ESP32-S3로 보이는 {device.PortName} 포트를 선택했습니다."
            : $"{device.PortName} 포트를 선택했습니다. 타마포케가 맞는지 확인하세요.";
    }

    private async void StartButton_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        if (DeviceComboBox.SelectedItem is not SerialDeviceInfo selected)
        {
            MessageBox.Show(this, "먼저 타마포케의 COM 포트를 선택해 주세요.", "기기 선택",
                MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (!selected.IsEspressif)
        {
            var answer = MessageBox.Show(this,
                $"{selected.PortName} 포트를 ESP32-S3로 자동 확인하지 못했습니다.\n\n이 포트가 타마포케가 맞습니까?",
                "COM 포트 확인", MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No);
            if (answer != MessageBoxResult.Yes) return;
        }

        var assetStatus = RefreshAssetStatus();
        if (!assetStatus.IsValid)
        {
            var details = string.Join("\n", assetStatus.Errors.Select(error => $"• {error}"));
            MessageBox.Show(this,
                $"플래셔 실행 파일과 같은 폴더의 Additional_assets.zip과 " +
                $"sample_Additional_assets.zip을 확인해 주세요.\n\n{details}",
                "추가 자산 확인",
                MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        var allowSampleSupplement = false;
        if (assetStatus.RequiresSampleSupplement)
        {
            var missing = string.Join("\n", assetStatus.MissingFiles.Select(file => $"• {file}"));
            var supplementAnswer = MessageBox.Show(this,
                "누락된 파일이 있습니다.\n\n" +
                "아니오를 눌러 직접 누락된 파일을 보충하거나, 예를 눌러\n" +
                "sample_Additional_assets.zip에서 누락된 파일을 자동으로 보충합니다.\n\n" +
                $"누락된 파일:\n{missing}\n\n" +
                "계속 진행하시겠습니까?",
                "누락된 추가 자산",
                MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No);
            if (supplementAnswer != MessageBoxResult.Yes) return;
            allowSampleSupplement = true;
        }

        SetBusy(true);
        _operationCancellation = new CancellationTokenSource();
        var cancellationToken = _operationCancellation.Token;
        var progress = new Progress<ProgressUpdate>(UpdateProgress);
        string? backupPath = null;
        try
        {
            InstallProgress.Value = 1;
            PercentText.Text = "1%";
            StageText.Text = "설치 엔진 준비";
            DetailText.Text = "빌드 및 플래싱 도구의 무결성을 확인하고 있습니다.";
            AppendLog("────────────────────────────────");
            AppendLog($"선택 포트: {selected.PortName}");
            var payload = await _payloadManager.PrepareToolsAsync(cancellationToken);
            var esptool = new EspToolService(payload.EsptoolPath);

            UpdateProgress(new ProgressUpdate(2, "기존 설치 확인", "실행 중인 타마포케 한글판과 저장 데이터를 확인하고 있습니다."));
            var installed = await _firmwareProbe.ProbeAsync(selected.PortName, AppendLog, cancellationToken);

            UpdateProgress(new ProgressUpdate(4, "기종 자동 확인", "기기의 플래시 용량을 읽고 있습니다."));
            BoardDefinition? detectedBoard = null;
            Exception? detectionFailure = null;
            try
            {
                var flashBytes = await esptool.DetectFlashSizeAsync(selected.PortName, AppendLog, cancellationToken);
                detectedBoard = BoardCatalog.FromFlashBytes(flashBytes);
                AppendLog($"감지된 플래시: {flashBytes / 1048576}MB");
                if (detectedBoard is null)
                    throw new InvalidOperationException($"지원하지 않는 플래시 용량입니다: {flashBytes / 1048576}MB");
            }
            catch (OperationCanceledException) { throw; }
            catch (Exception ex)
            {
                detectionFailure = ex;
                AppendLog($"자동 판별 실패: {ex.Message}");
            }

            var choice = BoardComboBox.SelectedItem as BoardChoice;
            BoardDefinition board;
            if (choice?.Board is null)
            {
                board = detectedBoard ?? throw new InvalidOperationException(
                    "기종을 자동 판별하지 못했습니다. 1.75 또는 1.75C를 직접 선택한 뒤 다시 시도하세요.",
                    detectionFailure);
                BoardComboBox.SelectedItem = (BoardComboBox.ItemsSource as IEnumerable<BoardChoice>)?
                    .First(item => item.Board?.Id == board.Id);
            }
            else
            {
                board = choice.Board;
                if (detectedBoard is not null && detectedBoard.Id != board.Id)
                    throw new InvalidOperationException(
                        $"선택한 기종은 {board.DisplayName}이지만 실제 플래시는 {detectedBoard.DisplayName}로 확인되었습니다. " +
                        "잘못된 펌웨어 설치를 막기 위해 중단했습니다.");
                if (detectedBoard is null)
                {
                    var manualAnswer = MessageBox.Show(this,
                        $"기종을 자동 확인하지 못했습니다.\n\n선택한 {board.DisplayName}용 펌웨어를 설치하시겠습니까?",
                        "수동 기종 확인", MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No);
                    if (manualAnswer != MessageBoxResult.Yes) return;
                }
            }

            var saveChoice = SaveDataChoice.Delete;
            if (installed?.IsKorean == true)
            {
                var dialog = new SaveDataDialog(installed.Version) { Owner = this };
                dialog.ShowDialog();
                saveChoice = dialog.Choice;
                if (saveChoice == SaveDataChoice.Cancel) return;
            }
            else
            {
                var finalAnswer = MessageBox.Show(this,
                    $"연결된 기기를 {board.DisplayName}로 확인했습니다.\n\n" +
                    "기존 타마포케 한글판 저장 데이터를 발견하지 못했습니다.\n" +
                    "설치하면 기기의 기존 데이터가 모두 삭제됩니다.\n" +
                    "설치 중에는 USB 케이블을 분리하지 마세요.\n\n계속하시겠습니까?",
                    "새로 설치 확인", MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No);
                if (finalAnswer != MessageBoxResult.Yes) return;
            }

            AppendLog($"설치 기종: {board.DisplayName}");
            AppendLog($"저장 데이터 처리: {(saveChoice == SaveDataChoice.Keep ? "백업 후 복원" : "삭제")}");

            string? backupHash = null;
            if (saveChoice == SaveDataChoice.Keep)
            {
                backupPath = CreateBackupFilePath(board, selected.PortName);
                backupHash = await esptool.BackupNvsAsync(selected.PortName, backupPath, AppendLog,
                    progress, cancellationToken);
            }

            var additional = await _additionalAssets.PrepareAsync(assetStatus, allowSampleSupplement,
                AppendLog, progress, cancellationToken);
            var sprites = await _spritePipeline.PrepareAsync(AppendLog, progress, cancellationToken);
            var imageBuilder = new FirmwareBuildService(payload);
            var firmwarePath = await imageBuilder.BuildAsync(board, sprites, additional,
                AppendLog, progress, cancellationToken);

            await esptool.EraseFlashAsync(selected.PortName, AppendLog, progress, cancellationToken);
            await esptool.FlashAsync(selected.PortName, board, firmwarePath, AppendLog,
                progress, 70,
                saveChoice == SaveDataChoice.Keep ? 89 : 99, cancellationToken);

            if (saveChoice == SaveDataChoice.Keep)
                await esptool.RestoreNvsAsync(selected.PortName, backupPath!, backupHash!, AppendLog,
                    progress, cancellationToken);

            AppendLog("펌웨어와 스프라이트 설치 완료");
            UpdateProgress(new ProgressUpdate(100, "설치 완료", (saveChoice == SaveDataChoice.Keep
                ? "새 펌웨어 설치와 저장 데이터 복원이 완료되었습니다."
                : "새 펌웨어 설치가 완료되었습니다.") + " 필요하면 기기의 시간 보정 안내를 따라 주세요."));
            const string timeNotice = "\n\n첫 기동에 시간을 설정합니다. 1.75는 RTC 시간 손실 시, 1.75C는 실제 재부팅 뒤 저장된 Wi-Fi 또는 수동 입력으로 시간을 보정합니다.";
            MessageBox.Show(this,
                saveChoice == SaveDataChoice.Keep
                    ? $"{board.DisplayName}용 타마포케 한글판 설치와 저장 데이터 복원이 완료되었습니다.\n\n" +
                      $"PC 백업 파일도 보관했습니다.\n{backupPath}" + timeNotice + "\n\n기기는 자동으로 재시작됩니다."
                    : $"{board.DisplayName}용 타마포케 한글판 설치가 완료되었습니다." + timeNotice +
                      "\n\n기기는 자동으로 재시작됩니다.",
                "설치 완료", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (OperationCanceledException)
        {
            StageText.Text = "설치 취소됨";
            DetailText.Text = backupPath is null
                ? "작업을 취소했습니다. 기기를 다시 연결하고 설치를 처음부터 실행해 주세요."
                : $"작업을 취소했습니다. 저장 데이터 백업은 PC에 남아 있습니다: {backupPath}";
            AppendLog("사용자가 작업을 취소했습니다.");
            if (backupPath is not null) AppendLog($"보존된 저장 데이터 백업: {backupPath}");
        }
        catch (Exception ex)
        {
            StageText.Text = "설치 실패";
            DetailText.Text = backupPath is null
                ? ex.Message
                : $"{ex.Message} 저장 데이터 백업은 PC에 남아 있습니다: {backupPath}";
            AppendLog($"오류: {ex}");
            if (backupPath is not null) AppendLog($"보존된 저장 데이터 백업: {backupPath}");
            LogExpander.IsExpanded = true;
            MessageBox.Show(this,
                backupPath is null ? ex.Message : $"{ex.Message}\n\n저장 데이터 백업은 삭제되지 않았습니다.\n{backupPath}",
                "설치 실패", MessageBoxButton.OK, MessageBoxImage.Error);
        }
        finally
        {
            _operationCancellation.Dispose();
            _operationCancellation = null;
            SetBusy(false);
        }
    }

    private void CancelButton_Click(object sender, RoutedEventArgs e)
    {
        var answer = MessageBox.Show(this,
            "플래싱을 중단하면 기기가 부팅되지 않을 수 있습니다. 그래도 중단하시겠습니까?",
            "설치 중단 확인", MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No);
        if (answer == MessageBoxResult.Yes) _operationCancellation?.Cancel();
    }

    private void Window_Closing(object? sender, CancelEventArgs e)
    {
        if (!_busy) return;
        e.Cancel = true;
        MessageBox.Show(this, "설치가 끝나거나 취소된 뒤 창을 닫아 주세요.", "설치 진행 중",
            MessageBoxButton.OK, MessageBoxImage.Warning);
    }

    private void UpdateProgress(ProgressUpdate update)
    {
        var value = Math.Clamp(update.Percent, 0, 100);
        InstallProgress.Value = value;
        PercentText.Text = $"{value:F0}%";
        StageText.Text = update.Stage;
        DetailText.Text = update.Detail;
    }

    private void AppendLog(string message)
    {
        if (!Dispatcher.CheckAccess())
        {
            Dispatcher.Invoke(() => AppendLog(message));
            return;
        }
        var line = $"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}";
        LogTextBox.AppendText(line);
        LogTextBox.ScrollToEnd();
        try { File.AppendAllText(_logFilePath, line, new UTF8Encoding(false)); }
        catch (IOException) { }
    }

    private void SetBusy(bool busy)
    {
        _busy = busy;
        DeviceComboBox.IsEnabled = !busy;
        BoardComboBox.IsEnabled = !busy;
        RefreshButton.IsEnabled = !busy;
        RefreshAssetsButton.IsEnabled = !busy;
        StartButton.IsEnabled = !busy;
        CancelButton.IsEnabled = busy;
    }

    private static string CreateLogFilePath()
    {
        var directory = AppStoragePaths.Under("Logs");
        Directory.CreateDirectory(directory);
        return Path.Combine(directory, $"flash-{DateTime.Now:yyyyMMdd-HHmmss}.log");
    }

    private static string CreateBackupFilePath(BoardDefinition board, string portName)
    {
        var directory = AppStoragePaths.Under("Backups");
        Directory.CreateDirectory(directory);
        var safePort = string.Concat(portName.Select(character =>
            Path.GetInvalidFileNameChars().Contains(character) ? '_' : character));
        return Path.Combine(directory,
            $"tamapoke-save-{DateTime.Now:yyyyMMdd-HHmmss}-{board.Id}-{safePort}.nvs.bin");
    }
}
