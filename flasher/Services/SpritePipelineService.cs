using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Xml.Linq;

namespace TamaPoke.Flasher.Services;

public sealed record SpriteBuildResult(string DataDirectory, string ManifestHash, long Bytes, bool Reused);

public sealed class SpritePipelineService
{
    private const string BaseUrl = "https://raw.githubusercontent.com/PMDCollab/SpriteCollab/master";
    private const string PipelineVersion = "tpk3-cropped-v1-csharp-20260824";
    private const byte Transparent = 0xFF;
    private const int Cell = 40;
    private static readonly HttpClient Http = CreateClient();
    private static readonly (byte Id, string Name, int Row)[] Actions =
    [
        (0, "Idle", 0), (1, "Walk", 6), (2, "Walk", 2), (3, "Sleep", 0),
        (4, "Eat", 0), (5, "Hurt", 0), (7, "Pose", 0), (8, "Hop", 0),
        (9, "Nod", 0), (10, "DeepBreath", 0), (11, "Sit", 0),
        (12, "Attack", 6), (13, "Attack", 2),
    ];

    private readonly string _root = AppStoragePaths.Under(
        "Cache", "sprites", PipelineVersion);

    public async Task<SpriteBuildResult> PrepareAsync(
        Action<string> log, IProgress<ProgressUpdate>? progress, CancellationToken cancellationToken)
    {
        Directory.CreateDirectory(_root);
        await DownloadFileAsync($"{BaseUrl}/credit_names.txt", Path.Combine(_root, "credit_names.txt"), cancellationToken);
        await DownloadFileAsync($"{BaseUrl}/tracker.json", Path.Combine(_root, "tracker.json"), cancellationToken);
        await DownloadFileAsync($"{BaseUrl}/LICENSE.md", Path.Combine(_root, "SPRITECOLLAB-LICENSE.md"), cancellationToken);
        var output = Path.Combine(_root, "data", "mons");
        var manifestPath = Path.Combine(output, "manifest.json");
        if (IsComplete(output, manifestPath))
        {
            var manifestHash = await HashFileAsync(manifestPath, cancellationToken);
            var bytes = Directory.EnumerateFiles(output).Sum(path => new FileInfo(path).Length);
            log($"스프라이트 캐시 재사용: {bytes / 1048576d:0.00}MiB");
            progress?.Report(new ProgressUpdate(48, "스프라이트 준비 완료", "저장된 최적화 스프라이트를 재사용합니다."));
            return new(output, manifestHash, bytes, true);
        }

        Directory.CreateDirectory(output);
        var rawRoot = Path.Combine(_root, "download");
        Directory.CreateDirectory(rawRoot);
        log("PMDCollab SpriteCollab에서 1세대 스프라이트를 내려받습니다.");
        log("첫 설치는 시간이 걸리며, 받은 원본과 가공 결과는 다음 설치에 재사용됩니다.");
        var thumbs = new byte[151][];
        var entries = new SpriteManifestEntry[302];
        var completed = 0;
        using var gate = new SemaphoreSlim(5);
        var tasks = new List<Task>(302);
        for (var dex = 1; dex <= 151; dex++)
        {
            foreach (var shiny in new[] { false, true })
            {
                var capturedDex = dex;
                var capturedShiny = shiny;
                tasks.Add(Task.Run(async () =>
                {
                    await gate.WaitAsync(cancellationToken);
                    try
                    {
                        var packed = await PackOneAsync(capturedDex, capturedShiny, rawRoot, cancellationToken);
                        var name = $"p{(capturedShiny ? "s" : "")}{capturedDex:000}.bin";
                        var destination = Path.Combine(output, name);
                        await WriteAtomicAsync(destination, packed.Bytes, cancellationToken);
                        entries[(capturedDex - 1) + (capturedShiny ? 151 : 0)] =
                            new(name, packed.Bytes.Length, Convert.ToHexString(SHA256.HashData(packed.Bytes)).ToLowerInvariant());
                        if (!capturedShiny) thumbs[capturedDex - 1] = BuildThumb(packed.Idle);
                        var done = Interlocked.Increment(ref completed);
                        var percent = 19 + done / 302d * 28d;
                        progress?.Report(new ProgressUpdate(percent, "스프라이트 다운로드 및 최적화",
                            $"{done}/302 · #{capturedDex:000}{(capturedShiny ? " 이로치" : "")}"));
                        if (done % 20 == 0 || done == 302) log($"스프라이트 처리: {done}/302");
                    }
                    finally { gate.Release(); }
                }, cancellationToken));
            }
        }
        await Task.WhenAll(tasks);

        var thumbsBytes = BuildThumbFile(thumbs);
        await WriteAtomicAsync(Path.Combine(output, "thumbs.bin"), thumbsBytes, cancellationToken);
        var allEntries = entries.OrderBy(entry => entry.Name, StringComparer.Ordinal).ToList();
        allEntries.Add(new("thumbs.bin", thumbsBytes.Length,
            Convert.ToHexString(SHA256.HashData(thumbsBytes)).ToLowerInvariant()));
        var manifest = new
        {
            format = "TPK3-cropped-v1",
            source = "PMDCollab/SpriteCollab",
            sourceUrl = "https://github.com/PMDCollab/SpriteCollab",
            sourceLicense = "CC BY-NC 4.0",
            pipeline = PipelineVersion,
            files = allEntries.Select(entry => new { path = $"mons/{entry.Name}", bytes = entry.Bytes, sha256 = entry.Sha256 }),
        };
        var manifestBytes = JsonSerializer.SerializeToUtf8Bytes(manifest, new JsonSerializerOptions { WriteIndented = true });
        await WriteAtomicAsync(manifestPath, manifestBytes, cancellationToken);
        var totalBytes = allEntries.Sum(entry => (long)entry.Bytes) + manifestBytes.Length;
        var hash = Convert.ToHexString(SHA256.HashData(manifestBytes)).ToLowerInvariant();
        log($"스프라이트 최적화 완료: {totalBytes / 1048576d:0.00}MiB");
        return new(output, hash, totalBytes, false);
    }

