using System.IO;
using System.Text;

namespace SWGBModern.Core;

/// <summary>
/// Minimal INI reader/writer that preserves section order, comments and
/// unknown keys, so hand-edited settings survive the launcher saving.
/// </summary>
public sealed class IniFile
{
    private readonly List<string> _lines = new();
    public string Path { get; }

    public IniFile(string path)
    {
        Path = path;
        if (File.Exists(path)) _lines.AddRange(File.ReadAllLines(path));
    }

    public static IniFile FromText(string path, string text)
    {
        var ini = new IniFile(path);
        ini._lines.Clear();
        ini._lines.AddRange(text.Replace("\r\n", "\n").Split('\n'));
        return ini;
    }

    private (int start, int end) FindSection(string section)
    {
        int start = -1;
        for (int i = 0; i < _lines.Count; i++)
        {
            string t = _lines[i].Trim();
            if (t.StartsWith('[') && t.EndsWith(']'))
            {
                if (start >= 0) return (start, i);
                if (string.Equals(t[1..^1], section, StringComparison.OrdinalIgnoreCase)) start = i;
            }
        }
        return start >= 0 ? (start, _lines.Count) : (-1, -1);
    }

    public string? Get(string section, string key)
    {
        var (s, e) = FindSection(section);
        if (s < 0) return null;
        for (int i = s + 1; i < e; i++)
        {
            string t = _lines[i].Trim();
            if (t.Length == 0 || t[0] == ';' || t[0] == '#') continue;
            int eq = t.IndexOf('=');
            if (eq > 0 && string.Equals(t[..eq].Trim(), key, StringComparison.OrdinalIgnoreCase))
            {
                string v = t[(eq + 1)..];
                int c = v.IndexOfAny(new[] { ';', '#' });
                if (c >= 0) v = v[..c];
                return v.Trim();
            }
        }
        return null;
    }

    public string GetString(string section, string key, string def) => Get(section, key) ?? def;
    public int GetInt(string section, string key, int def) => int.TryParse(Get(section, key), out var v) ? v : def;
    public double GetDouble(string section, string key, double def) =>
        double.TryParse(Get(section, key), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var v) ? v : def;
    public bool GetBool(string section, string key, bool def)
    {
        string? v = Get(section, key);
        if (string.IsNullOrEmpty(v)) return def;
        return v == "1" || v.StartsWith("t", StringComparison.OrdinalIgnoreCase) || v.StartsWith("y", StringComparison.OrdinalIgnoreCase) ||
               v.Equals("on", StringComparison.OrdinalIgnoreCase);
    }

    public void Set(string section, string key, string value)
    {
        var (s, e) = FindSection(section);
        if (s < 0)
        {
            if (_lines.Count > 0 && _lines[^1].Trim().Length > 0) _lines.Add("");
            _lines.Add($"[{section}]");
            _lines.Add($"{key}={value}");
            return;
        }
        for (int i = s + 1; i < e; i++)
        {
            string t = _lines[i].Trim();
            int eq = t.IndexOf('=');
            if (eq > 0 && t[0] != ';' && string.Equals(t[..eq].Trim(), key, StringComparison.OrdinalIgnoreCase))
            {
                // keep an inline comment if present
                string old = _lines[i];
                int c = old.IndexOf(';', old.IndexOf('='));
                _lines[i] = $"{key}={value}" + (c >= 0 ? "   " + old[c..].Trim() : "");
                return;
            }
        }
        int insert = e;
        while (insert - 1 > s && _lines[insert - 1].Trim().Length == 0) insert--;
        _lines.Insert(insert, $"{key}={value}");
    }

    public void Set(string section, string key, int value) => Set(section, key, value.ToString());
    public void Set(string section, string key, bool value) => Set(section, key, value ? "1" : "0");
    public void Set(string section, string key, double value) => Set(section, key, value.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture));

    public void Save()
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
        File.WriteAllText(Path, string.Join("\r\n", _lines).TrimEnd() + "\r\n", new UTF8Encoding(false));
    }
}
