"""Physical DSP56724 sampler-core RAM limits selected by Rhythm's stock loader.

NXP DSP56724PB, Table 2: MS=MSW0=MSW1=1 gives core-1 P=4K, X=12K,
Y=8K words. Shared SRAM is 0x30000..0x37fff. This checks physical placement,
not runtime ownership, clocks, peripheral behavior or real-time feasibility.
"""
import struct

BASE = 0x08010000
LOADER = 0x080662CC
LIMITS = {'P': 0x1000, 'X': 0x3000, 'Y': 0x2000}
SHARED_START, SHARED_END = 0x30000, 0x38000
SOURCE = 'https://www.nxp.com/docs/en/product-brief/DSP56724PB.pdf'


def verify_stock_configuration(image):
    offset = LOADER - BASE
    raw = image[offset:offset + 6 + 5 * 3]
    words = [int.from_bytes(raw[i:i+3], 'big') for i in range(0, len(raw), 3)]
    expected = [64, 0, 0x07F411, 0x0000A9, 0x0AFA67, 0x0AFA75, 0x0AFA76]
    if words != expected:
        raise ValueError('Stock DSP bootstrap configuration changed; re-audit the memory map')
    if struct.unpack_from('<I', image, 0x080315CC - BASE)[0] != LOADER:
        raise ValueError('ARM bootstrap pointer no longer selects the audited DSP loader')
    return {'loader_address': hex(LOADER), 'loader_words': words,
            'omr_set_bits': [7, 21, 22], 'MS': 1, 'MSW0': 1, 'MSW1': 1,
            'limits_exclusive': dict(LIMITS), 'source': SOURCE}


def require_mapped(space, address, words, purpose='DSP segment'):
    if space not in LIMITS or words < 1 or address < 0:
        raise ValueError('Invalid DSP memory range')
    end = address + words
    if end <= LIMITS[space] or SHARED_START <= address < end <= SHARED_END:
        return
    raise ValueError(f'{purpose}: {space}:0x{address:04X}..0x{end-1:04X} is outside '
                     f'sampler core RAM (internal {space} ends at 0x{LIMITS[space]-1:04X}; '
                     'shared RAM is 0x30000..0x37FFF)')


def validate_segments(image, segments):
    configuration = verify_stock_configuration(image)
    for segment in segments:
        require_mapped(segment['space'], segment['address'], segment['words'])
    return configuration