    private async Task<PackedPokemon> PackOneAsync(int dex, bool shiny, string rawRoot, CancellationToken token)
    {
        var variant = shiny ? $"{dex:0000}/0000/0001" : $"{dex:0000}";
        var local = Path.Combine(rawRoot, $"{dex:0000}{(shiny ? "s" : "")}");
        var xmlPath = Path.Combine(local, "AnimData.xml");
        await DownloadFileAsync($"{BaseUrl}/sprite/{variant}/AnimData.xml", xmlPath, token);
        var animations = LoadAnimations(xmlPath);
        var palette = new List<ushort>();
        var paletteMap = new Dictionary<ushort, byte>();
        var packedActions = new List<PackedAction>();

        foreach (var requested in Actions)
        {
            if (!animations.TryGetValue(requested.Name, out var animation) || animation is null) continue;
            var pngPath = Path.Combine(local, $"{animation.SourceName}-Anim.png");
            await DownloadFileAsync($"{BaseUrl}/sprite/{variant}/{Uri.EscapeDataString(animation.SourceName)}-Anim.png", pngPath, token);
            var image = ReadPng(pngPath);
            var rows = image.Height / animation.Height;
            var row = rows > requested.Row ? requested.Row : 0;
            var frameCount = Math.Min(24, Math.Min(animation.Durations.Count, image.Width / animation.Width));
            if (frameCount <= 0) continue;
            var frames = new List<byte[]>(frameCount);
            for (var frame = 0; frame < frameCount; frame++)
                frames.Add(IndexFrame(image, frame * animation.Width, row * animation.Height,
                    animation.Width, animation.Height, palette, paletteMap));
            var durations = animation.Durations.Take(frameCount)
                .Select(value => (ushort)Math.Clamp(Math.Max(70, (int)Math.Round(value * 1000d / 60d * 1.4d)), 1, ushort.MaxValue))
                .ToArray();
            packedActions.Add(new(requested.Id, (byte)animation.Width, (byte)animation.Height, durations, frames));
        }
        var idle = packedActions.FirstOrDefault(action => action.Id == 0)
                   ?? throw new InvalidDataException($"#{dex:000}{(shiny ? " shiny" : "")}: Idle 애니메이션이 없습니다.");
        var bytes = EncodeTpk3(palette, packedActions);
        return new(bytes, new(idle.Width, idle.Height, palette.ToArray(), idle.Frames[0]));
    }

