#pragma once

#include <stdint.h>
#include <vector>

#include "BfsFormat.h"
#include "Geometry.h"


namespace bfs {


class BlockAllocator;


// Number of blocks required to hold 'size' bytes.
inline int64_t StreamBlockCount(uint64_t size, uint32_t blockSize)
{
	return static_cast<int64_t>((size + blockSize - 1) / blockSize);
}


// How a data stream is split across the direct / indirect / double-indirect
// tiers. Defaults follow the standard BFS layout; the environment overrides
// (see FromEnvironment) let tests force the double-indirect path with a small
// file, since it is otherwise unreachable below multi-gigabyte sizes.
struct StreamTuning {
	int64_t maxIndirectRuns = 0;   // entries the indirect array holds before dd
	int64_t forcedBase = 0;        // 0 => choose the double-indirect base automatically

	static StreamTuning FromEnvironment(const Geometry &geometry);
};


// The concrete placement of one stream's blocks across the three data_stream
// tiers, produced by BuildStreamLayout and consumed by WriteDataStream and the
// array-block/content serializers.
struct StreamLayout {
	std::vector<BlockRun> direct;              // <= kNumDirectBlocks runs
	BlockRun indirect;                         // indirect array run (zero if unused)
	std::vector<BlockRun> indirectEntries;     // continuation runs stored in it
	BlockRun doubleIndirect;                   // top array run, length == base (zero if unused)
	std::vector<BlockRun> ddSecondArrays;      // one second-level array run per group
	std::vector<BlockRun> ddDataRuns;          // fixed 'base'-block data runs
	int64_t base = 0;                          // double_indirect fixed run length

	int64_t maxDirectRange = 0;
	int64_t maxIndirectRange = 0;
	int64_t maxDoubleIndirectRange = 0;
	uint64_t size = 0;

	// The stream's data runs in logical order (direct ++ indirect ++ dd), used to
	// write the file/tree contents sequentially.
	std::vector<BlockRun> DataRuns() const;
};


// Upper bound on the extra array/alignment blocks (beyond the 'dataBlocks' of
// data) a stream needs, so ComputeGeometry can reserve room. Place then uses the
// exact (<=) amount; any slack becomes a harmless free tail.
int64_t StreamMetadataUpperBound(const Geometry &geometry, int64_t dataBlocks,
	const StreamTuning &tuning);


// Allocate all of a stream's blocks (data + arrays) from 'allocator' and return
// the resulting tier layout.
StreamLayout BuildStreamLayout(BlockAllocator &allocator, const Geometry &geometry,
	int64_t dataBlocks, uint64_t size, const StreamTuning &tuning);


// Fill the 144-byte data_stream region of an inode (at inode::kData) from a
// computed layout. Supports all three tiers.
void WriteDataStream(uint8_t *streamFields, const Geometry &geometry,
	const StreamLayout &layout);


} // bfs
