using System.Globalization;

namespace Translator.Core;

/// <summary>
/// The optional <c>runtime.address_map</c>: the hand-written runtime is written against one game
/// version's guest addresses (the canonical one), and this table moves each of them to the version
/// being translated. Every address the translator reads out of runtime C++ goes through it, and an
/// address it does not cover is an error rather than a pass-through: a registration landing on the
/// wrong function is worse than a failed build. Without a map the scanned addresses are used as
/// written.
/// </summary>
/// <remarks>
/// File format, one range per line: <c>canonical_start canonical_end target_start</c> in hex
/// (<c>0x</c> optional, end exclusive), separated by tabs or spaces; <c>#</c> starts a comment.
/// Ranges must be sorted, may not overlap, and may not land on the same target addresses.
/// </remarks>
public sealed class RuntimeAddressMap
{
    private readonly uint[] _starts;
    private readonly uint[] _ends;
    private readonly uint[] _targets;

    private RuntimeAddressMap(string sourcePath, uint[] starts, uint[] ends, uint[] targets)
    {
        SourcePath = sourcePath;
        _starts = starts;
        _ends = ends;
        _targets = targets;
    }

    public string SourcePath { get; }

    public int RangeCount => _starts.Length;

    public static RuntimeAddressMap Load(string path)
    {
        var fullPath = Path.GetFullPath(path);
        if (!File.Exists(fullPath))
            throw new FileNotFoundException("runtime.address_map not found.", fullPath);
        var ranges = new List<(uint Start, uint End, uint Target)>();
        var lineNumber = 0;
        foreach (var raw in File.ReadLines(fullPath))
        {
            lineNumber++;
            var hash = raw.IndexOf('#');
            var line = (hash >= 0 ? raw[..hash] : raw).Trim();
            if (line.Length == 0)
                continue;
            var fields = line.Split((char[])[' ', '\t'], StringSplitOptions.RemoveEmptyEntries);
            if (fields.Length != 3 ||
                !GuestTargetParser.TryParseHexAddress(fields[0], out var start) ||
                !GuestTargetParser.TryParseHexAddress(fields[1], out var end) ||
                !GuestTargetParser.TryParseHexAddress(fields[2], out var target))
            {
                throw new InvalidDataException(
                    $"{fullPath}:{lineNumber.ToString(CultureInfo.InvariantCulture)}: expected " +
                    "'canonical_start canonical_end target_start' in hex.");
            }
            ranges.Add((start, end, target));
        }
        return FromRanges(ranges, fullPath);
    }

    public static RuntimeAddressMap FromRanges(
        IEnumerable<(uint Start, uint End, uint Target)> ranges, string sourcePath = "<address map>")
    {
        var list = ranges.ToList();
        for (var index = 0; index < list.Count; index++)
        {
            var (start, end, target) = list[index];
            if (end <= start)
                throw new InvalidDataException($"{sourcePath}: empty range 0x{start:X8}-0x{end:X8}.");
            if ((ulong)target + (end - start) > 0x1_0000_0000UL)
                throw new InvalidDataException($"{sourcePath}: range 0x{start:X8} maps past 0xFFFFFFFF.");
            if (index > 0 && start < list[index - 1].End)
                throw new InvalidDataException(
                    $"{sourcePath}: ranges must be sorted and disjoint (0x{start:X8} after 0x{list[index - 1].Start:X8}).");
        }
        var byTarget = list.OrderBy(static range => range.Target).ToList();
        for (var index = 1; index < byTarget.Count; index++)
        {
            var previous = byTarget[index - 1];
            if (byTarget[index].Target < previous.Target + (previous.End - previous.Start))
                throw new InvalidDataException(
                    $"{sourcePath}: 0x{previous.Start:X8} and 0x{byTarget[index].Start:X8} map onto the same addresses.");
        }
        return new RuntimeAddressMap(
            sourcePath,
            list.Select(static range => range.Start).ToArray(),
            list.Select(static range => range.End).ToArray(),
            list.Select(static range => range.Target).ToArray());
    }

    public bool TryMap(uint canonical, out uint target)
    {
        var index = Array.BinarySearch(_starts, canonical);
        if (index < 0)
            index = ~index - 1;
        if (index >= 0 && canonical < _ends[index])
        {
            target = _targets[index] + (canonical - _starts[index]);
            return true;
        }
        target = 0;
        return false;
    }

    /// <summary>The target address, or an exception naming where the unmapped address came from.</summary>
    public uint Map(uint canonical, string context) =>
        TryMap(canonical, out var target)
            ? target
            : throw new InvalidDataException(
                $"Runtime address 0x{canonical:X8} ({context}) has no mapping in {SourcePath}. " +
                "Map it (an override for this game version) or remove its use.");
}
