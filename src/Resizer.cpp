#include "Resizer.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <utility>

#include "Endian.h"


namespace bfs {


namespace {


bool RecoverJournal(ResizeJournal &journal, ImageFile &image)
{
	return journal.Recover(image);
}


} // unnamed namespace


Resizer::Resizer(const std::string &imagePath, const ResizeOptions &options):
	fImagePath(imagePath),
	fOptions(options),
	fImage(imagePath, ImageFile::Access::ReadWrite),
	fJournal(imagePath),
	fReader((RecoverJournal(fJournal, fImage), fImage)),
	fGeo(fReader.GetGeometry()),
	fUsedBlocks(fReader.UsedBlocks())
{
	// The resizer writes structures back with the little-endian Put* helpers, so
	// it can only safely modify little-endian volumes. Big-endian volumes are
	// read-only across these tools; refuse rather than corrupt.
	if (fReader.Order() != ByteOrder::Little) {
		throw std::runtime_error(
			"big-endian volumes are supported read-only; cannot resize");
	}
	fJournal.SetBlockSize(fGeo.blockSize);
}


int64_t Resizer::Reserved(int64_t bitmapBlocks) const
{
	return 1 + bitmapBlocks + fGeo.logBlocks.length;
}


int64_t Resizer::BitmapBlocksFor(int64_t numBlocks) const
{
	int64_t bitsPerBlock = static_cast<int64_t>(fGeo.blockSize) * 8;
	return (numBlocks + bitsPerBlock - 1) / bitsPerBlock;
}


bool Resizer::Allocated(int64_t block) const
{
	size_t index = static_cast<size_t>(block >> 3);
	if (index >= fBitmap.size()) {
		return true;
	}
	return (fBitmap[index] >> (block & 7)) & 1;
}


void Resizer::SetAllocated(int64_t block, bool used)
{
	size_t index = static_cast<size_t>(block >> 3);
	if (index >= fBitmap.size()) {
		return;
	}
	uint8_t mask = static_cast<uint8_t>(1u << (block & 7));
	if (used) {
		fBitmap[index] |= mask;
	} else {
		fBitmap[index] &= static_cast<uint8_t>(~mask);
	}
	fDirtyBitmap.insert(block / (static_cast<int64_t>(fGeo.blockSize) * 8));
}


int64_t Resizer::AllocateBlock(int64_t low, int64_t limit)
{
	for (int64_t block = low; block < limit; block++) {
		if (!Allocated(block)) {
			SetAllocated(block, true);
			return block;
		}
	}
	return -1;
}


int64_t Resizer::AllocateRun(int64_t length, int64_t low, int64_t limit)
{
	// Find 'length' consecutive free blocks that do not cross an allocation-group
	// boundary (a block_run must stay within one group).
	int64_t groupSize = static_cast<int64_t>(1) << fGeo.agShift;
	int64_t start = low;
	int64_t run = 0;
	for (int64_t block = low; block < limit; block++) {
		if (Allocated(block)) {
			run = 0;
			continue;
		}
		if (run == 0 || (block % groupSize) == 0) {
			start = block;
			run = 1;
		} else {
			run++;
		}
		if (run >= length) {
			for (int64_t b = start; b < start + length; b++) {
				SetAllocated(b, true);
			}
			return start;
		}
	}
	return -1;
}


void Resizer::StageDirtyBitmap()
{
	for (int64_t index : fDirtyBitmap) {
		fJournal.Stage(1 + index,
			fBitmap.data() + static_cast<size_t>(index) * fGeo.blockSize);
	}
	fDirtyBitmap.clear();
}


int64_t Resizer::CountUsed(int64_t numBlocks) const
{
	// Popcount the allocated bits in [0, numBlocks): whole 64-bit words first, then
	// leftover whole bytes, then the partial final byte. Counting only [0, numBlocks)
	// (rather than the whole in-memory bitmap) is what makes this correct on the
	// shrink path, where the bitmap still describes the larger pre-shrink volume and
	// its middle is free rather than allocated padding.
	int64_t fullBytes = numBlocks >> 3;
	const uint8_t *data = fBitmap.data();
	int64_t count = 0;
	int64_t i = 0;
	for (; i + 8 <= fullBytes; i += 8) {
		uint64_t word;
		memcpy(&word, data + i, sizeof(word));
		count += std::popcount(word);
	}
	for (; i < fullBytes; i++) {
		count += std::popcount(data[i]);
	}
	int rem = static_cast<int>(numBlocks & 7);
	if (rem != 0) {
		count += std::popcount(
			static_cast<unsigned>(data[fullBytes] & ((1u << rem) - 1)));
	}
	return count;
}


std::vector<uint8_t> Resizer::BuildSuperBlock(int64_t numBlocks, int64_t numAgs,
	int64_t usedBlocks, const BlockRun *indices)
{
	std::vector<uint8_t> block(fGeo.blockSize);
	fImage.ReadAt(0, block.data(), fGeo.blockSize);
	uint8_t *p = block.data() + kSuperBlockOffset;
	PutS64(p + super::kNumBlocks, numBlocks);
	PutS32(p + super::kNumAgs, static_cast<int32_t>(numAgs));
	PutS64(p + super::kUsedBlocks, usedBlocks);
	if (indices != nullptr) {
		PutBlockRun(p + super::kIndices, *indices);
	}
	return block;
}


void Resizer::Grow(int64_t newNum)
{
	int64_t bitsPerBlock = static_cast<int64_t>(fGeo.blockSize) * 8;
	int64_t groupSize = static_cast<int64_t>(1) << fGeo.agShift;
	int64_t finalBitmap = BitmapBlocksFor(newNum);

	// ag_shift is held fixed, so the reserved prefix (boot + bitmap + log) must
	// still fit inside the first allocation group after growing. Enlarging
	// ag_shift would re-encode every block_run in the volume and is out of scope.
	if (Reserved(finalBitmap) > groupSize) {
		int64_t maxBitmap = groupSize - 1 - fGeo.logBlocks.length;
		int64_t maxBlocks = maxBitmap > 0 ? maxBitmap * bitsPerBlock : 0;
		char message[192];
		snprintf(message, sizeof(message),
			"target too large for this volume's ag_shift: at most ~%lld blocks "
			"(%lld bytes) without re-encoding every block_run",
			static_cast<long long>(maxBlocks),
			static_cast<long long>(maxBlocks) * fGeo.blockSize);
		throw std::runtime_error(message);
	}

	if (fOptions.dryRun) {
		int64_t newNumAgs = (newNum + groupSize - 1) >> fGeo.agShift;
		int64_t boundaries = finalBitmap - fGeo.bitmapBlocks;
		printf("grow: %lld -> %lld blocks, num_ags %d -> %lld, bitmap %lld -> %lld "
			"blocks (%lld boundary crossing(s), up to ~%lld blocks relocated)\n",
			static_cast<long long>(fGeo.numBlocks), static_cast<long long>(newNum),
			fGeo.numAgs, static_cast<long long>(newNumAgs),
			static_cast<long long>(fGeo.bitmapBlocks),
			static_cast<long long>(finalBitmap), static_cast<long long>(boundaries),
			static_cast<long long>(boundaries));
		return;
	}

	// Fast path: the target stays within the current bitmap's capacity, so the
	// reserved region does not move -- pure metadata plus a file extension.
	if (finalBitmap == fGeo.bitmapBlocks) {
		GrowInPlace(newNum);
		return;
	}

	// General path. Growing the bitmap by (finalBitmap - startBitmap) blocks expands
	// the reserved prefix (boot + bitmap + log) by the same amount. With the log in
	// its standard position right after the bitmap, the newly-claimed blocks form one
	// contiguous range [Reserved(startBitmap), Reserved(finalBitmap)) immediately past
	// the current log. Evacuate that whole range ONCE, then append the bitmap blocks
	// one boundary at a time as pure metadata. Doing the relocation up front rather
	// than per boundary turns O(boundaries) full-filesystem scans into a single scan.
	int64_t startBitmap = fGeo.bitmapBlocks;
	int64_t startCapacity = startBitmap * bitsPerBlock;

	// Fill the current bitmap to capacity first (metadata only) so the relocation has
	// the entire committed tail available as target space. startCapacity <= newNum
	// here, since finalBitmap > startBitmap means newNum exceeds startCapacity.
	if (fGeo.numBlocks < startCapacity) {
		GrowInPlace(startCapacity);
	}

	// Evacuate the reserved-expansion range into committed free space no later step
	// can reclaim ([Reserved(finalBitmap), numBlocks)). Idempotent on re-run: any
	// blocks already vacated by an interrupted attempt simply have nothing to move.
	fWinLo = Reserved(startBitmap);
	fWinHi = Reserved(finalBitmap);
	fAllocLow = Reserved(finalBitmap);
	fAllocHigh = fGeo.numBlocks;
	RelocateInWindow();
	for (int64_t block = fWinLo; block < fWinHi; block++) {
		if (Allocated(block)) {
			throw std::runtime_error("reserved-expansion block still allocated after "
				"relocation (unreferenced allocated block?); run bfscheck");
		}
	}

	if (fRelocated > 0) {
		fprintf(stderr, "\n");
	}

	// Append one bitmap block per boundary. Each iteration is metadata only now that
	// the reserved expansion is free: fill the current bitmap to capacity, take over
	// the old log's head block for the new bitmap block, and shift the empty log
	// forward by one (into an already-vacated block). Each commit leaves a mountable
	// volume, so an interrupted run can be resumed.
	while (fGeo.bitmapBlocks < finalBitmap) {
		int64_t curB = fGeo.bitmapBlocks;
		int64_t capacity = curB * bitsPerBlock;   // current bitmap covers [0, capacity)
		if (fGeo.numBlocks < capacity) {
			GrowInPlace(capacity);
		}
		int64_t nextCapacity = (curB + 1) * bitsPerBlock;
		int64_t stepTarget = std::min(newNum, nextCapacity);
		AddBitmapBlock(stepTarget);
	}
}


void Resizer::GrowInPlace(int64_t newNum)
{
	// Precondition: newNum lies within the current bitmap's capacity, so no new
	// bitmap block is needed and the reserved region stays put.
	int64_t oldNum = fGeo.numBlocks;
	int64_t groupSize = static_cast<int64_t>(1) << fGeo.agShift;
	int64_t newNumAgs = (newNum + groupSize - 1) >> fGeo.agShift;

	fImage.Truncate(static_cast<uint64_t>(newNum) * fGeo.blockSize);
	fImage.Flush();

	for (int64_t block = oldNum; block < newNum; block++) {
		SetAllocated(block, false);
	}

	fJournal.Begin();
	StageDirtyBitmap();
	std::vector<uint8_t> superBlock = BuildSuperBlock(newNum, newNumAgs,
		CountUsed(newNum), nullptr);
	fJournal.Stage(0, superBlock.data());
	fJournal.Commit(fImage);

	fReader.ReloadSuperBlock();
	fGeo = fReader.GetGeometry();
}


void Resizer::AddBitmapBlock(int64_t stepTarget)
{
	int64_t bitsPerBlock = static_cast<int64_t>(fGeo.blockSize) * 8;
	int64_t groupSize = static_cast<int64_t>(1) << fGeo.agShift;
	int64_t curB = fGeo.bitmapBlocks;
	int64_t nextCapacity = (curB + 1) * bitsPerBlock;
	int64_t logLen = fGeo.logBlocks.length;

	// This only supports the standard layout where the log sits immediately after
	// the bitmap; the new bitmap block takes over the log's old head block.
	int64_t oldLogStart = fGeo.ToBlock(fGeo.logBlocks);
	if (oldLogStart != 1 + curB) {
		throw std::runtime_error("cannot grow across a bitmap boundary: the log is "
			"not in the standard position right after the bitmap");
	}
	int64_t newLogStart = 1 + (curB + 1);            // == 2 + curB
	int64_t src = Reserved(curB);                    // == 1 + curB + logLen (now free)

	// Extend the backing file to cover the newly committed range. All reserved
	// blocks (the new bitmap block and the shifted log) already lie on disk.
	fImage.Truncate(static_cast<uint64_t>(stepTarget) * fGeo.blockSize);
	fImage.Flush();

	// Append the new (all-free) bitmap block; it describes blocks [capacity,
	// nextCapacity). Growing bitmapBlocks first lets SetAllocated address it.
	// Its on-disk home (the old log's head block) holds stale bytes, so it must be
	// staged even if no bit in its range changes -- mark it dirty explicitly.
	fBitmap.resize(static_cast<size_t>(curB + 1) * fGeo.blockSize, 0);
	fGeo.bitmapBlocks = curB + 1;
	fDirtyBitmap.insert(curB);

	// Padding beyond the committed size is marked used; the reserved-expansion
	// block becomes the shifted log's tail block. The old log head (block 1+curB)
	// stays used -- it is now the new bitmap block.
	for (int64_t block = stepTarget; block < nextCapacity; block++) {
		SetAllocated(block, true);
	}
	SetAllocated(src, true);

	int64_t newNumAgs = (stepTarget + groupSize - 1) >> fGeo.agShift;
	BlockRun newLog = fGeo.ToRun(newLogStart, static_cast<uint16_t>(logLen));

	fJournal.Begin();
	StageDirtyBitmap();
	std::vector<uint8_t> superBlock = BuildSuperBlock(stepTarget, newNumAgs,
		CountUsed(stepTarget), nullptr);
	uint8_t *p = superBlock.data() + kSuperBlockOffset;
	PutBlockRun(p + super::kLogBlocks, newLog);
	PutS64(p + super::kLogStart, newLogStart);
	PutS64(p + super::kLogEnd, newLogStart);
	fJournal.Stage(0, superBlock.data());
	fJournal.Commit(fImage);

	fReader.ReloadSuperBlock();
	fGeo = fReader.GetGeometry();
}


void Resizer::ShrinkFreeTail(int64_t newNum)
{
	int64_t oldNum = fGeo.numBlocks;
	int64_t oldBitmapBlocks = fGeo.bitmapBlocks;
	int64_t newBitmapBlocks = BitmapBlocksFor(newNum);
	int64_t bitsPerBlock = static_cast<int64_t>(fGeo.blockSize) * 8;
	int64_t newCapacity = newBitmapBlocks * bitsPerBlock;
	int64_t groupSize = static_cast<int64_t>(1) << fGeo.agShift;
	int64_t newNumAgs = (newNum + groupSize - 1) >> fGeo.agShift;
	int64_t newReserved = Reserved(newBitmapBlocks);
	int64_t logEnd = fGeo.ToBlock(fGeo.logBlocks) + fGeo.logBlocks.length;

	if (newNum < newReserved || logEnd > newNum) {
		throw std::runtime_error("target size is too small for the reserved area");
	}

	// When the bitmap shrinks, the (empty) log must relocate to stay immediately
	// after the smaller bitmap, so the reserved region remains a contiguous used
	// prefix (Haiku's allocator requires this). This only supports the standard
	// layout where the log currently sits right after the bitmap.
	int64_t logLen = fGeo.logBlocks.length;
	int64_t oldLogStart = fGeo.ToBlock(fGeo.logBlocks);
	bool moveLog = newBitmapBlocks < oldBitmapBlocks;
	int64_t newLogStart = moveLog ? 1 + newBitmapBlocks : oldLogStart;
	if (moveLog && oldLogStart != 1 + oldBitmapBlocks) {
		throw std::runtime_error("cannot shrink across a bitmap boundary: the log is "
			"not in the standard position right after the bitmap");
	}

	if (fOptions.dryRun) {
		printf("shrink: %lld -> %lld blocks, num_ags %d -> %lld%s, no blocks moved\n",
			static_cast<long long>(oldNum), static_cast<long long>(newNum),
			fGeo.numAgs, static_cast<long long>(newNumAgs),
			moveLog ? ", log relocated" : "");
		return;
	}

	if (moveLog) {
		// New log occupies [newLogStart, newLogStart + logLen); these were the
		// former bitmap tail plus the head of the old log, all already used.
		for (int64_t block = newLogStart; block < newLogStart + logLen; block++) {
			SetAllocated(block, true);
		}
		// Free the tail of the old log that the new (shifted) log no longer covers.
		for (int64_t block = newLogStart + logLen; block < oldLogStart + logLen; block++) {
			SetAllocated(block, false);
		}
	}

	// Mark padding beyond the new volume used.
	for (int64_t block = newNum; block < newCapacity; block++) {
		SetAllocated(block, true);
	}

	int64_t usedNew = CountUsed(newNum);

	fJournal.Begin();
	StageDirtyBitmap();
	std::vector<uint8_t> superBlock = BuildSuperBlock(newNum, newNumAgs, usedNew,
		nullptr);
	if (moveLog) {
		uint8_t *p = superBlock.data() + kSuperBlockOffset;
		PutBlockRun(p + super::kLogBlocks, fGeo.ToRun(newLogStart,
			static_cast<uint16_t>(logLen)));
		PutS64(p + super::kLogStart, newLogStart);
		PutS64(p + super::kLogEnd, newLogStart);
	}
	fJournal.Stage(0, superBlock.data());
	fJournal.Commit(fImage);

	fImage.Truncate(static_cast<uint64_t>(newNum) * fGeo.blockSize);
	fImage.Flush();
}


void Resizer::CollectContainerChildren(const Inode &container,
	std::set<int64_t> &visited, std::vector<Inode> &out)
{
	// One level: the container's tree maps names to child inodes (attribute or
	// index entries) that are not themselves recursed into.
	std::vector<DirEntry> entries = fReader.ReadDirectory(container);
	for (const DirEntry &entry : entries) {
		if (visited.count(entry.inode) != 0) {
			continue;
		}
		visited.insert(entry.inode);
		out.push_back(fReader.ReadInode(entry.inode));
	}
}


void Resizer::CollectInodes(int64_t block, std::set<int64_t> &visited,
	std::vector<Inode> &out)
{
	if (visited.count(block) != 0) {
		return;
	}
	visited.insert(block);
	Inode inode = fReader.ReadInode(block);
	out.push_back(inode);

	if (!inode.attributes.IsZero()) {
		int64_t attrDir = fGeo.ToBlock(inode.attributes);
		if (visited.count(attrDir) == 0) {
			visited.insert(attrDir);
			Inode dir = fReader.ReadInode(attrDir);
			out.push_back(dir);
			CollectContainerChildren(dir, visited, out);
		}
	}

	if (inode.IsDirectory()) {
		for (const DirEntry &entry : fReader.ReadDirectory(inode)) {
			CollectInodes(entry.inode, visited, out);
		}
	}
}


int64_t Resizer::RelocateStreamTail(const Inode &inode, bool dryRun)
{
	// A short symlink stores its target inline in the data_stream union, so its
	// "runs" are text, not block_runs; it has no stream to relocate (matching
	// Checker's shortSymlink guard).
	if (inode.IsSymlink() && !inode.IsLongSymlink()) {
		return 0;
	}

	int64_t moved = 0;
	int64_t bs = fGeo.blockSize;
	// A run needs relocating when it overlaps the active source window.
	auto inTail = [&](const BlockRun &run) {
		if (run.IsZero()) {
			return false;
		}
		int64_t start = fGeo.ToBlock(run);
		return start < fWinHi && start + run.length > fWinLo;
	};

	// Direct runs, patched in the inode's own data_stream.
	for (int i = 0; i < kNumDirectBlocks; i++) {
		const BlockRun &run = inode.stream.direct[i];
		if (run.IsZero()) {
			break;
		}
		if (inTail(run)) {
			moved += run.length;
			if (!dryRun) {
				RelocateRun(inode.block, inode::kData + stream::kDirect + i * 8, run);
			}
		}
	}

	// Indirect: payload runs first (patched inside the array block), then the
	// array block itself (patched in the inode). Bottom-up so each parent copy
	// already includes the child edits.
	if (!inode.stream.indirect.IsZero()) {
		BlockRun indirect = inode.stream.indirect;
		int64_t arrStart = fGeo.ToBlock(indirect);
		std::vector<uint8_t> array = ReadRunBytes(indirect);
		int64_t entries = static_cast<int64_t>(array.size()) / 8;
		for (int64_t j = 0; j < entries; j++) {
			BlockRun run = GetBlockRun(array.data() + j * 8);
			if (run.IsZero()) {
				break;
			}
			if (inTail(run)) {
				moved += run.length;
				if (!dryRun) {
					RelocateRun(arrStart + (j * 8) / bs, static_cast<int>((j * 8) % bs),
						run);
				}
			}
		}
		if (inTail(indirect)) {
			moved += indirect.length;
			if (!dryRun) {
				RelocateRun(inode.block, inode::kData + stream::kIndirect, indirect);
			}
		}
	}

	// Double-indirect: payload runs, then second-level array blocks (patched in the
	// top block), then the top block itself (patched in the inode).
	if (!inode.stream.doubleIndirect.IsZero()) {
		BlockRun dbl = inode.stream.doubleIndirect;
		int64_t topStart = fGeo.ToBlock(dbl);
		std::vector<uint8_t> top = ReadRunBytes(dbl);
		int64_t topEntries = static_cast<int64_t>(top.size()) / 8;
		for (int64_t t = 0; t < topEntries; t++) {
			BlockRun arr = GetBlockRun(top.data() + t * 8);
			if (arr.IsZero()) {
				break;
			}
			int64_t arrStart = fGeo.ToBlock(arr);
			std::vector<uint8_t> level = ReadRunBytes(arr);
			int64_t entries = static_cast<int64_t>(level.size()) / 8;
			for (int64_t j = 0; j < entries; j++) {
				BlockRun run = GetBlockRun(level.data() + j * 8);
				if (run.IsZero()) {
					break;
				}
				if (inTail(run)) {
					moved += run.length;
					if (!dryRun) {
						RelocateRun(arrStart + (j * 8) / bs,
							static_cast<int>((j * 8) % bs), run);
					}
				}
			}
			if (inTail(arr)) {
				moved += arr.length;
				if (!dryRun) {
					RelocateRun(topStart + (t * 8) / bs, static_cast<int>((t * 8) % bs),
						arr);
				}
			}
		}
		if (inTail(dbl)) {
			moved += dbl.length;
			if (!dryRun) {
				RelocateRun(inode.block, inode::kData + stream::kDoubleIndirect, dbl);
			}
		}
	}

	return moved;
}


// Map a byte offset within a data stream to the absolute block that holds it.
// Block-granular counterpart of BfsReader::ResolveRun.
int64_t Resizer::StreamPhysicalBlock(const DataStreamInfo &stream, int64_t logicalBlock)
{
	int shift = fGeo.blockShift;
	int64_t pos = logicalBlock << shift;

	// Gate on max_indirect_range, matching BfsReader::ResolveRun and section 7.
	if (stream.maxIndirectRange == 0 || pos < stream.maxDirectRange) {
		int64_t end = 0;
		for (int i = 0; i < kNumDirectBlocks; i++) {
			if (stream.direct[i].IsZero()) {
				break;
			}
			int64_t runBytes = static_cast<int64_t>(stream.direct[i].length) << shift;
			end += runBytes;
			if (end > pos) {
				return fGeo.ToBlock(stream.direct[i]) + ((pos - (end - runBytes)) >> shift);
			}
		}
		throw std::runtime_error("stream position beyond direct range");
	}

	if (stream.maxDoubleIndirectRange == 0 || pos < stream.maxIndirectRange) {
		std::vector<uint8_t> array = ReadRunBytes(stream.indirect);
		int64_t entries = static_cast<int64_t>(array.size()) / 8;
		int64_t end = stream.maxDirectRange;
		for (int64_t j = 0; j < entries; j++) {
			BlockRun run = GetBlockRun(array.data() + j * 8);
			if (run.IsZero()) {
				break;
			}
			int64_t runBytes = static_cast<int64_t>(run.length) << shift;
			end += runBytes;
			if (end > pos) {
				return fGeo.ToBlock(run) + ((pos - (end - runBytes)) >> shift);
			}
		}
		throw std::runtime_error("stream position beyond indirect range");
	}

	int64_t base = stream.doubleIndirect.length;
	int64_t runsPerBlock = fGeo.blockSize / 8;
	int64_t directSize = base << shift;
	int64_t indirectSize = base * directSize * runsPerBlock;
	int64_t start = pos - stream.maxIndirectRange;
	int64_t index = start / indirectSize;

	std::vector<uint8_t> topBlock(fGeo.blockSize);
	fImage.ReadAt(static_cast<uint64_t>(fGeo.ToBlock(stream.doubleIndirect)
		+ index / runsPerBlock) << shift, topBlock.data(), fGeo.blockSize);
	BlockRun secondArray = GetBlockRun(topBlock.data() + (index % runsPerBlock) * 8);

	int64_t current = (start % indirectSize) / directSize;
	std::vector<uint8_t> secondBlock(fGeo.blockSize);
	fImage.ReadAt(static_cast<uint64_t>(fGeo.ToBlock(secondArray)
		+ current / runsPerBlock) << shift, secondBlock.data(), fGeo.blockSize);
	BlockRun run = GetBlockRun(secondBlock.data() + (current % runsPerBlock) * 8);

	int64_t runStart = stream.maxIndirectRange + index * indirectSize
		+ current * directSize;
	return fGeo.ToBlock(run) + ((pos - runStart) >> shift);
}


// Visit every inode-number value stored in a B+tree stream, resolving inline
// leaf values, duplicate fragments, and chained duplicate nodes. Reports each
// value's byte offset within the stream so it can be repointed in place.
void Resizer::ForEachTreeValue(const std::vector<uint8_t> &tree,
	const std::function<void(int64_t, int64_t)> &cb)
{
	if (tree.size() < static_cast<size_t>(btreehdr::kSize)) {
		return;
	}
	const uint8_t *base = tree.data();
	if (GetU32(base + btreehdr::kMagic) != kBPlusTreeMagic) {
		throw std::runtime_error("bad B+tree magic in reference scan");
	}
	int64_t treeSize = static_cast<int64_t>(tree.size());
	int64_t nodeSize = GetU32(base + btreehdr::kNodeSize);
	if (nodeSize <= 0 || nodeSize > treeSize) {
		throw std::runtime_error("bad B+tree node size in reference scan");
	}

	auto nodeAt = [&](int64_t offset) -> const uint8_t * {
		if (offset < 0 || offset + nodeSize > treeSize) {
			throw std::runtime_error("B+tree node link out of range in reference scan");
		}
		return base + offset;
	};
	auto checkSlot = [&](int64_t byteOffset) {
		if (byteOffset < 0 || byteOffset + 8 > treeSize) {
			throw std::runtime_error("B+tree value slot out of range in reference scan");
		}
	};

	// Emit every inode-number slot behind one leaf value link.
	auto emitLeafValue = [&](int64_t link) {
		uint64_t type = static_cast<uint64_t>(link) >> 62;
		if (type == 0) {
			return;   // caller handles the inline slot directly
		}
		int64_t offset = link & 0x3ffffffffffffc00LL;
		if (type == static_cast<uint64_t>(kBPlusTreeDuplicateFragment)) {
			int64_t fragIndex = link & 0x3ff;
			int64_t countOffset = offset + fragIndex
				* static_cast<int64_t>(kNumFragmentValues + 1) * 8;
			checkSlot(countOffset);
			int64_t count = GetS64(base + countOffset);
			if (count < 0 || count > kNumFragmentValues) {
				throw std::runtime_error("duplicate fragment count out of range");
			}
			for (int64_t i = 0; i < count; i++) {
				int64_t slot = countOffset + (1 + i) * 8;
				checkSlot(slot);
				cb(slot, GetS64(base + slot));
			}
		} else if (type == static_cast<uint64_t>(kBPlusTreeDuplicateNode)) {
			int64_t cur = offset;
			int guard = 0;
			while (cur != kBPlusTreeNull) {
				if (++guard > 200000) {
					throw std::runtime_error("duplicate-node chain too long");
				}
				checkSlot(cur + 2 * 8);
				int64_t count = GetS64(base + cur + 2 * 8);
				if (count < 0 || count > kNumDuplicateValues) {
					throw std::runtime_error("duplicate-node count out of range");
				}
				for (int64_t i = 0; i < count; i++) {
					int64_t slot = cur + (3 + i) * 8;
					checkSlot(slot);
					cb(slot, GetS64(base + slot));
				}
				cur = GetS64(base + cur + 1 * 8);   // right_link to the next dup node
			}
		} else {
			throw std::runtime_error("unexpected B+tree duplicate link type");
		}
	};

	// Descend to the leftmost leaf.
	int64_t nodeOffset = GetS64(base + btreehdr::kRootNodePointer);
	for (int guard = 0; guard < 64; guard++) {
		const uint8_t *node = nodeAt(nodeOffset);
		int64_t overflow = GetS64(node + btreenode::kOverflowLink);
		if (overflow == kBPlusTreeNull) {
			break;
		}
		uint16_t keyCount = GetU16(node + btreenode::kAllKeyCount);
		uint16_t keyLength = GetU16(node + btreenode::kAllKeyLength);
		if (keyCount == 0) {
			nodeOffset = overflow;
			continue;
		}
		const uint8_t *values = node + KeyAlign(btreenode::kSize + keyLength)
			+ keyCount * sizeof(uint16_t);
		nodeOffset = GetS64(values);
	}

	// Walk the leaf chain.
	while (nodeOffset != kBPlusTreeNull) {
		const uint8_t *node = nodeAt(nodeOffset);
		uint16_t keyCount = GetU16(node + btreenode::kAllKeyCount);
		uint16_t keyLength = GetU16(node + btreenode::kAllKeyLength);
		int64_t valuesOffset = nodeOffset + KeyAlign(btreenode::kSize + keyLength)
			+ keyCount * sizeof(uint16_t);
		for (uint16_t k = 0; k < keyCount; k++) {
			int64_t slot = valuesOffset + k * sizeof(int64_t);
			checkSlot(slot);
			int64_t link = GetS64(base + slot);
			if ((static_cast<uint64_t>(link) >> 62) == 0) {
				cb(slot, link);   // inline inode number
			} else {
				emitLeafValue(link);
			}
		}
		nodeOffset = GetS64(node + btreenode::kRightLink);
	}
}


int64_t Resizer::CurrentBlock(int64_t originalBlock) const
{
	auto it = fMoved.find(originalBlock);
	return it == fMoved.end() ? originalBlock : it->second;
}


std::vector<uint8_t> Resizer::ReadRunBytes(const BlockRun &run)
{
	std::vector<uint8_t> buffer(static_cast<size_t>(run.length) * fGeo.blockSize);
	int64_t base = fGeo.ToBlock(run);
	for (uint16_t i = 0; i < run.length; i++) {
		fImage.ReadAt(static_cast<uint64_t>(base + i) << fGeo.blockShift,
			buffer.data() + static_cast<size_t>(i) * fGeo.blockSize, fGeo.blockSize);
	}
	return buffer;
}


void Resizer::CopyRun(int64_t oldStart, int64_t newStart, uint16_t length)
{
	std::vector<uint8_t> buffer(fGeo.blockSize);
	for (uint16_t i = 0; i < length; i++) {
		fImage.ReadAt(static_cast<uint64_t>(oldStart + i) << fGeo.blockShift,
			buffer.data(), fGeo.blockSize);
		fImage.WriteAt(static_cast<uint64_t>(newStart + i) << fGeo.blockShift,
			buffer.data(), fGeo.blockSize);
	}
	// The copy targets currently-free blocks; flush it before the descriptor
	// commit so a torn copy can never surface as committed data.
	fImage.Flush();
}


void Resizer::RelocateRun(int64_t ownerBlock, int slotOffset,
	const BlockRun &oldRun)
{
	int64_t length = oldRun.length;
	int64_t newStart = AllocateRun(length, fAllocLow, fAllocHigh);
	if (newStart < 0) {
		throw std::runtime_error("not enough contiguous free space to relocate a "
			"run; shrink/grow less");
	}
	int64_t oldStart = fGeo.ToBlock(oldRun);

	// Copy the run's data to the free target (harmless until referenced).
	CopyRun(oldStart, newStart, static_cast<uint16_t>(length));

	// Free the old run; the new run was marked used by AllocateRun.
	for (int64_t b = oldStart; b < oldStart + length; b++) {
		SetAllocated(b, false);
	}

	// Repoint the block_run stored at (ownerBlock, slotOffset). The owner is the
	// inode for a direct/indirect/double-indirect descriptor, or an array block
	// for a run listed inside it. Reading fImage picks up any earlier committed
	// edits to the same owner (e.g. sibling payload runs already relocated).
	std::vector<uint8_t> block(fGeo.blockSize);
	fImage.ReadAt(static_cast<uint64_t>(ownerBlock) << fGeo.blockShift,
		block.data(), fGeo.blockSize);
	PutBlockRun(block.data() + slotOffset,
		fGeo.ToRun(newStart, static_cast<uint16_t>(length)));

	// Commit atomically: descriptor + bitmap changes together.
	fJournal.Begin();
	StageDirtyBitmap();
	fJournal.Stage(ownerBlock, block.data());
	fJournal.Commit(fImage);

	fRelocated += length;
	if (!fOptions.verbose) {
		fprintf(stderr, "\rmoved %lld blocks", static_cast<long long>(fRelocated));
	} else {
		fprintf(stderr, "moved run %lld+%lld -> %lld (owner %lld)\n",
			static_cast<long long>(oldStart), static_cast<long long>(length),
			static_cast<long long>(newStart), static_cast<long long>(ownerBlock));
	}
}


std::vector<Inode> Resizer::CollectAllInodes()
{
	std::set<int64_t> visited;
	std::vector<Inode> inodes;
	CollectInodes(fGeo.ToBlock(fReader.RootDirectory()), visited, inodes);
	if (!fReader.IndexDirectory().IsZero()) {
		int64_t indexDir = fGeo.ToBlock(fReader.IndexDirectory());
		if (visited.count(indexDir) == 0) {
			visited.insert(indexDir);
			Inode dir = fReader.ReadInode(indexDir);
			inodes.push_back(dir);
			CollectContainerChildren(dir, visited, inodes);
		}
	}
	return inodes;
}


void Resizer::BuildReferenceMap(const std::vector<Inode> &inodes)
{
	fRefs.clear();
	fTreeStreams.clear();

	// A block_run field referencing an inode: patch it when the target moves.
	auto addField = [&](const BlockRun &target, int64_t ownerBlock, int offset) {
		if (target.IsZero()) {
			return;
		}
		fRefs[fGeo.ToBlock(target)].fields.push_back({ownerBlock, offset});
	};

	// Superblock roots (patched inside block 0 at the superblock offset).
	addField(fReader.RootDirectory(), 0,
		static_cast<int>(kSuperBlockOffset) + super::kRootDir);
	addField(fReader.IndexDirectory(), 0,
		static_cast<int>(kSuperBlockOffset) + super::kIndices);

	for (const Inode &inode : inodes) {
		addField(inode.parent, inode.block, inode::kParent);
		addField(inode.attributes, inode.block, inode::kAttributes);

		bool ownsTree = inode.IsDirectory()
			|| (inode.mode & kSAttrDir) != 0
			|| (inode.mode & kSIndexDir) != 0;
		if (!ownsTree) {
			continue;
		}

		std::vector<uint8_t> tree = fReader.ReadStreamFully(inode.stream);
		int treeIndex = static_cast<int>(fTreeStreams.size());
		fTreeStreams.push_back(inode.stream);
		ForEachTreeValue(tree, [&](int64_t logicalOffset, int64_t value) {
			if (value >= 0 && value < fGeo.numBlocks) {
				fRefs[value].treeValues.push_back({treeIndex, logicalOffset});
			}
		});
	}
}


void Resizer::RelocateInode(int64_t oldBlock)
{
	int64_t newBlock = AllocateBlock(fAllocLow, fAllocHigh);
	if (newBlock < 0) {
		throw std::runtime_error("not enough free space to relocate an inode; "
			"shrink/grow less");
	}

	// Buffer every block this move rewrites so repeated edits to the same block
	// (e.g. several tree values in one node) accumulate before staging.
	std::map<int64_t, std::vector<uint8_t>> pending;
	auto load = [&](int64_t block) -> std::vector<uint8_t> & {
		auto it = pending.find(block);
		if (it != pending.end()) {
			return it->second;
		}
		std::vector<uint8_t> buf(fGeo.blockSize);
		fImage.ReadAt(static_cast<uint64_t>(block) << fGeo.blockShift, buf.data(),
			fGeo.blockSize);
		return pending.emplace(block, std::move(buf)).first->second;
	};

	// The relocated inode: a copy of the old block with its own inode_num fixed.
	{
		std::vector<uint8_t> buf(fGeo.blockSize);
		fImage.ReadAt(static_cast<uint64_t>(oldBlock) << fGeo.blockShift, buf.data(),
			fGeo.blockSize);
		PutBlockRun(buf.data() + inode::kInodeNum, fGeo.ToRun(newBlock, 1));
		pending[newBlock] = std::move(buf);
	}

	auto refIt = fRefs.find(oldBlock);
	if (refIt != fRefs.end()) {
		for (const RunFieldRef &field : refIt->second.fields) {
			// A field stored in the block being vacated (e.g. the root directory's
			// parent, which points at itself) lives in the freshly written copy.
			int64_t owner = CurrentBlock(field.block);
			if (owner == oldBlock) {
				owner = newBlock;
			}
			std::vector<uint8_t> &blk = load(owner);
			PutBlockRun(blk.data() + field.offset, fGeo.ToRun(newBlock, 1));
		}
		for (const TreeValueRef &ref : refIt->second.treeValues) {
			const DataStreamInfo &stream = fTreeStreams[ref.treeIndex];
			int64_t phys = StreamPhysicalBlock(stream,
				ref.logicalOffset >> fGeo.blockShift);
			int intra = static_cast<int>(ref.logicalOffset & (fGeo.blockSize - 1));
			std::vector<uint8_t> &blk = load(phys);
			if (GetS64(blk.data() + intra) != oldBlock) {
				throw std::runtime_error("inode reference map inconsistency; run bfscheck");
			}
			PutS64(blk.data() + intra, newBlock);
		}
	}

	SetAllocated(oldBlock, false);
	fMoved[oldBlock] = newBlock;

	// One atomic transaction: new inode + every repointed reference + bitmap.
	fJournal.Begin();
	StageDirtyBitmap();
	for (const auto &entry : pending) {
		fJournal.Stage(entry.first, entry.second.data());
	}
	fJournal.Commit(fImage);

	fRelocated += 1;
	if (!fOptions.verbose) {
		fprintf(stderr, "\rmoved %lld blocks", static_cast<long long>(fRelocated));
	} else {
		fprintf(stderr, "moved inode %lld -> %lld\n",
			static_cast<long long>(oldBlock), static_cast<long long>(newBlock));
	}
}


void Resizer::RelocateInWindow()
{
	// fMoved tracks inode moves within a single Phase B (for references among
	// inodes moved together); it must not carry over between windows.
	fMoved.clear();

	// Phase A: relocate stream data overlapping the window so every affected
	// stream lies outside it and its descriptors are final before any inode
	// header moves.
	std::vector<Inode> inodes = CollectAllInodes();
	for (const Inode &inode : inodes) {
		RelocateStreamTail(inode, /*dryRun=*/false);
	}

	// Phase B: relocate inode headers inside the window, fixing all references.
	// Re-read inodes so tree streams reflect Phase A's descriptor edits (fReader
	// reads straight from fImage, which now holds those committed changes).
	inodes = CollectAllInodes();
	BuildReferenceMap(inodes);
	for (const Inode &inode : inodes) {
		if (inode.block < fWinLo || inode.block >= fWinHi) {
			continue;
		}
		RelocateInode(inode.block);
	}
}


int64_t Resizer::Relocate(int64_t newNum)
{
	fprintf(stderr, "scanning inodes...\n");

	// Shrink evacuates everything from the doomed tail [newNum, numBlocks).
	fWinLo = newNum;
	fWinHi = fGeo.numBlocks;

	if (fOptions.dryRun) {
		std::vector<Inode> inodes = CollectAllInodes();
		int64_t dataBlocks = 0;
		int64_t inodeMoves = 0;
		for (const Inode &inode : inodes) {
			dataBlocks += RelocateStreamTail(inode, /*dryRun=*/true);
			if (inode.block >= newNum) {
				inodeMoves++;
			}
		}
		printf("shrink: %lld -> %lld blocks, %lld data blocks and %lld inodes "
			"would be moved\n",
			static_cast<long long>(fGeo.numBlocks), static_cast<long long>(newNum),
			static_cast<long long>(dataBlocks), static_cast<long long>(inodeMoves));
		return dataBlocks + inodeMoves;
	}

	fAllocLow = Reserved(fGeo.bitmapBlocks);
	fAllocHigh = newNum;
	RelocateInWindow();

	if (fRelocated > 0) {
		fprintf(stderr, "\n");
	}

	for (int64_t block = newNum; block < fGeo.numBlocks; block++) {
		if (Allocated(block)) {
			throw std::runtime_error("tail is still not free after relocation "
				"(unreferenced allocated blocks?); run bfscheck");
		}
	}

	ShrinkFreeTail(newNum);
	return fRelocated;
}


int64_t Resizer::Run(uint64_t newSizeBytes)
{
	if (!fReader.IsClean()) {
		throw std::runtime_error("volume is not cleanly unmounted; mount and unmount "
			"it cleanly before resizing");
	}

	int64_t blockSize = fGeo.blockSize;
	int64_t newNum = static_cast<int64_t>(newSizeBytes / blockSize);
	int64_t oldNum = fGeo.numBlocks;

	if (newNum < 10) {
		throw std::runtime_error("target size is too small");
	}
	if (newNum == oldNum) {
		printf("Volume is already %lld blocks; nothing to do.\n",
			static_cast<long long>(oldNum));
		fJournal.Remove();
		return 0;
	}

	fBitmap.resize(static_cast<size_t>(fGeo.bitmapBlocks) * fGeo.blockSize);
	for (int64_t i = 0; i < fGeo.bitmapBlocks; i++) {
		fImage.ReadAt(static_cast<uint64_t>(1 + i) << fGeo.blockShift,
			fBitmap.data() + static_cast<size_t>(i) * fGeo.blockSize, fGeo.blockSize);
	}

	if (newNum > oldNum) {
		Grow(newNum);
	} else {
		bool tailFree = true;
		for (int64_t block = newNum; block < oldNum; block++) {
			if (Allocated(block)) {
				tailFree = false;
				break;
			}
		}
		if (tailFree) {
			ShrinkFreeTail(newNum);
		} else {
			fRelocated = Relocate(newNum);
		}
	}

	fJournal.Remove();
	if (!fOptions.dryRun) {
		printf("Resized '%s': %lld -> %lld blocks (%lld bytes), %lld blocks moved.\n",
			fImagePath.c_str(), static_cast<long long>(oldNum),
			static_cast<long long>(newNum),
			static_cast<long long>(newNum) * blockSize,
			static_cast<long long>(fRelocated));
	}
	return fRelocated;
}


} // bfs
