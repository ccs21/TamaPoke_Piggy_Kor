namespace TamaPoke.Flasher;

public sealed record SerialDeviceInfo(string PortName, string FriendlyName, string PnpDeviceId, bool IsEspressif)
{
    public string DisplayName => string.IsNullOrWhiteSpace(FriendlyName)
        ? PortName
        : $"{PortName}  ·  {FriendlyName.Replace($"({PortName})", "").Trim()}";
}

public sealed record BoardDefinition(
    string Id,
    string DisplayName,
    int FlashBytes,
    string FlashSizeArgument,
    int FileSystemOffset,
    int FileSystemSize,
    string ResourceStem);

public static class BoardCatalog
{
    public static readonly BoardDefinition Board175 = new(
        "1.75", "1.75 (16MB)", 16 * 1024 * 1024, "16MB",
        0x310000, 0xCE0000, "base-1.75");

    public static readonly BoardDefinition Board175C = new(
        "1.75C", "1.75C (32MB)", 32 * 1024 * 1024, "32MB",
        0x410000, 0x1000000, "base-1.75C");

    public static IReadOnlyList<BoardDefinition> All { get; } = [Board175, Board175C];

    public static BoardDefinition? FromFlashBytes(int flashBytes) =>
        All.FirstOrDefault(board => board.FlashBytes == flashBytes);
}

public sealed record BoardChoice(string Label, BoardDefinition? Board)
{
    public override string ToString() => Label;
}

public sealed record ProgressUpdate(double Percent, string Stage, string Detail);

public enum SaveDataChoice
{
    Keep,
    Delete,
    Cancel,
}

public sealed record FirmwareSaveInfo(string Version, bool IsKorean, bool HasExistingSave, string RawResponse);

public sealed record BoardBasePayload(
    string AppPath, string BootloaderPath, string PartitionsPath, string BootAppPath);

public sealed record PreparedPayload(
    string EsptoolPath, string MkLittleFsPath,
    IReadOnlyDictionary<string, BoardBasePayload> Boards)
{
    public BoardBasePayload For(BoardDefinition board) => Boards[board.Id];
}
