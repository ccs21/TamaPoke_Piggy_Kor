using System.IO.Ports;
using System.Management;
using System.Text.RegularExpressions;

namespace TamaPoke.Flasher.Services;

public sealed partial class DeviceDetector
{
    public IReadOnlyList<SerialDeviceInfo> FindAll()
    {
        var known = new Dictionary<string, SerialDeviceInfo>(StringComparer.OrdinalIgnoreCase);
        try
        {
            using var searcher = new ManagementObjectSearcher(
                "SELECT Name, DeviceID, PNPDeviceID FROM Win32_PnPEntity WHERE Name LIKE '%(COM%'");
            foreach (ManagementObject device in searcher.Get())
            {
                var name = Convert.ToString(device["Name"]) ?? "";
                var match = ComPortRegex().Match(name);
                if (!match.Success) continue;
                var port = match.Groups[1].Value.ToUpperInvariant();
                var pnpId = Convert.ToString(device["PNPDeviceID"])
                            ?? Convert.ToString(device["DeviceID"])
                            ?? "";
                known[port] = new SerialDeviceInfo(port, name, pnpId, LooksLikeEspressif(name, pnpId));
            }
        }
        catch (ManagementException)
        {
            // WMI를 사용할 수 없어도 COM 포트 수동 선택은 지원한다.
        }

        foreach (var port in SerialPort.GetPortNames())
        {
            var normalized = port.ToUpperInvariant();
            known.TryAdd(normalized, new SerialDeviceInfo(normalized, "직렬 장치", "", false));
        }

        return known.Values
            .OrderByDescending(device => device.IsEspressif)
            .ThenBy(device => ComNumber(device.PortName))
            .ToArray();
    }

    private static bool LooksLikeEspressif(string friendlyName, string pnpId)
    {
        var text = $"{friendlyName} {pnpId}".ToUpperInvariant();
        return text.Contains("VID_303A") || text.Contains("ESP32") ||
               text.Contains("ESPRESSIF") || text.Contains("USB JTAG/SERIAL");
    }

    private static int ComNumber(string portName) =>
        int.TryParse(new string(portName.Where(char.IsDigit).ToArray()), out var number)
            ? number
            : int.MaxValue;

    [GeneratedRegex(@"\((COM\d+)\)", RegexOptions.IgnoreCase)]
    private static partial Regex ComPortRegex();
}
