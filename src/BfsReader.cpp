#include "BfsReader.h"

#include <string.h>

#include <algorithm>
#include <stdexcept>

#include "BPlusTreeReader.h"
#include "Endian.h"
#include "ImageFile.h"


namespace bfs {


namespace {


// Read the superblock into 'sb', following the offset-512-then-0 fallback, and
// determine the volume's byte order from magic1 (see section 2 and "Byte order").
ByteOrder ReadSuperBlock(ImageFile &image, uint8_t *sb)
{
	ByteOrder order;
	image.ReadAt(kSuperBlockOffset, sb, 512);
	if (!DetectByteOrder(sb + super::kMagic1, order)) {
		// Fall back to a superblock at offset 0 (see section 2).
		image.ReadAt(0, sb, 512);
		if (!DetectByteOrder(sb + super::kMagic1, order)) {
			throw std::runtime_error("not a BFS volume (bad superblock magic)");
		}
	}
	return order;
}


// Wording for the tree problems this reader treats as fatal. A malformed key
// table is reported as a corrupt leaf: to a caller reading directory entries
// that is exactly what it means, and stopping beats handing back the garbage
// names a nonsensical table decodes to.
const char *TreeErrorMessage(TreeStatus status)
{
	switch (status) {
		case TreeStatus::BadMagic:
			return "bad B+tree magic";
		case TreeStatus::BadNodeSize:
			return "bad B+tree node size";
		case TreeStatus::LinkOutOfRange:
			return "B+tree node link out of range";
		default:
			return "corrupt B+tree leaf";
	}
}


} // unnamed namespace


BfsReader::BfsReader(ImageFile &image):
	fImage(image)
{
	uint8_t sb[512];
	fOrder = ReadSuperBlock(fImage, sb);

	if (U32(sb + super::kMagic2) != kSuperBlockMagic2
		|| U32(sb + super::kMagic3) != kSuperBlockMagic3) {
		throw std::runtime_error("not a BFS volume (bad superblock magic)");
	}
	// The fs_byte_order marker holds the integer 'BIGE' in the volume's own byte
	// order; read with fOrder it must come back as that value on either layout.
	if (U32(sb + super::kFsByteOrder) != kSuperBlockFsLendian) {
		throw std::runtime_error("superblock byte-order marker is inconsistent");
	}

	ParseSuperBlock(sb, true);

	if (static_cast<uint64_t>(fGeometry.numBlocks) << fGeometry.blockShift
			> fImage.Size()) {
		throw std::runtime_error("image is smaller than the volume it declares");
	}
}


void BfsReader::ParseSuperBlock(const uint8_t *sb, bool full)
{
	if (full) {
		fGeometry.blockSize = U32(sb + super::kBlockSize);
		fGeometry.blockShift = U32(sb + super::kBlockShift);
		fGeometry.agShift = S32(sb + super::kAgShift);
		fGeometry.blocksPerAg = S32(sb + super::kBlocksPerAg);

		if (fGeometry.blockSize == 0
			|| (1u << fGeometry.blockShift) != fGeometry.blockSize
			|| fGeometry.agShift < 1) {
			throw std::runtime_error("invalid superblock geometry");
		}

		size_t nameLen = ::strnlen(reinterpret_cast<const char *>(sb + super::kName),
			kDiskNameLength);
		fName.assign(reinterpret_cast<const char *>(sb + super::kName), nameLen);
	}

	fGeometry.numBlocks = S64(sb + super::kNumBlocks);
	fGeometry.numAgs = S32(sb + super::kNumAgs);
	fGeometry.logBlocks = Run(sb + super::kLogBlocks);

	int64_t bitsPerBlock = static_cast<int64_t>(fGeometry.blockSize) * 8;
	fGeometry.bitmapBlocks = (fGeometry.numBlocks + bitsPerBlock - 1) / bitsPerBlock;

	fInodeSize = U32(sb + super::kInodeSize);
	fFlags = U32(sb + super::kFlags);
	fUsedBlocks = S64(sb + super::kUsedBlocks);
	fLogStart = S64(sb + super::kLogStart);
	fLogEnd = S64(sb + super::kLogEnd);
	fRootDir = Run(sb + super::kRootDir);
	fIndices = Run(sb + super::kIndices);
}


void BfsReader::ReloadSuperBlock()
{
	uint8_t sb[512];
	fImage.ReadAt(kSuperBlockOffset, sb, sizeof(sb));
	if (U32(sb + super::kMagic1) != kSuperBlockMagic1) {
		fImage.ReadAt(0, sb, sizeof(sb));
	}
	if (U32(sb + super::kMagic1) != kSuperBlockMagic1
		|| U32(sb + super::kMagic2) != kSuperBlockMagic2
		|| U32(sb + super::kMagic3) != kSuperBlockMagic3) {
		throw std::runtime_error("not a BFS volume (bad superblock magic)");
	}

	ParseSuperBlock(sb, false);
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
	if (U32(p + inode::kMagic1) != kInodeMagic1) {
		throw std::runtime_error("bad inode magic");
	}
	if ((U32(p + inode::kFlags) & kInodeInUse) == 0) {
		throw std::runtime_error("inode is not in use");
	}

	inode.mode = U32(p + inode::kMode);
	inode.flags = U32(p + inode::kFlags);
	inode.uid = U32(p + inode::kUid);
	inode.gid = U32(p + inode::kGid);
	inode.createTime = S64(p + inode::kCreateTime);
	inode.modifiedTime = S64(p + inode::kLastModifiedTime);
	inode.statusChangeTime = S64(p + inode::kStatusChangeTime);
	inode.type = U32(p + inode::kType);
	inode.parent = Run(p + inode::kParent);
	inode.attributes = Run(p + inode::kAttributes);

	const uint8_t *ds = p + inode::kData;
	for (int i = 0; i < kNumDirectBlocks; i++) {
		inode.stream.direct[i] = Run(ds + stream::kDirect + i * 8);
	}
	inode.stream.maxDirectRange = S64(ds + stream::kMaxDirectRange);
	inode.stream.indirect = Run(ds + stream::kIndirect);
	inode.stream.maxIndirectRange = S64(ds + stream::kMaxIndirectRange);
	inode.stream.doubleIndirect = Run(ds + stream::kDoubleIndirect);
	inode.stream.maxDoubleIndirectRange = S64(ds + stream::kMaxDoubleIndirectRange);
	inode.stream.size = S64(ds + stream::kSize);
	return inode;
}


BlockRun BfsReader::ResolveRun(const DataStreamInfo &stream, int64_t pos,
	int64_t &runStart)
{
	uint32_t shift = fGeometry.blockShift;

	// The tier gate keys on max_indirect_range, not max_direct_range (see
	// section 7): a stream with no indirect tier resolves everything in the
	// direct runs whatever max_direct_range says.
	if (stream.maxIndirectRange == 0 || pos < stream.maxDirectRange) {
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

	if (stream.maxDoubleIndirectRange == 0 || pos < stream.maxIndirectRange) {
		std::vector<uint8_t> array = ReadRun(stream.indirect);
		int64_t entries = static_cast<int64_t>(array.size()) / 8;
		int64_t end = stream.maxDirectRange;
		for (int64_t j = 0; j < entries; j++) {
			BlockRun run = Run(array.data() + j * 8);
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
	BlockRun secondArray = Run(topBlock.data() + (index % runsPerBlock) * 8);

	int64_t current = (start % indirectSize) / directSize;
	std::vector<uint8_t> secondBlock(fGeometry.blockSize);
	ReadBlock(fGeometry.ToBlock(secondArray) + current / runsPerBlock,
		secondBlock.data());
	BlockRun run = Run(secondBlock.data() + (current % runsPerBlock) * 8);

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
	BPlusTreeReader treeReader(tree, fOrder);

	// A stream too small to hold a header is an empty tree, not a broken one.
	TreeStatus status = treeReader.Open();
	if (status == TreeStatus::StreamTooSmall) {
		return;
	}
	if (status != TreeStatus::Ok) {
		throw std::runtime_error(TreeErrorMessage(status));
	}

	status = treeReader.ForEachLeafEntry([&out](const TreeKey &key) {
		DirEntry entry;
		entry.name.assign(reinterpret_cast<const char *>(key.data), key.length);
		entry.inode = key.value;
		if (entry.name != "." && entry.name != "..") {
			out.push_back(std::move(entry));
		}
		return true;
	});
	if (status != TreeStatus::Ok) {
		throw std::runtime_error(TreeErrorMessage(status));
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
		uint32_t type = U32(p + 0);
		uint16_t nameSize = U16(p + 4);
		uint16_t dataSize = U16(p + 6);
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
		int32_t count = S32(header.data() + runarray::kCount);
		int64_t maxRuns = (blockSize - runarray::kRuns) / 8;
		if (count < 1 || count > maxRuns) {
			throw std::runtime_error("corrupt journal run_array");
		}

		int64_t dataPosition = wrap(position + 1);
		int64_t totalData = 0;
		for (int32_t i = 0; i < count; i++) {
			BlockRun run = Run(header.data() + runarray::kRuns + i * 8);
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
