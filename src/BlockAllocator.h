#pragma once

#include <stdint.h>
#include <vector>

#include "BfsFormat.h"
#include "Geometry.h"


namespace bfs {


class ImageFile;


// Offline bump allocator. Because the image is generated in a single pass with
// every object known up front, blocks are handed out sequentially: the reserved
// region (boot block, bitmap, log) is a contiguous prefix, and all content
// follows it. The set of used blocks is therefore exactly [0, Cursor()), which
// makes emitting the on-disk bitmap trivial.
class BlockAllocator {
public:
	BlockAllocator(const Geometry &geometry, int64_t firstFreeBlock);

	// Allocate 'count' consecutive blocks and return the first block number.
	int64_t Allocate(int64_t count);

	// Allocate a stream of 'blockCount' blocks, returned as one or more runs.
	// Runs never cross an allocation-group boundary and never exceed
	// kMaxBlockRunLength, as required by block_run validity.
	std::vector<BlockRun> AllocateStream(int64_t blockCount);

	// Like AllocateStream but stops once 'maxRuns' runs have been produced even if
	// blocks remain. Sets 'blocksConsumed' to how many blocks were actually
	// allocated. Used to fill the direct+indirect tiers, leaving any overflow for
	// the double-indirect tier.
	std::vector<BlockRun> AllocateStreamCapped(int64_t blockCount, int64_t maxRuns,
		int64_t &blocksConsumed);

	int64_t Cursor() const {return fCursor;}
	int64_t UsedBlocks() const {return fCursor;}

	// Emit the block bitmap (blocks 1 .. bitmapBlocks) reflecting the used set.
	void WriteBitmap(ImageFile &image) const;

private:
	Geometry fGeometry;
	int64_t fCursor = 0;
};


} // bfs
