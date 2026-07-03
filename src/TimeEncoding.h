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

	int64_t field = (static_cast<int64_t>(nanoseconds) + 16383) >> 14;
	if (field > 0x0fff) {
		field = 0x0fff;
	}
	value |= (field << 4) & kInodeTimeMask;
	return value;
}


} // bfs
