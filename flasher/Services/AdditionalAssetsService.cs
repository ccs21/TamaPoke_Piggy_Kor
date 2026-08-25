using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using NAudio.Wave;

namespace TamaPoke.Flasher.Services;

public sealed record AdditionalAssetStatus(
    string ArchivePath, string? FallbackArchivePath, bool Exists, bool IsValid,
    bool RequiresSampleSupplement, string CompactText, string? ArchiveHash,
    IReadOnlyList<string> MissingFiles, IReadOnlyList<string> Errors);

public sealed record AdditionalAssetBuildResult(string DataDirectory, string InputHash);

public sealed class AdditionalAssetsService
{
    private const string PipelineVersion = "additional-assets-v5";
    private const int TargetRate = 16_000;
    private const int MaxEntries = 128;
    private const long MaxExpandedBytes = 256L * 1024 * 1024;
    private const long MaxSingleFileBytes = 80L * 1024 * 1024;

    private sealed record VisualDefinition(string Name, string Output, int Width, int Height,
        bool OpaqueBlack, bool CleanSnorlax = false);
    private sealed record AudioDefinition(string Name, string Output, double StartSeconds,
        double DurationSeconds, double FadeSeconds, float Peak, bool TrimSilence = false,
        double MaximumSeconds = 0);
    private sealed record AssetRequirement(string Folder, string Stem, string DisplayName,
        IReadOnlyCollection<string> Extensions);

    private static readonly VisualDefinition[] Visuals =
    [
        new("L01.png", "L01.tvr", 350, 350, true),
        new("B01.png", "B01.tvr", 160, 160, true),
        new("B02.png", "B02.tvr", 160, 160, true),
        new("S01.png", "S01.tvr", 116, 116, false, true),
        new("S02.png", "S02.tvr", 116, 116, false, true),
        new("S03.png", "S03.tvr", 116, 116, false, true),
        new("S04.png", "S04.tvr", 116, 116, false, true),
        new("D01.png", "D01.tvr", 22, 17, false),
        new("D02.png", "D02.tvr", 22, 17, false),
        new("D03.png", "D03.tvr", 22, 17, false),
        new("D04.png", "D04.tvr", 22, 17, false),
    ];

    private static readonly AudioDefinition[] Audio =
    [
        new("P", "pikachu.tpa", 0, 0, 0.005, 0.92f, true, 4.0),
        new("Care", "care.tpa", 0, 0, 0.005, 0.92f, true, 10.0),
        new("C", "catch.tpa", 0, 0, 0.10, 0.92f, false, 10.0),
        new("L", "lowhp.tpa", 0, 0, 0.05, 0.92f, false, 6.0),
        new("E", "evolve.tpa", 1.0, 5.2, 2.0, 0.92f),
        new("B", "farewell.tpa", 0, 10.0, 2.0, 0.64f),
        new("BAT", "battle.tpa", 0, 60.0, 5.0, 0.64f),
        new("MIN", "minigame.tpa", 0, 60.0, 5.0, 0.64f),
        new("MAR", "marriage.tpa", 0, 15.5, 5.0, 0.64f),
    ];

    private readonly string _cacheRoot = AppStoragePaths.Under(
        "Cache", "additional", "5.0.0");
    private readonly string _assetDirectory;

    public AdditionalAssetsService(string? assetDirectory = null)
    {
        _assetDirectory = Path.GetFullPath(assetDirectory ?? AppContext.BaseDirectory);
    }

    public string ArchivePath => Path.Combine(_assetDirectory, "Additional_assets.zip");
    public string SampleArchivePath => Path.Combine(_assetDirectory, "sample_Additional_assets.zip");

