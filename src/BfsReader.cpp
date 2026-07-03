#include "BfsReader.h"

#include <string.h>

#include <algorithm>
#include <stdexcept>

#include "Endian.h"
#include "ImageFile.h"


namespace bfs {


namespace {


void RequireMagic(const uint8_t *sb)
{
	if (GetU32(sb + super::kMagic1) != kSuperBlockMagic1
		|| GetU32(sb + super::kMagic2) != kSuperBlockMagic2
		|| GetU32(sb + super::kMagic3) != kSuperBlockMagic3) {
		throw std::runtime_error("not a BFS volume (bad superblock magic)");
	}
	if (GetU32(sb + super::kFsByteOrder) != kSuperBlockFsLendian) {
		throw std::runtime_error(
			"unsupported byte order (only little-endian volumes are supported)");
	}
}


} // unnamed namespace


BfsReader::BfsReader(ImageFile &image):
	fImage(image)
{
	uint8_t sb[512];
	fImage.ReadAt(kSuperBlockOffset, sb, sizeof(sb));
	if (GetU32(sb + super::kMagic1) != kSuperBlockMagic1) {
		// Fall back to a superblock at offset 0 (see section 2).
		fImage.ReadAt(0, sb, sizeof(sb));
	}
	RequireMagic(sb);

	fGeometry.blockSize = GetU32(sb + super::kBlockSize);
	fGeometry.blockShift = GetU32(sb + super::kBlockShift);
	fGeometry.numBlocks = GetS64(sb + super::kNumBlocks);
	fGeometry.agShift = GetS32(sb + super::kAgShift);
	fGeometry.blocksPerAg = GetS32(sb + super::kBlocksPerAg);
	fGeometry.numAgs = GetS32(sb + super::kNumAgs);
	fGeometry.logBlocks = GetBlockRun(sb + super::kLogBlocks);

	if (fGeometry.blockSize == 0
		|| (1u << fGeometry.blockShift) != fGeometry.blockSize
		|| fGeometry.agShift < 1) {
		throw std::runtime_error("invalid superblock geometry");
	}

	int64_t bitsPerBlock = static_cast<int64_t>(fGeometry.blockSize) * 8;
	fGeometry.bitmapBlocks = (fGeometry.numBlocks + bitsPerBlock - 1) / bitsPerBlock;

	fLogStart = GetS64(sb + super::kLogStart);
	fLogEnd = GetS64(sb + super::kLogEnd);
	fRootDir = GetBlockRun(sb + super::kRootDir);
	fIndices = GetBlockRun(sb + super::kIndices);

	size_t nameLen = ::strnlen(reinterpret_cast<const char *>(sb + super::kName),
		kDiskNameLength);
	fName.assign(reinterpret_cast<const char *>(sb + super::kName), nameLen);

	if (static_cast<uint64_t>(fGeometry.numBlocks) << fGeometry.blockShift
			> fImage.Size()) {
		throw std::runtime_error("image is smaller than the volume it declares");
	}
}


void BfsReader::ReadBlock(int64_t block, uint8_t *out)
{
	if (block < 0 || block >= fGeometry.numBlocks) {
		throw std::runtime_error("block number out of range");
	}
	auto it = fOverlay.find(block);
	if (it != fOverlay.end()) {
		::memcpy(out, it->second.data(), fGeometry.blockSize);
		return;
	}
	fImage.ReadAt(static_cast<uint64_t>(block) << fGeometry.blockShift, out,
		fGeometry.blockSize);
}


std::vector<uint8_t> BfsReader::ReadRun(const BlockRun &run)
{
	std::vector<uint8_t> buffer(static_cast<size_t>(run.length) * fGeometry.blockSize);
	int64_t base = fGeometry.ToBlock(run);
	for (uint16_t i = 0; i < run.length; i++) {
		ReadBlock(base + i, buffer.data() + static_cast<size_t>(i) * fGeometry.blockSize);
	}
	return buffer;
}


Inode BfsReader::ReadInode(int64_t block)
{
	Inode inode;
	inode.block = block;
	inode.raw.resize(fGeometry.blockSize);
	ReadBlock(block, inode.raw.data());

	const uint8_t *p = inode.raw.data();
	if (GetU32(p + inode::kMagic1) != kInodeMagic1) {
		throw std::runtime_error("bad inode magic");
	}
	if ((GetU32(p + inode::kFlags) & kInodeInUse) == 0) {
		throw std::runtime_error("inode is not in use");
	}

	inode.mode = GetU32(p + inode::kMode);
	inode.flags = GetU32(p + inode::kFlags);
	inode.uid = GetU32(p + inode::kUid);
	inode.gid = GetU32(p + inode::kGid);
	inode.createTime = GetS64(p + inode::kCreateTime);
	inode.modifiedTime = GetS64(p + inode::kLastModifiedTime);
	inode.statusChangeTime = GetS64(p + inode::kStatusChangeTime);
	inode.type = GetU32(p + inode::kType);
	inode.parent = GetBlockRun(p + inode::kParent);
	inode.attributes = GetBlockRun(p + inode::kAttributes);

	const uint8_t *ds = p + inode::kData;
	for (int i = 0; i < kNumDirectBlocks; i++) {
		inode.stream.direct[i] = GetBlockRun(ds + stream::kDirect + i * 8);
	}
	inode.stream.maxDirectRange = GetS64(ds + stream::kMaxDirectRange);
	inode.stream.indirect = GetBlockRun(ds + stream::kIndirect);
	inode.stream.maxIndirectRange = GetS64(ds + stream::kMaxIndirectRange);
	inode.stream.doubleIndirect = GetBlockRun(ds + stream::kDoubleIndirect);
	inode.stream.maxDoubleIndirectRange = GetS64(ds + stream::kMaxDoubleIndirectRange);
	inode.stream.size = GetS64(ds + stream::kSize);
	return inode;
}


BlockRun BfsReader::ResolveRun(const DataStreamInfo &stream, int64_t pos,
	int64_t &runStart)
{
	uint32_t shift = fGeometry.blockShift;

	if (stream.maxDirectRange == 0 || pos < stream.maxDirectRange) {
		int64_t end = 0;
		for (int i = 0; i < kNumDirectBlocks; i++) {
			if (stream.direct[i].IsZero()) {
				break;
			}
			int64_t runBytes = static_cast<int64_t>(stream.direct[i].length) << shift;
			end += runBytes;
			if (end > pos) {
				runStart = end - runBytes;
				return stream.direct[i];
			}
		}
		throw std::runtime_error("position beyond direct range");
	}

	if (pos < stream.maxIndirectRange) {
		std::vector<uint8_t> array = ReadRun(stream.indirect);
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
				runStart = end - runBytes;
				return run;
			}
		}
		throw std::runtime_error("position beyond indirect range");
	}

	// Double-indirect: fixed-size accounting (see section 7).
	int64_t base = stream.doubleIndirect.length;
	int64_t runsPerBlock = fGeometry.blockSize / 8;
	int64_t directSize = base << shift;
	int64_t indirectSize = base * directSize * runsPerBlock;
	int64_t start = pos - stream.maxIndirectRange;

	int64_t index = start / indirectSize;
	std::vector<uint8_t> topBlock(fGeometry.blockSize);
	ReadBlock(fGeometry.ToBlock(stream.doubleIndirect) + index / runsPerBlock,
		topBlock.data());
	BlockRun secondArray = GetBlockRun(topBlock.data() + (index % runsPerBlock) * 8);

	int64_t current = (start % indirectSize) / directSize;
	std::vector<uint8_t> secondBlock(fGeometry.blockSize);
	ReadBlock(fGeometry.ToBlock(secondArray) + current / runsPerBlock,
		secondBlock.data());
	BlockRun run = GetBlockRun(secondBlock.data() + (current % runsPerBlock) * 8);

	runStart = stream.maxIndirectRange + index * indirectSize + current * directSize;
	return run;
}


