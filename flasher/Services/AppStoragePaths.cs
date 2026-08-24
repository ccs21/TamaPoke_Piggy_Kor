namespace TamaPoke.Flasher.Services;

// The public flasher is portable. Keep every downloaded, generated and backup
// file visibly beside the executable instead of hiding data under AppData.
public static class AppStoragePaths
{
    public static string Root { get; } = Path.Combine(
        AppContext.BaseDirectory, "TamaPoke-Flasher-Data");

    public static string Under(params string[] parts)
    {
        var path = Root;
        foreach (var part in parts) path = Path.Combine(path, part);
        return path;
    }
}
