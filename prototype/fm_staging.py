"""Executable reference protocol for an MCU producer and DSP block hook.

This is a host-side model, not installable firmware. One producer, one consumer,
ordered atomic 24-bit words and configuration changes at block boundaries are
assumptions. Real transport ordering and safe memory allocation are unverified.
"""
from dataclasses import dataclass, field

WORD_MASK = 0xFFFFFF
TRACKS, FRAMES = 8, 32
READY, EPOCH, SEQUENCE, TRACK_MASK, FRAME_COUNT, PAYLOAD = range(6)
SLOT_WORDS = PAYLOAD + TRACKS * FRAMES


class BusySlot(RuntimeError):
    pass


@dataclass
class Slot:
    words: list[int] = field(default_factory=lambda: [0] * SLOT_WORDS)
    # Producer-local reservation: not an additional shared protocol word.
    writing: bool = False


class Bridge:
    def __init__(self, track_mask=1, epoch=1, sequence=0):
        if not 0 <= track_mask <= 255 or not 1 <= epoch <= WORD_MASK or not 0 <= sequence <= WORD_MASK:
            raise ValueError('Invalid configuration')
        self.slots = [Slot(), Slot()]
        self.track_mask, self.epoch, self.sequence = track_mask, epoch, sequence
        self.underruns = self.dropped = self.accepted = 0
        self.status = 'ok'

    def configure(self, track_mask):
        """Called at a consumer block boundary; old in-flight slots remain owned."""
        if not 0 <= track_mask <= 255:
            raise ValueError('Invalid track mask')
        if self.epoch == WORD_MASK:
            raise RuntimeError('Quiescent reset required before epoch reuse')
        self.epoch += 1
        self.track_mask, self.sequence = track_mask, 0

    def publication(self, epoch, sequence, track_mask, samples):
        """Reserve now; return an iterator performing one ordered word write/step.

        Iterate to completion (including READY last), or quiesce the producer
        before abandoning a reservation. A halted partial transfer stays private.
        """
        if not 1 <= epoch <= WORD_MASK or not 0 <= sequence <= WORD_MASK or not 0 <= track_mask <= 255:
            raise ValueError('Invalid block metadata')
        if len(samples) != TRACKS * FRAMES or any(not 0 <= x <= WORD_MASK for x in samples):
            raise ValueError('Payload must contain 256 encoded 24-bit words')
        slot = self.slots[sequence & 1]
        if slot.words[READY] or slot.writing:
            raise BusySlot('Unconsumed or producer-reserved slot')
        slot.writing = True
        writes = [(EPOCH, epoch), (SEQUENCE, sequence), (TRACK_MASK, track_mask),
                  (FRAME_COUNT, FRAMES)]
        writes += [(PAYLOAD + i, word) for i, word in enumerate(samples)]
        writes += [(READY, 1)]

        def steps():
            for offset, word in writes:
                slot.words[offset] = word
                if offset == READY:
                    slot.writing = False
                yield offset
        return steps()

    def consume(self, native_banks, bank, block_length=FRAMES, during_copy=None):
        """Reference for a bounded post-sampler hook; mutate only FM-track spans.

        `native_banks` is 512 encoded words, track-major, two banks per track.
        The current sequence advances even on underrun. A matching slot remains
        READY until every selected track has copied, then is released.
        """
        if len(native_banks) != TRACKS * 2 * FRAMES or bank not in (0, 1):
            raise ValueError('Invalid native buffer fixture')
        if block_length != FRAMES:
            self.status = 'unsupported_block_length'
            return None
        self.status = 'ok'
        selected = None
        for index, slot in enumerate(self.slots):
            words = slot.words
            if not words[READY]:
                continue
            valid = (words[READY] == 1 and words[EPOCH] == self.epoch and
                     words[TRACK_MASK] == self.track_mask and words[FRAME_COUNT] == FRAMES and
                     (words[SEQUENCE] & 1) == index)
            distance = (words[SEQUENCE] - self.sequence) & WORD_MASK
            if not valid or distance not in (0, 1):
                words[READY] = 0
                self.dropped += 1
            elif distance == 0:
                selected = slot
            # distance 1 is the one permitted look-ahead block: leave it owned.
        for track in range(TRACKS):
            if not self.track_mask & (1 << track):
                continue
            destination = track * 2 * FRAMES + bank * FRAMES
            for i in range(FRAMES):
                native_banks[destination + i] = (selected.words[PAYLOAD + track * FRAMES + i]
                                                if selected else 0)
                if during_copy:
                    during_copy(selected, track, i)
        if selected:
            selected.words[READY] = 0
            self.accepted += 1
        elif self.track_mask:
            self.underruns += 1
        accepted_sequence = self.sequence if selected else None
        self.sequence = (self.sequence + 1) & WORD_MASK
        return accepted_sequence