void BfsReader::ReadStream(const DataStreamInfo &stream,
	const std::function<void(const uint8_t *, size_t)> &sink)
{
	int64_t size = stream.size;
	if (size <= 0) {
		return;
	}

	// When the journal overlay is empty (the common case) a run's blocks are
	// contiguous on disk, so read them in bounded bulk chunks instead of one
	// block at a time. With an overlay active, fall back to per-block reads so
	// replayed blocks are honored.
	const int64_t chunkCap = 1 << 20;
	int64_t blockSize = fGeometry.blockSize;
	std::vector<uint8_t> buffer(static_cast<size_t>(std::min(chunkCap, size)));
	bool overlayEmpty = fOverlay.empty();
	int64_t pos = 0;

	while (pos < size) {
		int64_t runStart = 0;
		BlockRun run = ResolveRun(stream, pos, runStart);
		int64_t runEnd = std::min(runStart
			+ (static_cast<int64_t>(run.length) << fGeometry.blockShift), size);
		uint64_t runByteBase = static_cast<uint64_t>(fGeometry.ToBlock(run))
			<< fGeometry.blockShift;

		while (pos < runEnd) {
			int64_t chunk = std::min(chunkCap, runEnd - pos);
			if (overlayEmpty) {
				fImage.ReadAt(runByteBase + (pos - runStart), buffer.data(),
					static_cast<size_t>(chunk));
			} else {
				int64_t done = 0;
				while (done < chunk) {
					int64_t offsetInRun = (pos + done) - runStart;
					int64_t physBlock = fGeometry.ToBlock(run)
						+ (offsetInRun >> fGeometry.blockShift);
					int64_t blockOffset = offsetInRun & (blockSize - 1);
					std::vector<uint8_t> blockBuffer(blockSize);
					ReadBlock(physBlock, blockBuffer.data());
					int64_t take = std::min(blockSize - blockOffset, chunk - done);
					::memcpy(buffer.data() + done, blockBuffer.data() + blockOffset,
						static_cast<size_t>(take));
					done += take;
				}
			}
			sink(buffer.data(), static_cast<size_t>(chunk));
			pos += chunk;
		}
	}
}


