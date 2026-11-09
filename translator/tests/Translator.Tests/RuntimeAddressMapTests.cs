using Translator.Core;
using Translator.Core.Analysis;
using Translator.Core.CodeGen;
using Xunit;

namespace Translator.Tests;

public sealed class RuntimeAddressMapTests
{
    // Every registration kind the index reads, written against canonical addresses.
    private const string Registrations = """
        extern "C" void Typed(float value, uint32_t count) { (void)value; (void)count; }
        extern "C" void Direct(CpuContext* ctx) { ctx->gpr[3] = ctx->gpr[4]; }
        REGISTER_NATIVE_FUNCTION(0x80000010, Direct);
        REGISTER_NATIVE_FUNCTION_AS(0x80000020, Aliased, "alias");
        REGISTER_TRANSLATED_FUNCTION(0x80000030, Translated);
        PPC_NATIVE_OVERRIDE_VOID(80001234, Typed, (float value, uint32_t count), (value, count));
        GX_FATAL_STUB(80000050, "Fatal")
        """;

    [Fact]
    public void MapMovesRegistrationsEffectsAndStubAbisToTheTargetVersion()
    {
        using var fixture = new SourceFixture(Registrations);
        var map = RuntimeAddressMap.FromRanges([(0x80000000u, 0x80001000u, 0x80000100u), (0x80001234u, 0x80001238u, 0x80005000u)]);

        var index = RuntimeNativeIndexBuilder.Build(fixture.Directory, map);

        Assert.Equal(
            new uint[] { 0x80000110u, 0x80000120u, 0x80000130u, 0x80000150u, 0x80005000u },
            index.Registrations.Select(static entry => entry.Address).Order().ToArray());
        Assert.Contains(index.Registrations, static entry =>
            entry.Address == 0x80005000u && entry.Symbol == "Typed");
        Assert.Contains(index.Registrations, static entry =>
            entry.Address == 0x80000110u && entry.Symbol == "Direct" && entry.ExcludesBaseTranslation);
        Assert.Equal(new uint[] { 0x80005000u }, index.VoidStubAbis.Select(static entry => entry.Address).ToArray());
        var effects = index.ToGuestEffectSet();
        Assert.True(effects.Contracts.ContainsKey(0x80005000u));
        Assert.True(effects.Contracts.ContainsKey(0x80000110u));
        Assert.DoesNotContain(0x80001234u, effects.Contracts.Keys);
        Assert.DoesNotContain(0x80000010u, effects.Contracts.Keys);

        var provider = RuntimeNativeFunctionAbiProvider.FromIndex(index, new HashSet<uint> { 0x80005000u });
        Assert.True(provider.TryGetGuestFunctionAbi("func_80005000", out var abi));
        Assert.Contains("f1", abi.ArgumentRegisters);
        Assert.False(provider.TryGetGuestFunctionAbi("func_80001234", out _));
    }

    [Fact]
    public void WithoutAMapAddressesAreUsedAsWritten()
    {
        using var fixture = new SourceFixture(Registrations);

        var plain = RuntimeNativeIndexBuilder.Build(fixture.Directory);
        var explicitNull = RuntimeNativeIndexBuilder.Build(fixture.Directory, null);

        Assert.Equal(
            new uint[] { 0x80000010u, 0x80000020u, 0x80000030u, 0x80000050u, 0x80001234u },
            plain.Registrations.Select(static entry => entry.Address).Order().ToArray());
        Assert.Equal(plain.Registrations, explicitNull.Registrations);
        Assert.Equal(plain.VoidStubAbis.Select(static e => e.Address), explicitNull.VoidStubAbis.Select(static e => e.Address));
        Assert.Equal(plain.Effects.Select(static e => e.Address), explicitNull.Effects.Select(static e => e.Address));
    }

