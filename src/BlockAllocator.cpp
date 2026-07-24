#include "BlockAllocator.h"

#include <string.h>

#include <algorithm>

#include "ImageFile.h"


namespace bfs {


namespace {


void SetBitRange(std::vector<uint8_t> &bitmap, int64_t first, int64_t last)
{
	if (first >= last) {
		return;
	}

	int64_t pos = first;
	while (pos < last && (pos & 7) != 0) {
		bitmap[pos >> 3] |= static_cast<uint8_t>(1u << (pos & 7));
		pos++;
	}

	int64_t fullEndBit = last & ~static_cast<int64_t>(7);
	if (pos < fullEndBit) {
		::memset(&bitmap[pos >> 3], 0xff, static_cast<size_t>((fullEndBit - pos) >> 3));
		pos = fullEndBit;
	}

	while (pos < last) {
		bitmap[pos >> 3] |= static_cast<uint8_t>(1u << (pos & 7));
		pos++;
	}
}


} // unnamed namespace


BlockAllocator::BlockAllocator(const Geometry &geometry, int64_t firstFreeBlock):
	fGeometry(geometry),
	fCursor(firstFreeBlock)
{
}


int64_t BlockAllocator::Allocate(int64_t count)
{
	int64_t start = fCursor;
	fCursor += count;
	return start;
}


std::vector<BlockRun> BlockAllocator::AllocateStream(int64_t blockCount)
{
	int64_t consumed = 0;
	return AllocateStreamCapped(blockCount, -1, consumed);
}


std::vector<BlockRun> BlockAllocator::AllocateStreamCapped(int64_t blockCount,
	int64_t maxRuns, int64_t &blocksConsumed)
{
	std::vector<BlockRun> runs;
	int64_t groupSize = fGeometry.BlocksPerGroup();
	int64_t remaining = blockCount;
	blocksConsumed = 0;

	while (remaining > 0) {
		if (maxRuns >= 0 && static_cast<int64_t>(runs.size()) >= maxRuns) {
			break;
		}
		int64_t start = fCursor;
		int64_t offsetInGroup = start & (groupSize - 1);
		int64_t toGroupEnd = groupSize - offsetInGroup;
		int64_t chunk = std::min(remaining, toGroupEnd);
		chunk = std::min(chunk, static_cast<int64_t>(kMaxBlockRunLength));

		runs.push_back(fGeometry.ToRun(start, static_cast<uint16_t>(chunk)));
		fCursor += chunk;
		remaining -= chunk;
		blocksConsumed += chunk;
	}

	return runs;
}


void BlockAllocator::WriteBitmap(ImageFile &image, ByteOrder order) const
{
	int64_t blockSize = fGeometry.blockSize;
	int64_t bitmapBytes = fGeometry.bitmapBlocks * blockSize;
	int64_t capacityBits = bitmapBytes * 8;

	std::vector<uint8_t> bitmap(static_cast<size_t>(bitmapBytes), 0);

	// Used blocks are the contiguous prefix [0, cursor). Bits that map past the
	// end of the volume are also marked used so an allocator never hands out a
	// nonexistent block.
	SetBitRange(bitmap, 0, fCursor);
	SetBitRange(bitmap, fGeometry.numBlocks, capacityBits);

	// SetBitRange lays bit N at byte N/8 -- i.e. little-endian 32-bit words. For a
	// big-endian volume the bitmap words are stored byte-swapped, so reverse each
	// 4-byte group (bitmapBytes is a multiple of the block size, hence of 4).
	if (order == ByteOrder::Big) {
		for (size_t i = 0; i + 4 <= bitmap.size(); i += 4) {
			std::swap(bitmap[i + 0], bitmap[i + 3]);
			std::swap(bitmap[i + 1], bitmap[i + 2]);
		}
	}

	for (int64_t k = 0; k < fGeometry.bitmapBlocks; k++) {
		image.WriteBlock(1 + k, &bitmap[static_cast<size_t>(k * blockSize)]);
	}
}


} // bfs
