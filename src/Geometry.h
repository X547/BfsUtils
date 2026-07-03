#pragma once

#include <stdint.h>

#include "BfsFormat.h"


namespace bfs {


// Fixed geometry of the volume being generated. Filled in once the total
// content size is known (see BfsBuilder), then used everywhere to convert
// between absolute block numbers and block_runs.
struct Geometry {
	uint32_t blockSize = 0;
	uint32_t blockShift = 0;
	int64_t numBlocks = 0;
	int32_t agShift = 0;
	int32_t blocksPerAg = 0;   // bitmap blocks per allocation group
	int32_t numAgs = 0;
	int64_t bitmapBlocks = 0;
	BlockRun logBlocks;        // location + length of the journal area

	int64_t BlocksPerGroup() const {return static_cast<int64_t>(1) << agShift;}

	int64_t ToBlock(const BlockRun &run) const
	{
		return (static_cast<int64_t>(run.group) << agShift)
			| static_cast<int64_t>(run.start);
	}

	uint64_t ToOffset(const BlockRun &run) const
	{
		return static_cast<uint64_t>(ToBlock(run)) << blockShift;
	}

	// Build a run for 'length' consecutive blocks starting at absolute 'block'.
	// The caller must guarantee the run stays within a single allocation group.
	BlockRun ToRun(int64_t block, uint16_t length) const
	{
		BlockRun run;
		run.group = static_cast<int32_t>(block >> agShift);
		run.start = static_cast<uint16_t>(block & (BlocksPerGroup() - 1));
		run.length = length;
		return run;
	}
};


} // bfs