    public AdditionalAssetStatus Scan()
    {
        var hasPrimary = File.Exists(ArchivePath);
        var hasSample = File.Exists(SampleArchivePath);
        if (!hasPrimary && !hasSample)
            return new(ArchivePath, null, false, false, false,
                "추가 자산 ZIP이 없습니다. 플래셔를 다시 압축 해제해 주세요.", null, [],
                ["Additional_assets.zip 또는 sample_Additional_assets.zip을 찾지 못했습니다."]);

        var selectedPath = hasPrimary ? ArchivePath : SampleArchivePath;
        var errors = new List<string>();
        var missing = new List<AssetRequirement>();
        string? hash = null;
        string? fallbackPath = null;
        var requiresSampleSupplement = false;
        var disabledAudio = 0;
        try
        {
            var selectedHash = ComputeFileHash(selectedPath);
            using var archive = ZipFile.OpenRead(selectedPath);
            ValidateArchiveEnvelope(archive, errors);

            foreach (var requirement in Requirements())
            {
                var matches = FindEntries(archive, requirement);
                if (matches.Length == 0) missing.Add(requirement);
                else if (matches.Length > 1)
                    errors.Add($"중복: {requirement.DisplayName} 파일은 하나만 넣어 주세요.");
                else if (ValidateEntryContent(matches[0], requirement) is not null)
                    missing.Add(requirement);
            }

            ZipArchive? sampleArchive = null;
            try
            {
                if (missing.Count > 0 && hasPrimary)
                {
                    if (!hasSample)
                    {
                        errors.AddRange(missing.Select(item => $"누락: {item.DisplayName}"));
                    }
                    else
                    {
                        fallbackPath = SampleArchivePath;
                        sampleArchive = ZipFile.OpenRead(fallbackPath);
                        var sampleErrors = new List<string>();
                        ValidateArchiveEnvelope(sampleArchive, sampleErrors);
                        errors.AddRange(sampleErrors.Select(error => $"sample_Additional_assets.zip: {error}"));
                        foreach (var requirement in missing)
                        {
                            var matches = FindEntries(sampleArchive, requirement);
                            if (matches.Length == 0)
                                errors.Add($"샘플에도 누락: {requirement.DisplayName}");
                            else if (matches.Length > 1)
                                errors.Add($"샘플 중복: {requirement.DisplayName} 파일은 하나만 있어야 합니다.");
                            else if (ValidateEntryContent(matches[0], requirement) is { } contentError)
                                errors.Add($"샘플 손상: {requirement.DisplayName} ({contentError})");
                        }
                        if (errors.Count == 0)
                        {
                            requiresSampleSupplement = true;
                            var sampleHash = ComputeFileHash(fallbackPath);
                            hash = HashText($"{selectedHash}|{sampleHash}|{string.Join('|', missing.Select(item => item.DisplayName))}");
                        }
                    }
                }
                else if (missing.Count > 0)
                {
                    errors.AddRange(missing.Select(item => $"누락: {item.DisplayName}"));
                }

                if (errors.Count == 0)
                {
                    hash ??= selectedHash;
                    foreach (var audio in Audio)
                    {
                        var requirement = AudioRequirement(audio);
                        var needsSample = missing.Any(item => item.DisplayName == requirement.DisplayName);
                        var entry = !needsSample
                            ? FindEntries(archive, requirement).SingleOrDefault()
                            : sampleArchive is null ? null : FindEntries(sampleArchive, requirement).SingleOrDefault();
                        if (entry?.Length == 0) disabledAudio++;
                    }
                }
            }
            finally
            {
                sampleArchive?.Dispose();
            }
        }
        catch (Exception ex)
        {
            errors.Add($"ZIP을 읽을 수 없습니다: {ex.Message}");
        }

        var valid = errors.Count == 0;
        string compact;
        if (!valid)
            compact = "추가 자산 ZIP 구성을 확인해 주세요.";
        else if (requiresSampleSupplement)
            compact = $"Additional_assets.zip 우선 · 누락 {missing.Count}개 샘플 보충 가능 · {hash![..12]}";
        else
        {
            var suppliedAudio = Audio.Length - disabledAudio;
            var sourceName = hasPrimary ? "Additional_assets.zip" : "sample_Additional_assets.zip";
            compact = $"{sourceName} 확인됨 · 이미지 {Visuals.Length}개 · 음원 {suppliedAudio}개 · 생략 {disabledAudio}개 · {hash![..12]}";
        }
        return new(selectedPath, fallbackPath, true, valid, requiresSampleSupplement,
            compact, hash, missing.Select(item => item.DisplayName).ToArray(), errors);
    }

