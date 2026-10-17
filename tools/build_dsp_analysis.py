"""Build a local dsptool variant with explicitly simulated peripheral registers.

Leaves both source checkouts unchanged. Generated wrapper/binary stay ignored
under research/analysis. The linked DSP interpreter is GPL-3.0.
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main():
    upstream = ROOT / "research/CIRCUITS-HACKS-RC/tools/dsptool/dsptool.cpp"
    source = upstream.read_text()
    if source.count("static int cmdRun(") != 1 or source.count("PeripheralsNop px, py;") != 1:
        raise ValueError("Unexpected dsptool source")
    # Use a supported interpreter pairing. These are register fixtures here,
    # not a claim to model the Rhythm board's complete peripheral behavior.
    source = source.replace("PeripheralsNop px, py;", "Peripherals56362 px; Peripherals56367 py;", 1)
    marker = 'if (c == "P" || c == "X" || c == "Y") {'
    if source.count(marker) != 1:
        raise ValueError("Unexpected command parser")
    source = source.replace(marker, '''if (c == "BRIDGE") {
            std::string address; s >> address;
            mem.setExternalMemory(strtoul(address.c_str(), nullptr, 16), true);
        } else if (c == "HOSTC") {
            std::string value; s >> value;
            px.getPortC().hostWrite(strtoul(value.c_str(), nullptr, 16));
        } else if (c == "SHI") {
            std::string value; s >> value;
            px.getSHI().exchange(strtoul(value.c_str(), nullptr, 16));
        } else if (c == "READIOX" || c == "READIOY") {
            std::string address; s >> address;
            IPeripherals& fixture = c == "READIOX" ? static_cast<IPeripherals&>(px) : static_cast<IPeripherals&>(py);
            TWord addr = strtoul(address.c_str(), nullptr, 16);
            printf("IO%c %06x: %06x\\n", c.back(), addr, fixture.read(addr, Instruction::Movep_ppea));
        } else if (c == "IOX" || c == "IOY") {
            std::string address, value; s >> address >> value;
            IPeripherals& fixture = c == "IOX" ? static_cast<IPeripherals&>(px) : static_cast<IPeripherals&>(py);
            fixture.write(strtoul(address.c_str(), nullptr, 16), strtoul(value.c_str(), nullptr, 16));
        } else ''' + marker, 1)
    # The old interpreter arrays are much larger than the real sampler core.
    # CORE1 enables explicit checks independent of release/debug library builds.
    profile = '''static bool g_core1Profile = false;
static bool core1Mapped(EMemArea a, TWord addr) {
    if (addr >= 0x30000 && addr < 0x38000) return true;
    if (a != MemArea_P && addr >= 0xfff000) return true;
    return addr < (a == MemArea_P ? 0x1000u : a == MemArea_X ? 0x3000u : 0x2000u);
}
static void core1Check(EMemArea a, TWord addr) {
    if (g_core1Profile && !core1Mapped(a, addr)) {
        fprintf(stderr, "CORE1 RAM VIOLATION %c:%06x\\n", a == MemArea_P ? 'P' : a == MemArea_X ? 'X' : 'Y', addr);
        std::exit(2);
    }
}
'''
    source = source.replace('static TWord g_curPC', profile + '\nstatic TWord g_curPC', 1)
    source = source.replace('if (!g_watchOut || !g_watchMax) return;',
                            'core1Check(a, addr); if (!g_watchOut || !g_watchMax) return;', 1)
    source = source.replace('if (c == "BRIDGE") {', '''if (c == "CORE1") {
            g_core1Profile = true;
            std::fill(wX.begin(), wX.end(), 1); std::fill(wY.begin(), wY.end(), 1);
            watching = true;
            printf("CORE1 bounds enabled\\n");
        } else if (c == "BRIDGE") {''', 1)
    source = source.replace('while (s >> w) mem.set(area(c[0]), addr++, strtoul(w.c_str(), nullptr, 16));',
                            'while (s >> w) { core1Check(area(c[0]), addr); mem.set(area(c[0]), addr++, strtoul(w.c_str(), nullptr, 16)); }', 1)
    source = source.replace('mem.set(MemArea_P, addr++, (d[i] << 16) | (d[i+1] << 8) | d[i+2]);',
                            '{ core1Check(MemArea_P, addr); mem.set(MemArea_P, addr++, (d[i] << 16) | (d[i+1] << 8) | d[i+2]); }', 1)
    source = source.replace('while (dsp.getPC().toWord() != stop && n < maxi) {',
                            'while (dsp.getPC().toWord() != stop && n < maxi) { core1Check(MemArea_P, dsp.getPC().toWord());', 1)
    source = source.replace('uint64_t n = 0; const uint64_t i0',
                            'std::vector<uint8_t> profileP(0x80000, 1); if (g_core1Profile) g_memWatch[MemArea_P] = profileP.data();\n            uint64_t n = 0; const uint64_t i0', 1)
    source = source.replace('g_memWatch[MemArea_X] = g_memWatch[MemArea_Y] = nullptr;',
                            'g_memWatch[MemArea_P] = g_memWatch[MemArea_X] = g_memWatch[MemArea_Y] = nullptr;', 1)
    output = ROOT / "research/analysis"
    output.mkdir(parents=True, exist_ok=True)
    wrapper = output / "dsptool-io.cpp"
    wrapper.write_text(source)
    dsp = ROOT / "research/dsp56300"
    libraries = [dsp / "build/source" / name for name in (
        "dsp56kEmu/libdsp56kEmu.a", "dsp56kBase/libdsp56kBase.a", "asmjit/libasmjit.a")]
    if any(not path.exists() for path in libraries):
        raise SystemExit("Build the pinned DSP libraries first; see DSP-CONSUMER-FINDINGS.md")
    command = ["clang++", "-O2", "-std=c++17", "-DDSP56K_FORCE_INTERPRETER=1",
               "-Wno-nontrivial-memcall", "-Wno-shift-negative-value",
               "-I", str(dsp / "source"), "-I", str(dsp / "source/asmjit/src"),
               str(wrapper), *(str(path) for path in libraries), "-lpthread", "-ldl",
               "-o", str(output / "dsptool-io")]
    subprocess.run(command, check=True)
    print(output / "dsptool-io")


if __name__ == "__main__":
    main()
