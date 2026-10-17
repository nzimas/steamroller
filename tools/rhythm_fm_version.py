"""Single source for the current Rhythm FM candidate's identity and FIFO format.

Previous releases' archives under dist/ stay immutable; probes that compare
against them name those versions explicitly.
"""
VERSION = '0.3.1'
NAME = 'Rhythm FM ' + VERSION
STEM = 'rhythm-fm-' + VERSION
INSTALL_MARKER = b'RF0301'
IDENTITY_BUILD = 15745
MEMORY_FAILURE_BUILD = 15713
PREVIOUS = '0.3.0'
# 0.2.x: the stock sampler on all 8 tracks; poundhard is a sound generator
# (Shift + track button 1-8), auditioned on that track's output.
FM_TRACKS = 0
DRUM_TRACKS = 0
OUT_TRACKS = 8         # tracks an audition block can be added to
GEN_FAMILIES = 8
FRAMES = 32
TRACK_SHIFT = 6        # length word: FRAMES | audition track << 6
BLOCK_WORDS = 2 + FRAMES               # sequence, length|track, samples
SLOT_WORDS = 1 + BLOCK_WORDS           # READY first
SLOT_STRIDE = SLOT_WORDS
SLOT_COUNT = 12
SLOT_BASE = 0x37720
SEQ_WRAP = SLOT_COUNT << 19   # < 2^23: a positive DSP immediate
MAX_AHEAD = 10          # queued blocks kept ahead (6.7 ms)
REPORT_VERSION = 23
REPORT_FIELDS = 53