    public async Task<AdditionalAssetBuildResult> PrepareAsync(
        AdditionalAssetStatus status, bool allowSampleSupplement, Action<string> log,
        IProgress<ProgressUpdate>? progress, CancellationToken token)
    {
        if (!status.IsValid || status.ArchiveHash is null)
            throw new InvalidOperationException("추가 자산 ZIP이 없거나 구성이 올바르지 않습니다.");
        if (status.RequiresSampleSupplement && !allowSampleSupplement)
            throw new InvalidOperationException("누락된 파일을 샘플에서 보충하도록 승인하지 않았습니다.");
        var keyText = $"{PipelineVersion}|{status.ArchiveHash}";
        var inputHash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(keyText))).ToLowerInvariant();
        var key = inputHash[..24];
        var root = Path.Combine(_cacheRoot, key);
        var extracted = Path.Combine(root, "extracted");
        var data = Path.Combine(root, "data");
        var disabled = Path.Combine(root, "disabled");
        var marker = Path.Combine(root, "complete.sha256");
        if (File.Exists(marker) && File.ReadAllText(marker).Trim() == inputHash &&
            RequiredOutputsExist(data, disabled))
        {
            log($"추가 자산 캐시 재사용: {root}");
            progress?.Report(new ProgressUpdate(18, "추가 자산 준비 완료", "이전에 가공한 동일한 ZIP을 재사용합니다."));
            return new(data, inputHash);
        }

        Directory.CreateDirectory(extracted);
        Directory.CreateDirectory(Path.Combine(data, "extra"));
        Directory.CreateDirectory(Path.Combine(data, "audio"));
        Directory.CreateDirectory(disabled);
        progress?.Report(new ProgressUpdate(8, "추가 자산 압축 해제", "추가 자산 ZIP을 안전하게 검사하고 캐시에 보관합니다."));
        var primaryExtracted = Path.Combine(extracted, "selected");
        var fallbackExtracted = Path.Combine(extracted, "sample");
        using (var archive = ZipFile.OpenRead(status.ArchivePath))
        using (var fallbackArchive = status.RequiresSampleSupplement && status.FallbackArchivePath is not null
                   ? ZipFile.OpenRead(status.FallbackArchivePath)
                   : null)
        {
            var envelopeErrors = new List<string>();
            ValidateArchiveEnvelope(archive, envelopeErrors);
            if (fallbackArchive is not null) ValidateArchiveEnvelope(fallbackArchive, envelopeErrors);
            if (envelopeErrors.Count != 0) throw new InvalidDataException(string.Join(Environment.NewLine, envelopeErrors));
            await ExtractSafelyAsync(archive, primaryExtracted, token);
            if (fallbackArchive is not null)
                await ExtractSafelyAsync(fallbackArchive, fallbackExtracted, token);

            (ZipArchiveEntry Entry, string Root, bool FromSample) Resolve(AssetRequirement requirement)
            {
                var needsSample = status.MissingFiles.Contains(requirement.DisplayName, StringComparer.Ordinal);
                var primary = needsSample ? null : FindEntries(archive, requirement).SingleOrDefault();
                if (primary is not null) return (primary, primaryExtracted, false);
                var sample = fallbackArchive is null
                    ? null
                    : FindEntries(fallbackArchive, requirement).SingleOrDefault();
                if (sample is null) throw new InvalidDataException($"누락: {requirement.DisplayName}");
                return (sample, fallbackExtracted, true);
            }

            progress?.Report(new ProgressUpdate(11, "이미지 가공", "사용자가 준비한 이미지를 기기용 형식으로 변환합니다."));
            foreach (var visual in Visuals.Where(item => !item.CleanSnorlax))
            {
                token.ThrowIfCancellationRequested();
                var resolved = Resolve(VisualRequirement(visual));
                var source = SafeExtractedPath(resolved.Root, resolved.Entry.FullName);
                var output = Path.Combine(data, "extra", visual.Output);
                await Task.Run(() => ConvertVisual(source, output, visual), token);
                log($"이미지 가공: {visual.Name} -> {visual.Output}" + (resolved.FromSample ? " (샘플 보충)" : ""));
            }

            var snorlaxInputs = Visuals.Where(item => item.CleanSnorlax)
                .Select(visual =>
                {
                    var resolved = Resolve(VisualRequirement(visual));
                    return (Definition: visual,
                        Source: SafeExtractedPath(resolved.Root, resolved.Entry.FullName),
                        Destination: Path.Combine(data, "extra", visual.Output),
                        resolved.FromSample);
                }).ToArray();
            await Task.Run(() => ConvertSnorlaxVisuals(snorlaxInputs), token);
            foreach (var item in snorlaxInputs)
                log($"이미지 가공: {item.Definition.Name} -> {item.Definition.Output}" + (item.FromSample ? " (샘플 보충)" : ""));

            progress?.Report(new ProgressUpdate(14, "음원 가공", "사용자가 준비한 음원을 16kHz 모노 형식으로 변환합니다."));
            foreach (var definition in Audio)
            {
                token.ThrowIfCancellationRequested();
                var resolved = Resolve(AudioRequirement(definition));
                var entry = resolved.Entry;
                var source = SafeExtractedPath(resolved.Root, entry.FullName);
                var output = Path.Combine(data, "audio", definition.Output);
                var disabledMarker = Path.Combine(disabled, definition.Output + ".disabled");
                if (entry.Length == 0)
                {
                    if (File.Exists(output)) File.Delete(output);
                    await File.WriteAllTextAsync(disabledMarker, "disabled", Encoding.ASCII, token);
                    log($"음원 생략: {Path.GetFileName(source)} (0바이트 표시 파일)" + (resolved.FromSample ? " (샘플 보충)" : ""));
                    continue;
                }
                if (File.Exists(disabledMarker)) File.Delete(disabledMarker);
                var samples = await Task.Run(() => LoadAndConvertAudio(source, definition, token), token);
                await File.WriteAllBytesAsync(output, EncodeTpa(samples), token);
                log($"음원 가공: {Path.GetFileName(source)} -> {definition.Output}" + (resolved.FromSample ? " (샘플 보충)" : ""));
            }
        }
        await File.WriteAllTextAsync(marker, inputHash, Encoding.ASCII, token);
        progress?.Report(new ProgressUpdate(18, "추가 자산 준비 완료", "압축 원본과 변환 결과를 다음 설치용 캐시에 보관했습니다."));
        return new(data, inputHash);
    }

    private static bool RequiredOutputsExist(string data, string disabled)
    {
        return Visuals.All(item => File.Exists(Path.Combine(data, "extra", item.Output))) &&
               Audio.All(item => File.Exists(Path.Combine(data, "audio", item.Output)) ||
                                 File.Exists(Path.Combine(disabled, item.Output + ".disabled")));
    }

    private static IEnumerable<AssetRequirement> Requirements()
    {
        foreach (var visual in Visuals) yield return VisualRequirement(visual);
        foreach (var audio in Audio) yield return AudioRequirement(audio);
    }

    private static AssetRequirement VisualRequirement(VisualDefinition definition) =>
        new("Visual", Path.GetFileNameWithoutExtension(definition.Name),
            $"Visual/{definition.Name}", [".png"]);

    private static AssetRequirement AudioRequirement(AudioDefinition definition) =>
        new("Audio", definition.Name,
            $"Audio/{definition.Name}.mp3 또는 {definition.Name}.wav", [".mp3", ".wav"]);

    private static ZipArchiveEntry[] FindEntries(ZipArchive archive, AssetRequirement requirement)
    {
        var suffixes = requirement.Extensions
            .Select(extension => $"/{requirement.Folder}/{requirement.Stem}{extension}").ToArray();
        return archive.Entries.Where(entry =>
        {
            var path = "/" + entry.FullName.Replace('\\', '/').TrimStart('/');
            return suffixes.Any(suffix => path.EndsWith(suffix, StringComparison.OrdinalIgnoreCase));
        }).ToArray();
    }

    private static string? ValidateEntryContent(ZipArchiveEntry entry, AssetRequirement requirement)
    {
        try
        {
            if (entry.Length == 0 && requirement.Folder.Equals("Audio", StringComparison.OrdinalIgnoreCase))
                return null;
            if (entry.Length == 0) return "파일이 비어 있습니다";

            if (requirement.Folder.Equals("Visual", StringComparison.OrdinalIgnoreCase))
            {
                using var pngCompressed = entry.Open();
                using var input = new MemoryStream(entry.Length > int.MaxValue ? 0 : (int)entry.Length);
                pngCompressed.CopyTo(input);
                input.Position = 0;
                var decoder = BitmapDecoder.Create(input, BitmapCreateOptions.PreservePixelFormat,
                    BitmapCacheOption.OnLoad);
                if (decoder.Frames.Count == 0) return "PNG 화면을 읽을 수 없습니다";
                var frame = decoder.Frames[0];
                var visual = Visuals.Single(item =>
                    Path.GetFileNameWithoutExtension(item.Name).Equals(requirement.Stem,
                        StringComparison.OrdinalIgnoreCase));
                if (visual.Name.StartsWith('D') &&
                    (frame.PixelWidth != visual.Width || frame.PixelHeight != visual.Height))
                    return $"크기가 {visual.Width}x{visual.Height}px이 아닙니다";
                var stride = Math.Max(1, (frame.PixelWidth * frame.Format.BitsPerPixel + 7) / 8);
                frame.CopyPixels(new byte[stride * frame.PixelHeight], stride, 0);
                return null;
            }

            using var compressed = entry.Open();
            using var memory = new MemoryStream(entry.Length > int.MaxValue ? 0 : (int)entry.Length);
            compressed.CopyTo(memory);
            memory.Position = 0;
            using WaveStream reader = Path.GetExtension(entry.Name).Equals(".mp3", StringComparison.OrdinalIgnoreCase)
                ? new Mp3FileReader(memory)
                : new WaveFileReader(memory);
            var definition = Audio.Single(item => item.Name.Equals(requirement.Stem,
                StringComparison.OrdinalIgnoreCase));
            if (reader.TotalTime.TotalSeconds < 0.1) return "음원이 0.1초보다 짧습니다";
            if (definition.DurationSeconds > 0 &&
                reader.TotalTime.TotalSeconds + 0.01 < definition.StartSeconds + definition.DurationSeconds)
                return $"음원이 {definition.StartSeconds + definition.DurationSeconds:0.#}초보다 짧습니다";
            var buffer = new byte[8192];
            while (reader.Read(buffer, 0, buffer.Length) > 0) { }
            return null;
        }
        catch (Exception ex)
        {
            return ex.Message;
        }
    }

    private static string ComputeFileHash(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    private static string HashText(string text) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text))).ToLowerInvariant();

    private static void ValidateArchiveEnvelope(ZipArchive archive, List<string> errors)
    {
        if (archive.Entries.Count > MaxEntries) errors.Add($"ZIP 파일 수가 너무 많습니다. 최대 {MaxEntries}개입니다.");
        long total = 0;
        foreach (var entry in archive.Entries)
        {
            if (entry.Length > MaxSingleFileBytes) errors.Add($"파일이 너무 큽니다: {entry.FullName}");
            total += entry.Length;
            if (total > MaxExpandedBytes) { errors.Add("압축 해제 크기가 256MB를 초과합니다."); break; }
            var normalized = entry.FullName.Replace('\\', '/');
            if (normalized.StartsWith('/') || normalized.Contains(':') ||
                normalized.Split('/', StringSplitOptions.RemoveEmptyEntries).Any(part => part == ".."))
                errors.Add($"안전하지 않은 ZIP 경로입니다: {entry.FullName}");
            var unixType = (entry.ExternalAttributes >> 16) & 0xF000;
            if (unixType == 0xA000) errors.Add($"심볼릭 링크는 사용할 수 없습니다: {entry.FullName}");
        }
    }

    private static async Task ExtractSafelyAsync(ZipArchive archive, string root, CancellationToken token)
    {
        var rootFull = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        foreach (var entry in archive.Entries)
        {
            token.ThrowIfCancellationRequested();
            if (string.IsNullOrEmpty(entry.Name)) continue;
            var destination = SafeExtractedPath(root, entry.FullName);
            if (!Path.GetFullPath(destination).StartsWith(rootFull, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"ZIP 경로가 캐시 밖을 가리킵니다: {entry.FullName}");
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            await using var input = entry.Open();
            await using var output = new FileStream(destination, FileMode.Create, FileAccess.Write,
                FileShare.None, 65536, FileOptions.Asynchronous | FileOptions.SequentialScan);
            await input.CopyToAsync(output, token);
        }
    }

    private static string SafeExtractedPath(string root, string entryName)
    {
        var parts = entryName.Replace('\\', '/').Split('/', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Any(part => part is "." or ".." || part.Contains(':')))
            throw new InvalidDataException($"안전하지 않은 ZIP 경로입니다: {entryName}");
        return Path.Combine([root, .. parts]);
    }

    private static void ConvertVisual(string source, string destination, VisualDefinition definition)
    {
        var bitmap = LoadBitmap(source);
        if (definition.Name.StartsWith('D') &&
            (bitmap.Width != definition.Width || bitmap.Height != definition.Height))
            throw new InvalidDataException($"{definition.Name} 크기는 {definition.Width}x{definition.Height}px이어야 합니다.");
        var pixels = ResizeNearest(bitmap.Pixels, bitmap.Width, bitmap.Height,
            definition.Width, definition.Height);
        var encoded = EncodeTvr(pixels, definition.Width, definition.Height, definition.OpaqueBlack);
        File.WriteAllBytes(destination, encoded);
    }

    private sealed record BgraBitmap(int Width, int Height, byte[] Pixels);
    private readonly record struct PaletteColor(byte R, byte G, byte B, int Count);

    private sealed class PaletteBox(List<PaletteColor> colors)
    {
        public List<PaletteColor> Colors { get; } = colors;
        public long Population => Colors.Sum(color => (long)color.Count);

        public int Channel
        {
            get
            {
                var r = Colors.Max(color => color.R) - Colors.Min(color => color.R);
                var g = Colors.Max(color => color.G) - Colors.Min(color => color.G);
                var b = Colors.Max(color => color.B) - Colors.Min(color => color.B);
                return r >= g && r >= b ? 0 : g >= b ? 1 : 2;
            }
        }

        public long Score
        {
            get
            {
                var range = Channel switch
                {
                    0 => Colors.Max(color => color.R) - Colors.Min(color => color.R),
                    1 => Colors.Max(color => color.G) - Colors.Min(color => color.G),
                    _ => Colors.Max(color => color.B) - Colors.Min(color => color.B),
                };
                return (long)(range + 1) * Population;
            }
        }
    }

    private static void ConvertSnorlaxVisuals(
        IReadOnlyList<(VisualDefinition Definition, string Source, string Destination, bool FromSample)> inputs)
    {
        var frames = new List<BgraBitmap>(inputs.Count);
        foreach (var item in inputs)
        {
            var source = LoadBitmap(item.Source);
            var cleaned = CleanSnorlaxSource(source);
            var resized = ResizeLanczos(cleaned, item.Definition.Width, item.Definition.Height);
            for (var index = 3; index < resized.Pixels.Length; index += 4)
                resized.Pixels[index] = resized.Pixels[index] >= 96 ? (byte)255 : (byte)0;
            frames.Add(resized);
        }

        QuantizeSharedPalette(frames, 63);
        for (var index = 0; index < inputs.Count; index++)
        {
            var item = inputs[index];
            var frame = frames[index];
            File.WriteAllBytes(item.Destination,
                EncodeTvr(frame.Pixels, frame.Width, frame.Height, false));
        }
    }

    private static BgraBitmap LoadBitmap(string path)
    {
        using var stream = File.OpenRead(path);
        var decoder = BitmapDecoder.Create(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
        var converted = new FormatConvertedBitmap(decoder.Frames[0], PixelFormats.Bgra32, null, 0);
        var stride = converted.PixelWidth * 4;
        var pixels = new byte[stride * converted.PixelHeight];
        converted.CopyPixels(pixels, stride, 0);
        return new(converted.PixelWidth, converted.PixelHeight, pixels);
    }

    private static byte[] ResizeNearest(byte[] source, int sourceWidth, int sourceHeight,
        int width, int height)
    {
        var output = new byte[width * height * 4];
        for (var y = 0; y < height; y++)
        for (var x = 0; x < width; x++)
        {
            var sx = Math.Min(sourceWidth - 1, (int)((x + 0.5) * sourceWidth / width));
            var sy = Math.Min(sourceHeight - 1, (int)((y + 0.5) * sourceHeight / height));
            var sourceIndex = (sy * sourceWidth + sx) * 4;
            var outputIndex = (y * width + x) * 4;
            Buffer.BlockCopy(source, sourceIndex, output, outputIndex, 4);
        }
        return output;
    }

    private static BgraBitmap CleanSnorlaxSource(BgraBitmap source)
    {
        var width = source.Width;
        var height = source.Height;
        var integral = new int[(width + 1) * (height + 1)];
        for (var y = 0; y < height; y++)
        {
            var rowTotal = 0;
            for (var x = 0; x < width; x++)
            {
                var index = (y * width + x) * 4;
                var b = source.Pixels[index];
                var g = source.Pixels[index + 1];
                var r = source.Pixels[index + 2];
                var a = source.Pixels[index + 3];
                var spread = Math.Max(r, Math.Max(g, b)) - Math.Min(r, Math.Min(g, b));
                var seed = a >= 24 && (spread >= 16 || (r > 190 && g > 170 && b > 145));
                if (seed) rowTotal++;
                integral[(y + 1) * (width + 1) + x + 1] =
                    integral[y * (width + 1) + x + 1] + rowTotal;
            }
        }

        var output = new byte[source.Pixels.Length];
        for (var y = 0; y < height; y++)
        for (var x = 0; x < width; x++)
        {
            var left = Math.Max(0, x - 4);
            var top = Math.Max(0, y - 4);
            var right = Math.Min(width - 1, x + 4) + 1;
            var bottom = Math.Min(height - 1, y + 4) + 1;
            var nearby = integral[bottom * (width + 1) + right]
                         - integral[top * (width + 1) + right]
                         - integral[bottom * (width + 1) + left]
                         + integral[top * (width + 1) + left];
            var index = (y * width + x) * 4;
            if (nearby == 0 || source.Pixels[index + 3] < 24) continue;
            output[index] = source.Pixels[index];
            output[index + 1] = source.Pixels[index + 1];
            output[index + 2] = source.Pixels[index + 2];
            output[index + 3] = source.Pixels[index + 3] >= 96 ? (byte)255 : (byte)0;
        }
        return new(width, height, output);
    }

    private static BgraBitmap ResizeLanczos(BgraBitmap source, int width, int height)
    {
        var output = new byte[width * height * 4];
        var scaleX = source.Width / (double)width;
        var scaleY = source.Height / (double)height;
        var filterX = Math.Max(1.0, scaleX);
        var filterY = Math.Max(1.0, scaleY);
        var supportX = 3.0 * filterX;
        var supportY = 3.0 * filterY;

        for (var y = 0; y < height; y++)
        {
            var centerY = (y + 0.5) * scaleY - 0.5;
            var firstY = Math.Max(0, (int)Math.Ceiling(centerY - supportY));
            var lastY = Math.Min(source.Height - 1, (int)Math.Floor(centerY + supportY));
            for (var x = 0; x < width; x++)
            {
                var centerX = (x + 0.5) * scaleX - 0.5;
                var firstX = Math.Max(0, (int)Math.Ceiling(centerX - supportX));
                var lastX = Math.Min(source.Width - 1, (int)Math.Floor(centerX + supportX));
                double weightTotal = 0, alphaTotal = 0;
                double blueTotal = 0, greenTotal = 0, redTotal = 0;
                for (var sy = firstY; sy <= lastY; sy++)
                {
                    var wy = Lanczos((centerY - sy) / filterY);
                    if (Math.Abs(wy) < 0.0000001) continue;
                    for (var sx = firstX; sx <= lastX; sx++)
                    {
                        var weight = wy * Lanczos((centerX - sx) / filterX);
                        if (Math.Abs(weight) < 0.0000001) continue;
                        var sourceIndex = (sy * source.Width + sx) * 4;
                        var alpha = source.Pixels[sourceIndex + 3] / 255.0;
                        weightTotal += weight;
                        alphaTotal += weight * alpha;
                        blueTotal += weight * alpha * source.Pixels[sourceIndex];
                        greenTotal += weight * alpha * source.Pixels[sourceIndex + 1];
                        redTotal += weight * alpha * source.Pixels[sourceIndex + 2];
                    }
                }

                var outputIndex = (y * width + x) * 4;
                if (Math.Abs(weightTotal) < 0.0000001 || alphaTotal <= 0.0000001) continue;
                output[outputIndex] = ClampByte(blueTotal / alphaTotal);
                output[outputIndex + 1] = ClampByte(greenTotal / alphaTotal);
                output[outputIndex + 2] = ClampByte(redTotal / alphaTotal);
                output[outputIndex + 3] = ClampByte(alphaTotal / weightTotal * 255.0);
            }
        }
        return new(width, height, output);
    }

    private static double Lanczos(double value)
    {
        value = Math.Abs(value);
        if (value < 0.0000001) return 1.0;
        if (value >= 3.0) return 0.0;
        var piValue = Math.PI * value;
        return Math.Sin(piValue) / piValue * Math.Sin(piValue / 3.0) / (piValue / 3.0);
    }

    private static byte ClampByte(double value) =>
        (byte)Math.Clamp((int)Math.Round(value), 0, 255);

    private static void QuantizeSharedPalette(IReadOnlyList<BgraBitmap> frames, int colorLimit)
    {
        var histogram = new Dictionary<int, int>();
        foreach (var frame in frames)
        for (var index = 0; index < frame.Pixels.Length; index += 4)
        {
            var key = frame.Pixels[index + 3] >= 96
                ? frame.Pixels[index + 2] << 16 | frame.Pixels[index + 1] << 8 | frame.Pixels[index]
                : 0xFFFFFF;
            histogram.TryGetValue(key, out var count);
            histogram[key] = count + 1;
        }

        var colors = histogram.Select(item => new PaletteColor(
            (byte)(item.Key >> 16), (byte)(item.Key >> 8), (byte)item.Key, item.Value)).ToList();
        var boxes = new List<PaletteBox> { new(colors) };
        while (boxes.Count < colorLimit)
        {
            var candidate = boxes.Select((box, index) => (box, index))
                .Where(item => item.box.Colors.Count > 1)
                .OrderByDescending(item => item.box.Score)
                .FirstOrDefault();
            if (candidate.box is null) break;

            var channel = candidate.box.Channel;
            candidate.box.Colors.Sort((left, right) => channel switch
            {
                0 => left.R.CompareTo(right.R),
                1 => left.G.CompareTo(right.G),
                _ => left.B.CompareTo(right.B),
            });
            var half = candidate.box.Population / 2;
            long accumulated = 0;
            var split = 1;
            for (var index = 0; index < candidate.box.Colors.Count - 1; index++)
            {
                accumulated += candidate.box.Colors[index].Count;
                split = index + 1;
                if (accumulated >= half) break;
            }
            boxes.RemoveAt(candidate.index);
            boxes.Add(new(candidate.box.Colors.Take(split).ToList()));
            boxes.Add(new(candidate.box.Colors.Skip(split).ToList()));
        }

        var palette = boxes.Select(box =>
        {
            var population = Math.Max(1, box.Population);
            return new PaletteColor(
                (byte)(box.Colors.Sum(color => (long)color.R * color.Count) / population),
                (byte)(box.Colors.Sum(color => (long)color.G * color.Count) / population),
                (byte)(box.Colors.Sum(color => (long)color.B * color.Count) / population), 1);
        }).ToArray();

        foreach (var frame in frames)
        for (var index = 0; index < frame.Pixels.Length; index += 4)
        {
            if (frame.Pixels[index + 3] < 96) continue;
            var b = frame.Pixels[index];
            var g = frame.Pixels[index + 1];
            var r = frame.Pixels[index + 2];
            var best = palette[0];
            var bestDistance = int.MaxValue;
            foreach (var color in palette)
            {
                var dr = r - color.R;
                var dg = g - color.G;
                var db = b - color.B;
                var distance = dr * dr + dg * dg + db * db;
                if (distance >= bestDistance) continue;
                bestDistance = distance;
                best = color;
            }
            frame.Pixels[index] = best.B;
            frame.Pixels[index + 1] = best.G;
            frame.Pixels[index + 2] = best.R;
        }
    }

    private static byte[] EncodeTvr(byte[] bgra, int width, int height, bool opaqueBlack)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.ASCII, true);
        writer.Write("TVR1"u8);
        writer.Write((ushort)width);
        writer.Write((ushort)height);
        writer.Write(0u);
        uint runCount = 0;
        for (var y = 0; y < height; y++)
        {
            var x = 0;
            while (x < width)
            {
                var index = (y * width + x) * 4;
                var color = EncodedPixel(bgra, index, opaqueBlack, out var visible);
                var end = x + 1;
                while (end < width)
                {
                    var next = (y * width + end) * 4;
                    var nextColor = EncodedPixel(bgra, next, opaqueBlack, out var nextVisible);
                    if (nextVisible != visible || nextColor != color) break;
                    end++;
                }
                writer.Write((ushort)(end - x));
                writer.Write(color);
                writer.Write((ushort)(visible ? 1 : 0));
                runCount++;
                x = end;
            }
        }
        stream.Position = 8;
        writer.Write(runCount);
        writer.Flush();
        return stream.ToArray();
    }

    private static ushort EncodedPixel(byte[] bgra, int index, bool compositeBlack, out bool visible)
    {
        var alpha = bgra[index + 3];
        visible = compositeBlack || alpha >= 96;
        if (!visible) return 0;
        if (!compositeBlack)
            return ToRgb565(bgra[index + 2], bgra[index + 1], bgra[index]);

        var red = (byte)((bgra[index + 2] * alpha + 127) / 255);
        var green = (byte)((bgra[index + 1] * alpha + 127) / 255);
        var blue = (byte)((bgra[index] * alpha + 127) / 255);
        return ToRgb565(red, green, blue);
    }

    private static ushort ToRgb565(byte r, byte g, byte b) =>
        (ushort)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));

    private static short[] LoadAndConvertAudio(string path, AudioDefinition definition, CancellationToken token)
    {
        using var reader = new AudioFileReader(path);
        if (reader.TotalTime.TotalSeconds < 0.1) throw new InvalidDataException($"음원이 너무 짧습니다: {Path.GetFileName(path)}");
        if (definition.DurationSeconds > 0 &&
            reader.TotalTime.TotalSeconds + 0.01 < definition.StartSeconds + definition.DurationSeconds)
            throw new InvalidDataException($"{Path.GetFileName(path)}은(는) {definition.StartSeconds + definition.DurationSeconds:0.#}초 이상이어야 합니다.");
        var channels = reader.WaveFormat.Channels;
        var sourceRate = reader.WaveFormat.SampleRate;
        var values = new List<float>((int)Math.Min(int.MaxValue, reader.Length / 4));
        var buffer = new float[8192 - 8192 % channels];
        int read;
        while ((read = reader.Read(buffer, 0, buffer.Length)) > 0)
        {
            token.ThrowIfCancellationRequested();
            for (var index = 0; index + channels <= read; index += channels)
            {
                float mono = 0;
                for (var channel = 0; channel < channels; channel++) mono += buffer[index + channel];
                values.Add(mono / channels);
            }
        }
        var start = Math.Min(values.Count, (int)Math.Round(definition.StartSeconds * sourceRate));
        var available = values.Count - start;
        var wanted = definition.DurationSeconds > 0
            ? (int)Math.Round(definition.DurationSeconds * sourceRate)
            : available;
        if (definition.MaximumSeconds > 0)
            wanted = Math.Min(wanted, (int)Math.Round(definition.MaximumSeconds * sourceRate));
        var end = Math.Min(values.Count, start + wanted);
        if (definition.TrimSilence)
        {
            while (start < end && Math.Abs(values[start]) < 0.003f) start++;
            while (end > start && Math.Abs(values[end - 1]) < 0.003f) end--;
            start = Math.Max(0, start - (int)(0.15 * sourceRate));
            end = Math.Min(values.Count, end + (int)(0.20 * sourceRate));
        }
        var targetCount = Math.Max(1, (int)Math.Round((end - start) * (double)TargetRate / sourceRate));
        var output = new float[targetCount];
        for (var index = 0; index < targetCount; index++)
        {
            var sourcePosition = start + index * (double)sourceRate / TargetRate;
            var left = Math.Min(end - 1, (int)sourcePosition);
            var right = Math.Min(end - 1, left + 1);
            var fraction = (float)(sourcePosition - left);
            output[index] = values[left] + (values[right] - values[left]) * fraction;
        }
        var peak = output.Max(value => Math.Abs(value));
        if (peak > 0.00001f)
        {
            var gain = definition.Peak / peak;
            for (var index = 0; index < output.Length; index++) output[index] *= gain;
        }
        var fadeCount = Math.Min(output.Length, (int)Math.Round(definition.FadeSeconds * TargetRate));
        for (var index = 0; index < fadeCount; index++)
            output[output.Length - fadeCount + index] *= 1f - index / (float)Math.Max(1, fadeCount - 1);
        var pcm = new short[output.Length];
        for (var index = 0; index < pcm.Length; index++)
            pcm[index] = (short)Math.Clamp((int)Math.Round(output[index] * 32767f), short.MinValue, short.MaxValue);
        return pcm;
    }

    private static byte[] EncodeTpa(short[] samples)
    {
        ReadOnlySpan<int> stepTable =
        [
            7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
            73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,
            449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,
            2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,
            7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,
            24623,27086,29794,32767
        ];
        ReadOnlySpan<int> indexTable = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8];
        var payload = new byte[(samples.Length - 1 + 1) / 2];
        var predictor = (int)samples[0]; var stepIndex = 0;
        for (var sampleIndex = 1; sampleIndex < samples.Length; sampleIndex++)
        {
            var step = stepTable[stepIndex]; var delta = samples[sampleIndex] - predictor; var code = 0;
            if (delta < 0) { code = 8; delta = -delta; }
            var difference = step >> 3;
            if (delta >= step) { code |= 4; delta -= step; difference += step; }
            if (delta >= (step >> 1)) { code |= 2; delta -= step >> 1; difference += step >> 1; }
            if (delta >= (step >> 2)) { code |= 1; difference += step >> 2; }
            predictor = Math.Clamp(predictor + ((code & 8) != 0 ? -difference : difference), short.MinValue, short.MaxValue);
            stepIndex = Math.Clamp(stepIndex + indexTable[code], 0, 88);
            var packed = sampleIndex - 1;
            if ((packed & 1) == 0) payload[packed >> 1] = (byte)code;
            else payload[packed >> 1] |= (byte)(code << 4);
        }
        using var stream = new MemoryStream(16 + payload.Length);
        using var writer = new BinaryWriter(stream);
        writer.Write("TPA1"u8); writer.Write(TargetRate); writer.Write(samples.Length);
        writer.Write(samples[0]); writer.Write((byte)0); writer.Write((byte)0); writer.Write(payload);
        return stream.ToArray();
    }
}
