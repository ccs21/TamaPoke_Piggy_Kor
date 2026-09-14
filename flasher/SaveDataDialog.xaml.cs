using System.Windows;

namespace TamaPoke.Flasher;

public partial class SaveDataDialog : Window
{
    public SaveDataChoice Choice { get; private set; } = SaveDataChoice.Cancel;

    public SaveDataDialog(string firmwareVersion, bool confirmedKorean = true)
    {
        InitializeComponent();
        VersionText.Text = $"감지된 펌웨어: {firmwareVersion}";
        HeadingText.Text = confirmedKorean
            ? "기존 타마포케 한글판 저장 데이터를 확인했습니다"
            : "저장 데이터 처리 방법을 선택하세요";
    }

    private void KeepButton_Click(object sender, RoutedEventArgs e)
    {
        Choice = SaveDataChoice.Keep;
        DialogResult = true;
    }

    private void DeleteButton_Click(object sender, RoutedEventArgs e)
    {
        var answer = MessageBox.Show(this,
            "현재 포켓몬, 도감, 아이템과 설정이 모두 삭제됩니다.\n\n정말 저장 데이터를 삭제하고 새로 설치하시겠습니까?",
            "저장 데이터 삭제 확인",
            MessageBoxButton.YesNo,
            MessageBoxImage.Warning,
            MessageBoxResult.No);
        if (answer != MessageBoxResult.Yes) return;
        Choice = SaveDataChoice.Delete;
        DialogResult = true;
    }

    private void CancelButton_Click(object sender, RoutedEventArgs e)
    {
        Choice = SaveDataChoice.Cancel;
        DialogResult = false;
    }
}
