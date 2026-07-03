#pragma once

#include <stdint.h>
#include <vector>

#include "BfsFormat.h"
#include "Geometry.h"


namespace bfs {


// Number of blocks required to hold 'size' bytes.
inline int64_t StreamBlockCount(uint64_t size, uint32_t blockSize)
{
	return static_cast<int64_t>((size + blockSize - 1) / blockSize);
}


// Fill the 144-byte data_stream region of an inode (at inode::kData) from an
// allocated list of runs and the stream's logical size.
//
// This version supports the direct tier only (up to kNumDirectBlocks runs).
// With the compact, contiguous layout this tool produces, a stream splits into
// multiple runs only at allocation-group boundaries or the 65535-block run cap,
// so the direct tier covers very large files (over 1 GiB on typical geometry).
// A stream that would need more than 12 runs throws; indirect / double-indirect
// support is a documented follow-up (see README).
void WriteDataStream(uint8_t *streamFields, const Geometry &geometry,
	const std::vector<BlockRun> &runs, uint64_t size);


} // bfs