    private static Dictionary<string, AnimationInfo?> LoadAnimations(string path)
    {
        var result = new Dictionary<string, AnimationInfo?>(StringComparer.Ordinal);
        var aliases = new Dictionary<string, string>(StringComparer.Ordinal);
        var root = XDocument.Load(path).Root?.Element("Anims")
                   ?? throw new InvalidDataException("AnimData.xml에 Anims가 없습니다.");
        foreach (var element in root.Elements())
        {
            var name = element.Element("Name")?.Value;
            if (string.IsNullOrWhiteSpace(name)) continue;
            var widthText = element.Element("FrameWidth")?.Value;
            if (widthText is null)
            {
                var copy = element.Element("CopyOf")?.Value;
                if (!string.IsNullOrWhiteSpace(copy)) aliases[name] = copy;
                continue;
            }
            var durations = element.Element("Durations")?.Elements().Select(item => int.Parse(item.Value)).ToArray() ?? [];
            result[name] = new(int.Parse(widthText), int.Parse(element.Element("FrameHeight")!.Value), durations, name);
        }
        foreach (var alias in aliases)
            result[alias.Key] = result.TryGetValue(alias.Value, out var source) ? source : null;
        return result;
    }

    private static PngImage ReadPng(string path)
    {
        using var stream = File.OpenRead(path);
        var decoder = new PngBitmapDecoder(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
        BitmapSource source = decoder.Frames[0];
        if (source.Format != PixelFormats.Bgra32)
        {
            var converted = new FormatConvertedBitmap(source, PixelFormats.Bgra32, null, 0);
            converted.Freeze();
            source = converted;
        }
        var stride = source.PixelWidth * 4;
        var pixels = new byte[stride * source.PixelHeight];
        source.CopyPixels(pixels, stride, 0);
        return new(source.PixelWidth, source.PixelHeight, stride, pixels);
    }

    private static byte[] IndexFrame(PngImage image, int x, int y, int width, int height,
        List<ushort> palette, Dictionary<ushort, byte> map)
    {
        var output = new byte[width * height];
        for (var row = 0; row < height; row++)
        for (var column = 0; column < width; column++)
        {
            var source = (y + row) * image.Stride + (x + column) * 4;
            var alpha = image.Pixels[source + 3];
            if (alpha < 128) { output[row * width + column] = Transparent; continue; }
            var color = Rgb565(image.Pixels[source + 2], image.Pixels[source + 1], image.Pixels[source]);
            if (!map.TryGetValue(color, out var index))
            {
                if (palette.Count < 255)
                {
                    index = (byte)palette.Count;
                    palette.Add(color);
                }
                else index = FindNearest(palette, color);
                map[color] = index;
            }
            output[row * width + column] = index;
        }
        return output;
    }

    private static byte FindNearest(List<ushort> palette, ushort color)
    {
        var tr = (color >> 11) & 31; var tg = (color >> 5) & 63; var tb = color & 31;
        var best = 0; var bestDistance = int.MaxValue;
        for (var index = 0; index < palette.Count; index++)
        {
            var item = palette[index];
            var dr = ((item >> 11) & 31) - tr; var dg = ((item >> 5) & 63) - tg; var db = (item & 31) - tb;
            var distance = dr * dr + dg * dg + db * db;
            if (distance >= bestDistance) continue;
            bestDistance = distance; best = index;
        }
        return (byte)best;
    }

    private static byte[] EncodeTpk3(IReadOnlyList<ushort> palette, IReadOnlyList<PackedAction> actions)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream);
        writer.Write("TPK3"u8);
        writer.Write((byte)actions.Count);
        writer.Write((ushort)palette.Count);
        foreach (var color in palette) writer.Write(color);
        foreach (var action in actions)
        {
            writer.Write(action.Id); writer.Write(action.Width); writer.Write(action.Height); writer.Write((byte)action.Frames.Count);
            for (var index = 0; index < action.Frames.Count; index++)
            {
                var crop = Crop(action.Frames[index], action.Width, action.Height);
                writer.Write(action.Durations[index]); writer.Write(crop.Left); writer.Write(crop.Top);
                writer.Write(crop.Width); writer.Write(crop.Height); writer.Write(crop.Pixels);
            }
        }
        return stream.ToArray();
    }

    private static CroppedFrame Crop(byte[] frame, int width, int height)
    {
        var left = width; var top = height; var right = -1; var bottom = -1;
        for (var y = 0; y < height; y++)
        for (var x = 0; x < width; x++)
            if (frame[y * width + x] != Transparent)
            { left = Math.Min(left, x); right = Math.Max(right, x); top = Math.Min(top, y); bottom = Math.Max(bottom, y); }
        if (right < left) return new(0, 0, 0, 0, []);
        var cropWidth = right - left + 1; var cropHeight = bottom - top + 1;
        var pixels = new byte[cropWidth * cropHeight];
        for (var row = 0; row < cropHeight; row++)
            Buffer.BlockCopy(frame, (top + row) * width + left, pixels, row * cropWidth, cropWidth);
        return new((byte)left, (byte)top, (byte)cropWidth, (byte)cropHeight, pixels);
    }

    private static byte[] BuildThumb(IdleFrame idle)
    {
        var scale = Math.Min(1d, Math.Min(Cell / (double)idle.Width, Cell / (double)idle.Height));
        var width = Math.Max(1, (int)Math.Round(idle.Width * scale));
        var height = Math.Max(1, (int)Math.Round(idle.Height * scale));
        var palette = new List<ushort>(); var map = new Dictionary<ushort, byte>();
        var pixels = new byte[width * height];
        for (var y = 0; y < height; y++)
        for (var x = 0; x < width; x++)
        {
            var sx = scale < 1 ? Math.Min(idle.Width - 1, (int)(x / scale)) : x;
            var sy = scale < 1 ? Math.Min(idle.Height - 1, (int)(y / scale)) : y;
            var old = idle.Pixels[sy * idle.Width + sx];
            if (old == Transparent) { pixels[y * width + x] = Transparent; continue; }
            var color = idle.Palette[old];
            if (!map.TryGetValue(color, out var index)) { index = (byte)palette.Count; palette.Add(color); map[color] = index; }
            pixels[y * width + x] = index;
        }
        using var stream = new MemoryStream(); using var writer = new BinaryWriter(stream);
        writer.Write((byte)width); writer.Write((byte)height); writer.Write((byte)palette.Count);
        foreach (var color in palette) writer.Write(color); writer.Write(pixels);
        return stream.ToArray();
    }

    private static byte[] BuildThumbFile(IReadOnlyList<byte[]> blobs)
    {
        if (blobs.Any(blob => blob is null)) throw new InvalidDataException("일부 도감 썸네일이 생성되지 않았습니다.");
        using var stream = new MemoryStream(); using var writer = new BinaryWriter(stream);
        writer.Write("TPTH"u8); writer.Write((ushort)blobs.Count);
        var offset = 4 + 2 + 4 * blobs.Count;
        foreach (var blob in blobs) { writer.Write(offset); offset += blob.Length; }
        foreach (var blob in blobs) writer.Write(blob);
        return stream.ToArray();
    }

    private static bool IsComplete(string output, string manifest) =>
        File.Exists(manifest) && Directory.EnumerateFiles(output, "p*.bin").Count() == 302 &&
        File.Exists(Path.Combine(output, "thumbs.bin"));

    private static async Task DownloadFileAsync(string url, string destination, CancellationToken token)
    {
        if (File.Exists(destination) && new FileInfo(destination).Length > 0) return;
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        using var response = await Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, token);
        response.EnsureSuccessStatusCode();
        var temporary = destination + ".part";
        await using (var source = await response.Content.ReadAsStreamAsync(token))
        await using (var output = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None, 65536, true))
            await source.CopyToAsync(output, token);
        File.Move(temporary, destination, true);
    }

    private static async Task WriteAtomicAsync(string destination, byte[] bytes, CancellationToken token)
    {
        var temporary = destination + ".new";
        await File.WriteAllBytesAsync(temporary, bytes, token);
        File.Move(temporary, destination, true);
    }

    private static async Task<string> HashFileAsync(string path, CancellationToken token)
    {
        await using var stream = File.OpenRead(path);
        return Convert.ToHexString(await SHA256.HashDataAsync(stream, token)).ToLowerInvariant();
    }

    private static HttpClient CreateClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromSeconds(45) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("TamaPoke-Korean-Flasher/2.0 (+https://github.com/ccs21/TamaPoke_Piggy_Kor)");
        return client;
    }

    private static ushort Rgb565(byte r, byte g, byte b) => (ushort)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    private sealed record AnimationInfo(int Width, int Height, IReadOnlyList<int> Durations, string SourceName);
    private sealed record PngImage(int Width, int Height, int Stride, byte[] Pixels);
    private sealed record PackedAction(byte Id, byte Width, byte Height, ushort[] Durations, List<byte[]> Frames);
    private sealed record IdleFrame(int Width, int Height, ushort[] Palette, byte[] Pixels);
    private sealed record PackedPokemon(byte[] Bytes, IdleFrame Idle);
    private sealed record CroppedFrame(byte Left, byte Top, byte Width, byte Height, byte[] Pixels);
    private sealed record SpriteManifestEntry(string Name, int Bytes, string Sha256);
}