    [Fact]
    public void AnUnmappedRuntimeAddressIsAnErrorNamingItsSource()
    {
        using var fixture = new SourceFixture("""
            REGISTER_NATIVE_FUNCTION(0x80000010, Direct);
            PPC_NATIVE_OVERRIDE_VOID(80009999, Unmapped, (void), ());
            """);
        var map = RuntimeAddressMap.FromRanges([(0x80000000u, 0x80001000u, 0x80000000u)], "test-map.tsv");

        var error = Assert.Throws<InvalidDataException>(() => RuntimeNativeIndexBuilder.Build(fixture.Directory, map));
        Assert.Contains("0x80009999", error.Message);
        Assert.Contains("fixture.cpp", error.Message);
        Assert.Contains("test-map.tsv", error.Message);
        Assert.Throws<InvalidDataException>(() => RuntimeNativeGuestEffectAnalyzer.AnalyzeDirectory(fixture.Directory, map));
    }

    [Fact]
    public void DisabledRegistrationsNeedNoMapping()
    {
        using var fixture = new SourceFixture("""
            REGISTER_NATIVE_FUNCTION(0x80000010, Direct);
            #if 0
            PPC_NATIVE_OVERRIDE_VOID(80009999, OnlyOnAnotherVersion, (void), ());
            #endif
            // REGISTER_NATIVE_FUNCTION(0x80009998, CommentedOut);
            """);
        var map = RuntimeAddressMap.FromRanges([(0x80000000u, 0x80001000u, 0x80000400u)]);

        var index = RuntimeNativeIndexBuilder.Build(fixture.Directory, map);

        Assert.Equal(0x80000410u, Assert.Single(index.Registrations).Address);
    }

    [Fact]
    public void LoadReadsRangesAndRejectsOverlapsAndCollisions()
    {
        using var fixture = new SourceFixture(string.Empty);
        var path = Path.Combine(fixture.Directory, "addr_map.tsv");
        File.WriteAllText(path, """
            # canonical_start canonical_end target_start
            0x80000000	0x80001000	0x80000000
            80002000 80002010 80003000   # a moved range
            """);

        var map = RuntimeAddressMap.Load(path);
        Assert.Equal(2, map.RangeCount);
        Assert.True(map.TryMap(0x80000FFCu, out var same));
        Assert.Equal(0x80000FFCu, same);
        Assert.True(map.TryMap(0x80002008u, out var moved));
        Assert.Equal(0x80003008u, moved);
        Assert.False(map.TryMap(0x80001000u, out _));
        Assert.False(map.TryMap(0x80002010u, out _));
        Assert.False(map.TryMap(0x7FFFFFFCu, out _));

        Assert.Throws<InvalidDataException>(() => RuntimeAddressMap.FromRanges(
            [(0x80000000u, 0x80001000u, 0x80000000u), (0x80000800u, 0x80002000u, 0x80004000u)]));
        Assert.Throws<InvalidDataException>(() => RuntimeAddressMap.FromRanges(
            [(0x80000000u, 0x80001000u, 0x80000000u), (0x80002000u, 0x80003000u, 0x80000800u)]));
        Assert.Throws<InvalidDataException>(() => RuntimeAddressMap.FromRanges([(0x80000010u, 0x80000010u, 0x80000000u)]));
        File.WriteAllText(path, "0x80000000 0x80001000\n");
        Assert.Throws<InvalidDataException>(() => RuntimeAddressMap.Load(path));
    }

    private sealed class SourceFixture : IDisposable
    {
        public SourceFixture(string source)
        {
            Directory = Path.Combine(Path.GetTempPath(), $"nsmbw-address-map-{Guid.NewGuid():N}");
            System.IO.Directory.CreateDirectory(Directory);
            if (source.Length > 0)
                File.WriteAllText(Path.Combine(Directory, "fixture.cpp"), source);
        }

        public string Directory { get; }

        public void Dispose() => System.IO.Directory.Delete(Directory, recursive: true);
    }
}