std::vector<uint8_t> BfsReader::ReadStreamFully(const DataStreamInfo &stream)
{
	std::vector<uint8_t> out;
	out.reserve(static_cast<size_t>(stream.size));
	ReadStream(stream, [&out](const uint8_t *data, size_t length) {
		out.insert(out.end(), data, data + length);
	});
	return out;
}


std::string BfsReader::ReadSymlink(const Inode &inode)
{
	if (inode.IsLongSymlink()) {
		std::vector<uint8_t> data = ReadStreamFully(inode.stream);
		return std::string(data.begin(), data.end());
	}
	const char *text = reinterpret_cast<const char *>(inode.raw.data() + inode::kData);
	size_t maxLen = kShortSymlinkNameLength;
	size_t length = ::strnlen(text, maxLen);
	return std::string(text, length);
}


void BfsReader::IterateDirTree(const std::vector<uint8_t> &tree,
	std::vector<DirEntry> &out)
{
	if (tree.size() < static_cast<size_t>(btreehdr::kSize)) {
		return;
	}
	const uint8_t *base = tree.data();
	if (GetU32(base + btreehdr::kMagic) != kBPlusTreeMagic) {
		throw std::runtime_error("bad B+tree magic");
	}
	int64_t treeSize = static_cast<int64_t>(tree.size());
	int64_t nodeSize = GetU32(base + btreehdr::kNodeSize);
	if (nodeSize <= 0 || nodeSize > treeSize) {
		throw std::runtime_error("bad B+tree node size");
	}

	auto nodeAt = [&](int64_t offset) -> const uint8_t * {
		if (offset < 0 || offset + nodeSize > treeSize) {
			throw std::runtime_error("B+tree node link out of range");
		}
		return base + offset;
	};

	// Descend to the leftmost leaf.
	int64_t nodeOffset = GetS64(base + btreehdr::kRootNodePointer);
	for (int guard = 0; guard < 64; guard++) {
		const uint8_t *node = nodeAt(nodeOffset);
		int64_t overflow = GetS64(node + btreenode::kOverflowLink);
		if (overflow == kBPlusTreeNull) {
			break;   // a leaf
		}
		uint16_t keyCount = GetU16(node + btreenode::kAllKeyCount);
		uint16_t keyLength = GetU16(node + btreenode::kAllKeyLength);
		if (keyCount == 0) {
			nodeOffset = overflow;
			continue;
		}
		const uint8_t *values = node + KeyAlign(btreenode::kSize + keyLength)
			+ keyCount * sizeof(uint16_t);
		nodeOffset = GetS64(values);   // leftmost child
	}

	// Walk the leaf chain left to right.
	while (nodeOffset != kBPlusTreeNull) {
		const uint8_t *node = nodeAt(nodeOffset);
		uint16_t keyCount = GetU16(node + btreenode::kAllKeyCount);
		uint16_t keyLength = GetU16(node + btreenode::kAllKeyLength);
		const uint8_t *keys = node + btreenode::kSize;
		const uint8_t *lengthTable = node + KeyAlign(btreenode::kSize + keyLength);
		const uint8_t *values = lengthTable + keyCount * sizeof(uint16_t);

		if (KeyAlign(btreenode::kSize + keyLength) + keyCount * (2 + 8) > nodeSize) {
			throw std::runtime_error("corrupt B+tree leaf");
		}

		uint16_t previous = 0;
		for (uint16_t k = 0; k < keyCount; k++) {
			uint16_t cumulative = GetU16(lengthTable + k * sizeof(uint16_t));
			uint16_t length = static_cast<uint16_t>(cumulative - previous);
			DirEntry entry;
			entry.name.assign(reinterpret_cast<const char *>(keys + previous), length);
			entry.inode = GetS64(values + k * sizeof(int64_t));
			previous = cumulative;
			if (entry.name != "." && entry.name != "..") {
				out.push_back(std::move(entry));
			}
		}
		nodeOffset = GetS64(node + btreenode::kRightLink);
	}
}


