#pragma once

#include <stdint.h>

#include "BfsFormat.h"


namespace bfs {


// Encode a POSIX (seconds, nanoseconds) pair into BFS's packed 64-bit time
// value (see BFS_On-Disk_Format.md section 15). The whole seconds occupy the
// high bits; the low 16 bits carry a sub-second component. A writer only needs
// a deterministic encoding: time-based index keys store this same value, so as
// long as encoding is consistent the resulting indices are correct.

inline int64_t EncodeTime(int64_t seconds, uint32_t nanoseconds)
{
	int64_t value = seconds << kInodeTimeShift;
	if (nanoseconds == 0) {
		// Reserved 0xF000 range marks "no real sub-second timestamp".
		value |= 0xf000;
		return value;
	}

	// The >> 14 already positions the field in bits [15:4]; masking with
	// kInodeTimeMask clears the low nibble, which the reference implementation
	// fills with a uniqueness counter. Leaving it zero keeps output
	// deterministic; duplicate index keys are handled by the tree builder.
	// A legal nanosecond value tops out at 0xee60, clear of the reserved
	// 0xf000 range, so only an out-of-range input needs guarding against.
	if (nanoseconds > 999999999) {
		nanoseconds = 999999999;
	}
	value |= ((static_cast<int64_t>(nanoseconds) + 16383) >> 14) & kInodeTimeMask;
	return value;
}


// Decode a BFS-encoded time value back into seconds and nanoseconds.
inline void DecodeTime(int64_t encoded, int64_t &seconds, uint32_t &nanoseconds)
{
	seconds = encoded >> kInodeTimeShift;
	if ((encoded & 0xf000) == 0xf000) {
		nanoseconds = 0;
	} else {
		nanoseconds = static_cast<uint32_t>((encoded & kInodeTimeMask) << 14);
	}
}


} // bfs
