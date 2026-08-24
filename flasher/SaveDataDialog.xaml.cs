using System.Windows;

namespace TamaPoke.Flasher;

public partial class SaveDataDialog : Window
{
    public SaveDataChoice Choice { get; private set; } = SaveDataChoice.Cancel;

    public SaveDataDialog(string firmwareVersion)
    {
        InitializeComponent();
        VersionText.Text = $"감지된 펌웨어: {firmwareVersion}";
    }

    private void KeepButton_Click(object sender, RoutedEventArgs e)
    {
        Choice = SaveDataChoice.Keep;
        DialogResult = true;
    }

    private void DeleteButton_Click(object sender, RoutedEventArgs e)
    {
        Choice = SaveDataChoice.Delete;
        DialogResult = true;
    }

    private void CancelButton_Click(object sender, RoutedEventArgs e)
    {
        Choice = SaveDataChoice.Cancel;
        DialogResult = false;
    }
}
