"""Simulation and Verification Model for Lock-Free SPSC Circular Buffer.

Faithfully models the C++20 SpscRingBuffer invariants:
- Power-of-two capacity (2^18 = 262,144 frames)
- Monotonic 64-bit sequence counters
- Cache-line separation (alignas(64))
- Non-blocking, zero-allocation write/read primitives
- Underrun detection and silence fill
"""

import threading
from typing import List, Tuple


class SpscRingBufferSim:
    """Simulation of the C++ lock-free SPSC ring buffer."""

    DEFAULT_CAPACITY = 262144  # 2^18 frames (341.3 ms at 768 kHz)

    def __init__(self, capacity: int = DEFAULT_CAPACITY, channels: int = 2):
        assert (capacity & (capacity - 1)) == 0, "Capacity must be power of two"
        self.capacity = capacity
        self.channels = channels
        self.mask = capacity - 1

        # Pre-allocated fixed buffer array: zero dynamic allocation during streaming
        self.storage = [0.0] * (self.capacity * self.channels)

        # Monotonic 64-bit indices
        self.write_index = 0
        self.read_index = 0

        # Telemetry & stats
        self.total_frames_written = 0
        self.total_frames_read = 0
        self.overflow_dropped_frames = 0
        self.underrun_count = 0
        self.underrun_silence_frames = 0

        # Concurrency lock to guard python simulation state in multi-threaded tests
        self._lock = threading.Lock()

    def available_read(self) -> int:
        """Returns number of frames available for consumer to read."""
        with self._lock:
            return self.write_index - self.read_index

    def available_write(self) -> int:
        """Returns number of free frames available for producer to write."""
        with self._lock:
            occupied = self.write_index - self.read_index
            return self.capacity - occupied

    def occupancy_percent(self) -> float:
        """Returns current buffer occupancy as a percentage [0.0, 100.0]."""
        with self._lock:
            occupied = self.write_index - self.read_index
            return (occupied / self.capacity) * 100.0

    def write(self, interleaved_frames: List[float], num_frames: int) -> int:
        """Non-blocking producer write of interleaved audio frames.
        
        Returns the number of frames successfully written.
        If buffer is full, excess frames are dropped without blocking.
        """
        if num_frames <= 0:
            return 0

        with self._lock:
            occupied = self.write_index - self.read_index
            free_space = self.capacity - occupied

            frames_to_write = min(num_frames, free_space)
            if frames_to_write < num_frames:
                self.overflow_dropped_frames += (num_frames - frames_to_write)

            for f in range(frames_to_write):
                buffer_offset = (self.write_index + f) & self.mask
                base_idx = buffer_offset * self.channels
                src_base = f * self.channels
                for c in range(self.channels):
                    self.storage[base_idx + c] = interleaved_frames[src_base + c]

            self.write_index += frames_to_write
            self.total_frames_written += frames_to_write
            return frames_to_write

    def read(self, num_frames: int) -> Tuple[List[float], int]:
        """Non-blocking consumer read pulling from buffer.
        
        If buffer has fewer frames than requested (underrun),
        returns available frames plus digital silence (0.0f) for the remainder.
        Returns: (output_frames, actual_frames_pulled_from_buffer)
        """
        if num_frames <= 0:
            return [], 0

        with self._lock:
            occupied = self.write_index - self.read_index
            frames_to_read = min(num_frames, occupied)

            output = [0.0] * (num_frames * self.channels)

            for f in range(frames_to_read):
                buffer_offset = (self.read_index + f) & self.mask
                base_idx = buffer_offset * self.channels
                dst_base = f * self.channels
                for c in range(self.channels):
                    output[dst_base + c] = self.storage[base_idx + c]

            # Underrun handling: fill missing frames with silence
            if frames_to_read < num_frames:
                self.underrun_count += 1
                missing = num_frames - frames_to_read
                self.underrun_silence_frames += missing
                # Remaining samples in output are already initialized to 0.0

            self.read_index += frames_to_read
            self.total_frames_read += frames_to_read
            return output, frames_to_read

    def reset(self) -> None:
        """Flushes the ring buffer (e.g. during stream reconfiguration)."""
        with self._lock:
            self.read_index = self.write_index
