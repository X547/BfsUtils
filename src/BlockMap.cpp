#include "BlockMap.h"

#include <algorithm>
#include <stdexcept>
#include <utility>


namespace bfs {


const BlockTypeInfo kBlockTypeInfo[kBlockTypeCount] = {
	{"free",       0x24, 0x28, 0x30},
	{"reserved",   0x8a, 0x8f, 0x99},
	{"bitmap",     0x00, 0xa8, 0xa8},
	{"journal",    0xb0, 0x60, 0xff},
	{"inode",      0x4a, 0x90, 0xe2},
	{"metadata",   0x30, 0x54, 0xc8},
	{"index",      0x26, 0xc6, 0xda},
	{"indirect",   0xf5, 0xc5, 0x18},
	{"file data",  0x4c, 0xaf, 0x50},
	{"fragmented", 0xe5, 0x39, 0x35},
	{"attr data",  0xff, 0x98, 0x00},
	{"leaked",     0xd8, 0x1b, 0x60},
};


BlockMap::BlockMap(BfsReader &reader):
	fReader(reader),
	fGeo(reader.GetGeometry()),
	fOrder(reader.Order())
{
}


void BlockMap::SetType(int64_t block, BlockType type)
{
	if (!InRange(block)) {
		return;
	}
	// First assignment wins: the walk visits the most specific owner first, and
	// the reserved regions are marked before the tree walk.
	if (fTypes[block] == kBlockFree) {
		fTypes[block] = static_cast<uint8_t>(type);
	}
}


void BlockMap::LoadBitmap()
{
	fBitmap.assign(static_cast<size_t>(fGeo.bitmapBlocks) * fGeo.blockSize, 0);
	for (int64_t i = 0; i < fGeo.bitmapBlocks; i++) {
		try {
			fReader.ReadBlock(1 + i, fBitmap.data()
				+ static_cast<size_t>(i) * fGeo.blockSize);
		} catch (const std::exception &) {
			// Leave this bitmap block zeroed; leak detection just misses it.
		}
	}
}


bool BlockMap::BitmapAllocated(int64_t block) const
{
	// The bitmap is a sequence of 32-bit words in the volume's byte order; block N
	// is bit (N % 32) of word (N / 32) (see BFS_On-Disk_Format.md, block bitmap).
	size_t wordOffset = static_cast<size_t>(block >> 5) * 4;
	if (wordOffset + 4 > fBitmap.size()) {
		return false;
	}
	uint32_t word = GetU32(fBitmap.data() + wordOffset, fOrder);
	return ((word >> (block & 31)) & 1) != 0;
}


void BlockMap::MarkReserved()
{
	SetType(0, kBlockReserved);   // boot block carries the superblock at offset 512

	int64_t bitmapEnd = std::min<int64_t>(1 + fGeo.bitmapBlocks, fGeo.numBlocks);
	for (int64_t block = 1; block < bitmapEnd; block++) {
		SetType(block, kBlockBitmap);
	}

	int64_t logBase = fGeo.ToBlock(fGeo.logBlocks);
	for (uint16_t i = 0; i < fGeo.logBlocks.length; i++) {
		SetType(logBase + i, kBlockJournal);
	}
}


std::vector<uint8_t> BlockMap::ReadRunBytes(const BlockRun &run)
{
	std::vector<uint8_t> buffer(static_cast<size_t>(run.length) * fGeo.blockSize);
	int64_t base = fGeo.ToBlock(run);
	for (uint16_t i = 0; i < run.length; i++) {
		fReader.ReadBlock(base + i, buffer.data()
			+ static_cast<size_t>(i) * fGeo.blockSize);
	}
	return buffer;
}


void BlockMap::CollectStream(const DataStreamInfo &s,
	std::vector<std::pair<int64_t, int32_t>> &dataRuns,
	std::vector<std::pair<int64_t, int32_t>> &metaRuns)
{
	auto pushData = [&](const BlockRun &run) {
		if (run.length == 0) {
			return false;
		}
		int64_t base = fGeo.ToBlock(run);
		if (base < 0 || base + run.length > fGeo.numBlocks) {
			return false;
		}
		dataRuns.emplace_back(base, run.length);
		return true;
	};
	auto pushMeta = [&](const BlockRun &run) {
		int64_t base = fGeo.ToBlock(run);
		if (base < 0 || base + run.length > fGeo.numBlocks) {
			return false;
		}
		metaRuns.emplace_back(base, run.length);
		return true;
	};

	for (int i = 0; i < kNumDirectBlocks; i++) {
		if (s.direct[i].IsZero()) {
			break;
		}
		pushData(s.direct[i]);
	}

	if (!s.indirect.IsZero() && pushMeta(s.indirect)) {
		try {
			std::vector<uint8_t> array = ReadRunBytes(s.indirect);
			int64_t entries = static_cast<int64_t>(array.size()) / 8;
			for (int64_t j = 0; j < entries; j++) {
				BlockRun run = GetBlockRun(array.data() + j * 8, fOrder);
				if (run.IsZero()) {
					break;
				}
				pushData(run);
			}
		} catch (const std::exception &) {
		}
	}

	if (!s.doubleIndirect.IsZero() && pushMeta(s.doubleIndirect)) {
		try {
			std::vector<uint8_t> top = ReadRunBytes(s.doubleIndirect);
			int64_t topEntries = static_cast<int64_t>(top.size()) / 8;
			for (int64_t t = 0; t < topEntries; t++) {
				BlockRun array = GetBlockRun(top.data() + t * 8, fOrder);
				if (array.IsZero()) {
					break;
				}
				if (!pushMeta(array)) {
					continue;
				}
				std::vector<uint8_t> level = ReadRunBytes(array);
				int64_t entries = static_cast<int64_t>(level.size()) / 8;
				for (int64_t j = 0; j < entries; j++) {
					BlockRun run = GetBlockRun(level.data() + j * 8, fOrder);
					if (run.IsZero()) {
						break;
					}
					pushData(run);
				}
			}
		} catch (const std::exception &) {
		}
	}
}


void BlockMap::WalkInode(int64_t block, InodeRole role)
{
	if (!InRange(block) || block < fReserved) {
		return;
	}
	if (!fVisited.insert(block).second) {
		return;
	}

	bool underIndices = role != kRoleNormal;
	SetType(block, underIndices ? kBlockIndex : kBlockInode);

	Inode inode;
	try {
		inode = fReader.ReadInode(block);
	} catch (const std::exception &) {
		return;   // corrupt inode: it is still counted as an inode block
	}

	std::vector<std::pair<int64_t, int32_t>> dataRuns;
	std::vector<std::pair<int64_t, int32_t>> metaRuns;
	CollectStream(inode.stream, dataRuns, metaRuns);

	// Decide how this inode's own data stream should be colored, and whether its
	// directory entries are child inodes to descend into.
	BlockType dataType;
	bool recurseChildren = false;
	if (underIndices) {
		// The indices directory's entries are indexes (recurse); an individual
		// index's entries are file references, not inodes (do not recurse).
		dataType = kBlockIndex;
		recurseChildren = role == kRoleIndexRoot;
	} else if (inode.IsDirectory()) {
		dataType = kBlockMetadata;
		recurseChildren = true;
	} else if (inode.IsRegularFile()) {
		// A file is fragmented only when its data lands in physically separated
		// pieces. Runs that abut on disk (previous end == next start) are one
		// contiguous fragment even though they are separate block_runs.
		int fragments = dataRuns.empty() ? 0 : 1;
		for (size_t i = 1; i < dataRuns.size(); i++) {
			if (dataRuns[i - 1].first + dataRuns[i - 1].second != dataRuns[i].first) {
				fragments++;
			}
		}
		dataType = fragments > 1 ? kBlockFragmented : kBlockFileData;
	} else {
		// Long symlinks and anything unusual: treat the stream as metadata.
		dataType = kBlockMetadata;
	}

	for (const auto &run : dataRuns) {
		for (int32_t b = 0; b < run.second; b++) {
			SetType(run.first + b, dataType);
		}
	}
	for (const auto &run : metaRuns) {
		for (int32_t b = 0; b < run.second; b++) {
			SetType(run.first + b, kBlockIndirect);
		}
	}

	if (!inode.attributes.IsZero()) {
		WalkAttributeDir(fGeo.ToBlock(inode.attributes));
	}

	if (recurseChildren) {
		std::vector<DirEntry> entries;
		try {
			entries = fReader.ReadDirectory(inode);
		} catch (const std::exception &) {
			return;
		}
		for (const DirEntry &entry : entries) {
			WalkInode(entry.inode, role == kRoleIndexRoot ? kRoleIndex : role);
		}
	}
}


void BlockMap::WalkAttributeDir(int64_t block)
{
	if (!InRange(block) || block < fReserved) {
		return;
	}
	if (!fVisited.insert(block).second) {
		return;
	}
	SetType(block, kBlockInode);

	Inode dir;
	try {
		dir = fReader.ReadInode(block);
	} catch (const std::exception &) {
		return;
	}

	std::vector<std::pair<int64_t, int32_t>> dataRuns;
	std::vector<std::pair<int64_t, int32_t>> metaRuns;
	CollectStream(dir.stream, dataRuns, metaRuns);
	for (const auto &run : dataRuns) {
		for (int32_t b = 0; b < run.second; b++) {
			SetType(run.first + b, kBlockMetadata);
		}
	}
	for (const auto &run : metaRuns) {
		for (int32_t b = 0; b < run.second; b++) {
			SetType(run.first + b, kBlockIndirect);
		}
	}

	std::vector<DirEntry> entries;
	try {
		entries = fReader.ReadDirectory(dir);
	} catch (const std::exception &) {
		return;
	}
	for (const DirEntry &entry : entries) {
		if (!InRange(entry.inode) || entry.inode < fReserved) {
			continue;
		}
		if (!fVisited.insert(entry.inode).second) {
			continue;
		}
		SetType(entry.inode, kBlockInode);
		Inode attrInode;
		try {
			attrInode = fReader.ReadInode(entry.inode);
		} catch (const std::exception &) {
			continue;
		}
		std::vector<std::pair<int64_t, int32_t>> attrData;
		std::vector<std::pair<int64_t, int32_t>> attrMeta;
		CollectStream(attrInode.stream, attrData, attrMeta);
		for (const auto &run : attrData) {
			for (int32_t b = 0; b < run.second; b++) {
				SetType(run.first + b, kBlockAttrData);
			}
		}
		for (const auto &run : attrMeta) {
			for (int32_t b = 0; b < run.second; b++) {
				SetType(run.first + b, kBlockIndirect);
			}
		}
	}
}


void BlockMap::Build()
{
	fReserved = fReader.ReservedBlocks();
	fTypes.assign(static_cast<size_t>(fGeo.numBlocks), kBlockFree);

	LoadBitmap();
	MarkReserved();

	if (!fReader.RootDirectory().IsZero()) {
		WalkInode(fGeo.ToBlock(fReader.RootDirectory()), kRoleNormal);
	}
	if (!fReader.IndexDirectory().IsZero()) {
		WalkInode(fGeo.ToBlock(fReader.IndexDirectory()), kRoleIndexRoot);
	}

	// Anything still unclassified but marked used in the bitmap is a leak or an
	// object we could not reach from the root.
	for (int64_t block = 0; block < fGeo.numBlocks; block++) {
		if (fTypes[block] == kBlockFree && BitmapAllocated(block)) {
			fTypes[block] = kBlockLeaked;
		}
	}

	for (int i = 0; i < kBlockTypeCount; i++) {
		fCounts[i] = 0;
	}
	for (int64_t block = 0; block < fGeo.numBlocks; block++) {
		fCounts[fTypes[block]]++;
	}
}


} // bfs