std::vector<DirEntry> BfsReader::ReadDirectory(const Inode &inode)
{
	std::vector<uint8_t> tree = ReadStreamFully(inode.stream);
	std::vector<DirEntry> entries;
	IterateDirTree(tree, entries);
	return entries;
}


void BfsReader::ReadSmallData(const Inode &inode, std::vector<Attribute> &out)
{
	const uint8_t *start = inode.raw.data() + inode::kSmallDataStart;
	const uint8_t *end = inode.raw.data() + inode.raw.size();
	const uint8_t *p = start;

	while (p + 8 <= end) {
		uint32_t type = GetU32(p + 0);
		uint16_t nameSize = GetU16(p + 4);
		uint16_t dataSize = GetU16(p + 6);
		if (nameSize == 0) {
			break;   // end of the region
		}
		const uint8_t *name = p + 8;
		const uint8_t *data = name + nameSize + 3;
		if (data + dataSize + 1 > end) {
			break;   // truncated / corrupt
		}

		bool isNameRecord = nameSize == kFileNameNameLength
			&& name[0] == kFileNameName;
		if (!isNameRecord) {
			Attribute attribute;
			attribute.name.assign(reinterpret_cast<const char *>(name), nameSize);
			attribute.type = type;
			attribute.data.assign(data, data + dataSize);
			out.push_back(std::move(attribute));
		}
		p += 8 + nameSize + 3 + dataSize + 1;
	}
}


std::vector<Attribute> BfsReader::ReadAttributes(const Inode &inode)
{
	std::vector<Attribute> attributes;
	ReadSmallData(inode, attributes);

	if (!inode.attributes.IsZero()) {
		Inode attrDir = ReadInode(fGeometry.ToBlock(inode.attributes));
		for (const DirEntry &entry : ReadDirectory(attrDir)) {
			Inode attrInode = ReadInode(entry.inode);
			Attribute attribute;
			attribute.name = entry.name;
			attribute.type = attrInode.type;
			attribute.data = ReadStreamFully(attrInode.stream);
			attributes.push_back(std::move(attribute));
		}
	}
	return attributes;
}


void BfsReader::ReplayLog()
{
	if (IsClean()) {
		return;
	}

	int64_t logStartBlock = fGeometry.ToBlock(fGeometry.logBlocks);
	int64_t logSize = fGeometry.logBlocks.length;
	uint32_t blockSize = fGeometry.blockSize;

	auto wrap = [&](int64_t position) -> int64_t {
		int64_t relative = (position - logStartBlock) % logSize;
		if (relative < 0) {
			relative += logSize;
		}
		return logStartBlock + relative;
	};
	auto readLogBlock = [&](int64_t position, uint8_t *out) {
		fImage.ReadAt(static_cast<uint64_t>(position) << fGeometry.blockShift, out,
			blockSize);
	};

	std::vector<uint8_t> header(blockSize);
	std::vector<uint8_t> dataBlock(blockSize);
	int64_t position = fLogStart;
	int guard = 0;

	while (position != fLogEnd) {
		if (++guard > logSize + 1) {
			throw std::runtime_error("journal replay did not terminate");
		}
		readLogBlock(wrap(position), header.data());
		int32_t count = GetS32(header.data() + runarray::kCount);
		int64_t maxRuns = (blockSize - runarray::kRuns) / 8;
		if (count < 1 || count > maxRuns) {
			throw std::runtime_error("corrupt journal run_array");
		}

		int64_t dataPosition = wrap(position + 1);
		int64_t totalData = 0;
		for (int32_t i = 0; i < count; i++) {
			BlockRun run = GetBlockRun(header.data() + runarray::kRuns + i * 8);
			int64_t home = fGeometry.ToBlock(run);
			for (uint16_t b = 0; b < run.length; b++) {
				readLogBlock(dataPosition, dataBlock.data());
				fOverlay[home + b] = dataBlock;
				dataPosition = wrap(dataPosition + 1);
				totalData++;
			}
		}
		position = wrap(position + 1 + totalData);
	}
}


} // bfs
