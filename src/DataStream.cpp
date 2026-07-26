#include "DataStream.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <stdexcept>

#include "BlockAllocator.h"
#include "Endian.h"


namespace bfs {


namespace {


int64_t RunsPerBlock(const Geometry &geometry)
{
	return static_cast<int64_t>(geometry.blockSize) / 8;
}


int64_t NextPowerOfTwo(int64_t value)
{
	int64_t power = 1;
	while (power < value) {
		power <<= 1;
	}
	return power;
}


// Does a double-indirect run length 'base' cover at least 'ddBlocks' blocks?
// Capacity is base^3 * runsPerBlock^2 blocks; computed in long double to dodge
// overflow at large bases.
bool DoubleIndirectCapacitySuffices(int64_t base, int64_t rpb, int64_t ddBlocks)
{
	long double capacity = static_cast<long double>(base) * base * base
		* static_cast<long double>(rpb) * rpb;
	return capacity >= static_cast<long double>(ddBlocks);
}


// The fixed double-indirect run length. A power of two so it divides the
// (power-of-two) allocation-group size, which keeps every base-length run inside
// a single group once the run start is base-aligned.
int64_t ChooseBase(int64_t ddBlocks, int64_t rpb, int64_t groupSize,
	int64_t forcedBase)
{
	int64_t cap = std::min<int64_t>(groupSize, 32768);
	if (forcedBase > 0) {
		int64_t base = NextPowerOfTwo(forcedBase);
		return std::min(std::max<int64_t>(base, kNumArrayBlocks), cap);
	}
	int64_t base = kNumArrayBlocks;
	while (base < cap && !DoubleIndirectCapacitySuffices(base, rpb, ddBlocks)) {
		base <<= 1;
	}
	return base;
}


int64_t StreamRunsUpperBound(const Geometry &geometry, int64_t dataBlocks)
{
	int64_t groupSize = geometry.BlocksPerGroup();
	int64_t runs = (dataBlocks + groupSize - 1) / groupSize + 2;
	if (groupSize > kMaxBlockRunLength) {
		runs += (dataBlocks + kMaxBlockRunLength - 1) / kMaxBlockRunLength;
	}
	return runs;
}


int64_t RunBlocks(const std::vector<BlockRun> &runs)
{
	int64_t total = 0;
	for (const BlockRun &run : runs) {
		total += run.length;
	}
	return total;
}


// Allocate 'blocks' more blocks and append them (as AG-split runs) to 'entries'.
// Used for the small alignment fills that keep the double-indirect start and the
// indirect array inside single allocation groups; the filled blocks are real,
// referenced stream blocks (data within size, or zero padding beyond it).
void AppendRuns(BlockAllocator &allocator, std::vector<BlockRun> &entries,
	int64_t blocks)
{
	int64_t consumed = 0;
	std::vector<BlockRun> runs = allocator.AllocateStreamCapped(blocks, -1, consumed);
	entries.insert(entries.end(), runs.begin(), runs.end());
}


} // unnamed namespace


StreamTuning StreamTuning::FromEnvironment(const Geometry &geometry)
{
	StreamTuning tuning;
	tuning.maxIndirectRuns = static_cast<int64_t>(kNumArrayBlocks)
		* RunsPerBlock(geometry);

	if (const char *value = ::getenv("MAKEBFS_MAX_INDIRECT_RUNS")) {
		long parsed = ::strtol(value, nullptr, 10);
		if (parsed >= 0) {
			tuning.maxIndirectRuns = parsed;
		}
	}
	if (const char *value = ::getenv("MAKEBFS_DOUBLE_INDIRECT_BASE")) {
		long parsed = ::strtol(value, nullptr, 10);
		if (parsed > 0) {
			tuning.forcedBase = parsed;
		}
	}
	return tuning;
}


std::vector<BlockRun> StreamLayout::DataRuns() const
{
	std::vector<BlockRun> runs = direct;
	runs.insert(runs.end(), indirectEntries.begin(), indirectEntries.end());
	runs.insert(runs.end(), ddDataRuns.begin(), ddDataRuns.end());
	return runs;
}


int64_t StreamMetadataUpperBound(const Geometry &geometry, int64_t dataBlocks,
	const StreamTuning &tuning)
{
	if (dataBlocks <= 0) {
		return 0;
	}
	int64_t rpb = RunsPerBlock(geometry);
	int64_t groupSize = geometry.BlocksPerGroup();

	int64_t runsUB = StreamRunsUpperBound(geometry, dataBlocks);
	if (runsUB <= kNumDirectBlocks) {
		return 0;   // the direct tier holds every run; no arrays are allocated
	}

	// Indirect array: only the runs past the direct tier are stored in it, so
	// size it from this stream's run count rather than from the tier's capacity
	// (that difference is the bulk of an image's wasted space -- most streams
	// need no array at all). The run is rounded up to a power of two and the
	// cursor is aligned to it, so its region is at most twice the rounded size.
	int64_t indirectRuns = std::min(tuning.maxIndirectRuns, runsUB - kNumDirectBlocks);
	int64_t indirectArray = 0;
	if (indirectRuns > 0) {
		int64_t maxEntries = indirectRuns + 2;   // + up to 2 alignment entries
		int64_t arrayBlocks = (maxEntries + rpb - 1) / rpb;
		indirectArray = 2 * std::min(NextPowerOfTwo(std::max<int64_t>(arrayBlocks, 1)),
			groupSize);
	}

	if (runsUB <= kNumDirectBlocks + tuning.maxIndirectRuns) {
		return indirectArray;   // no double-indirect tier
	}

	// Double-indirect metadata, computed with the same deterministic base Place
	// uses (over-estimating the dd data as all of dataBlocks). '8 * base' slack
	// covers the pre-dd alignment fill, the dd rounding tail, and the small base
	// bump Place may apply to fit the indirect array in one group.
	int64_t base = ChooseBase(dataBlocks, rpb, groupSize, tuning.forcedBase);
	int64_t ddRuns = (dataBlocks + base - 1) / base;
	int64_t entriesPerSecond = base * rpb;
	int64_t numSecond = (ddRuns + entriesPerSecond - 1) / entriesPerSecond;
	int64_t ddMeta = base /* top */ + numSecond * base /* second-level arrays */;
	return indirectArray + ddMeta + 8 * base;
}


StreamLayout BuildStreamLayout(BlockAllocator &allocator, const Geometry &geometry,
	int64_t dataBlocks, uint64_t size, const StreamTuning &tuning)
{
	StreamLayout layout;
	layout.size = size;
	if (dataBlocks <= 0) {
		return layout;
	}

	uint32_t shift = geometry.blockShift;
	int64_t rpb = RunsPerBlock(geometry);
	int64_t groupSize = geometry.BlocksPerGroup();

	// Tier 1+2: direct + indirect from the natural allocation-group-split runs.
	int64_t maxDiRuns = kNumDirectBlocks + tuning.maxIndirectRuns;
	int64_t diConsumed = 0;
	std::vector<BlockRun> diRuns = allocator.AllocateStreamCapped(dataBlocks, maxDiRuns,
		diConsumed);
	int64_t ddBlocks = dataBlocks - diConsumed;

	size_t directCount = std::min(diRuns.size(), static_cast<size_t>(kNumDirectBlocks));
	layout.direct.assign(diRuns.begin(), diRuns.begin() + directCount);
	std::vector<BlockRun> indirectEntries(diRuns.begin() + directCount, diRuns.end());

	// Tier 3: double-indirect for any overflow, as fixed base-length runs.
	if (ddBlocks > 0) {
		int64_t base = ChooseBase(dataBlocks, rpb, groupSize, tuning.forcedBase);
		// Keep the indirect array (<= this many blocks) inside one group by making
		// the base at least as large as the array; the array is placed at the
		// base-aligned cursor left after the double-indirect region.
		int64_t arrayBlocksApprox = (static_cast<int64_t>(indirectEntries.size()) + 2
			+ rpb - 1) / rpb;
		base = std::max(base, NextPowerOfTwo(std::max<int64_t>(arrayBlocksApprox, 1)));
		base = std::min(base, std::min<int64_t>(groupSize, 32768));

		int64_t pad = (base - (allocator.Cursor() % base)) % base;
		if (pad >= ddBlocks) {
			// Everything left fits in the indirect tier once aligned; skip dd.
			AppendRuns(allocator, indirectEntries, ddBlocks);
			ddBlocks = 0;
		} else {
			if (pad > 0) {
				AppendRuns(allocator, indirectEntries, pad);
				ddBlocks -= pad;
			}
			int64_t ddRuns = (ddBlocks + base - 1) / base;   // round up to base
			for (int64_t k = 0; k < ddRuns; k++) {
				int64_t start = allocator.Allocate(base);
				layout.ddDataRuns.push_back(geometry.ToRun(start,
					static_cast<uint16_t>(base)));
			}
			int64_t entriesPerSecond = base * rpb;
			int64_t numSecond = (ddRuns + entriesPerSecond - 1) / entriesPerSecond;
			for (int64_t s = 0; s < numSecond; s++) {
				int64_t start = allocator.Allocate(base);
				layout.ddSecondArrays.push_back(geometry.ToRun(start,
					static_cast<uint16_t>(base)));
			}
			int64_t topStart = allocator.Allocate(base);
			layout.doubleIndirect = geometry.ToRun(topStart,
				static_cast<uint16_t>(base));
			layout.base = base;
		}
	}

	// Place the indirect array (allocated last, so the double-indirect region kept
	// its base-aligned start). Round the run up to a power of two and align the
	// cursor to it so a small array never crosses a group boundary; the alignment
	// fill (needed only when there is no double-indirect tier — the dd path already
	// left the cursor base-aligned) is referenced, beyond-size stream blocks.
	if (!indirectEntries.empty()) {
		int64_t arrayBlocks = (static_cast<int64_t>(indirectEntries.size()) + rpb - 1)
			/ rpb;
		int64_t runBlocks = std::min(NextPowerOfTwo(arrayBlocks), groupSize);
		int64_t pad = (runBlocks - (allocator.Cursor() % runBlocks)) % runBlocks;
		if (pad > 0) {
			AppendRuns(allocator, indirectEntries, pad);
		}
		int64_t start = allocator.Allocate(runBlocks);
		layout.indirect = geometry.ToRun(start, static_cast<uint16_t>(runBlocks));
	}

	layout.indirectEntries = std::move(indirectEntries);

	// Coverage ranges (see BFS_On-Disk_Format.md section 7).
	int64_t directBytes = RunBlocks(layout.direct) << shift;
	int64_t indirectBytes = RunBlocks(layout.indirectEntries) << shift;
	int64_t ddBytes = static_cast<int64_t>(layout.ddDataRuns.size()) * layout.base
		<< shift;

	bool hasIndirect = !layout.indirectEntries.empty();
	bool hasDouble = !layout.ddDataRuns.empty();
	layout.maxDirectRange = directBytes;
	layout.maxIndirectRange = (hasIndirect || hasDouble)
		? directBytes + indirectBytes : 0;
	layout.maxDoubleIndirectRange = hasDouble
		? directBytes + indirectBytes + ddBytes : 0;

	return layout;
}


void WriteDataStream(uint8_t *streamFields, const Geometry &geometry,
	const StreamLayout &layout, ByteOrder order)
{
	::memset(streamFields, 0, 144);

	for (size_t i = 0; i < layout.direct.size(); i++) {
		PutBlockRun(streamFields + stream::kDirect + i * 8, layout.direct[i], order);
	}
	PutBlockRun(streamFields + stream::kIndirect, layout.indirect, order);
	PutBlockRun(streamFields + stream::kDoubleIndirect, layout.doubleIndirect, order);
	PutS64(streamFields + stream::kMaxDirectRange, layout.maxDirectRange, order);
	PutS64(streamFields + stream::kMaxIndirectRange, layout.maxIndirectRange, order);
	PutS64(streamFields + stream::kMaxDoubleIndirectRange,
		layout.maxDoubleIndirectRange, order);
	PutS64(streamFields + stream::kSize, static_cast<int64_t>(layout.size), order);
	(void)geometry;
}


} // bfs
