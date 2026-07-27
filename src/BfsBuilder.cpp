#include "BfsBuilder.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "BlockAllocator.h"
#include "DataStream.h"
#include "Endian.h"
#include "ImageFile.h"
#include "SmallData.h"
#include "TimeEncoding.h"


namespace bfs {


namespace {


uint32_t Log2Exact(uint32_t value)
{
	uint32_t shift = 0;
	while ((1u << shift) < value) {
		shift++;
	}
	return shift;
}


int CompareStringKey(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
	size_t common = std::min(a.size(), b.size());
	int c = common == 0 ? 0 : ::memcmp(a.data(), b.data(), common);
	if (c != 0) {
		return c;
	}
	if (a.size() < b.size()) {
		return -1;
	}
	if (a.size() > b.size()) {
		return 1;
	}
	return 0;
}


std::vector<uint8_t> Int64Key(int64_t value, ByteOrder order)
{
	std::vector<uint8_t> key(8);
	PutS64(key.data(), value, order);
	return key;
}


int64_t ChooseLogBlocks(uint64_t volumeBytes)
{
	if (volumeBytes < (8ull << 20)) {
		return 512;
	}
	if (volumeBytes < (1ull << 30)) {
		return 2048;
	}
	return 4096;
}


void WriteBytesStream(ImageFile &image, const Geometry &geometry,
	const std::vector<BlockRun> &runs, const uint8_t *data, uint64_t size,
	Progress &progress)
{
	uint32_t blockSize = geometry.blockSize;
	std::vector<uint8_t> block(blockSize);
	uint64_t offset = 0;

	for (const BlockRun &run : runs) {
		int64_t startBlock = geometry.ToBlock(run);
		for (uint16_t b = 0; b < run.length; b++) {
			::memset(block.data(), 0, blockSize);
			uint64_t remaining = size > offset ? size - offset : 0;
			size_t take = remaining < blockSize ? static_cast<size_t>(remaining) : blockSize;
			if (take > 0) {
				::memcpy(block.data(), data + offset, take);
			}
			image.WriteBlock(startBlock + b, block.data());
			progress.Advance(blockSize);
			offset += blockSize;
		}
	}
}


// Write one contiguous array run (indirect array, double-indirect top array, or a
// second-level array) filled with 'count' block_run entries and zero-padded.
void WriteRunArray(ImageFile &image, const Geometry &geometry,
	const BlockRun &arrayRun, const BlockRun *entries, size_t count, ByteOrder order,
	Progress &progress)
{
	uint32_t blockSize = geometry.blockSize;
	std::vector<uint8_t> buffer(static_cast<size_t>(arrayRun.length) * blockSize, 0);
	for (size_t i = 0; i < count; i++) {
		PutBlockRun(buffer.data() + i * 8, entries[i], order);
	}
	int64_t start = geometry.ToBlock(arrayRun);
	for (uint16_t b = 0; b < arrayRun.length; b++) {
		image.WriteBlock(start + b, buffer.data() + static_cast<size_t>(b) * blockSize);
		progress.Advance(blockSize);
	}
}


// Serialize the indirect / double-indirect array blocks referenced by a layout.
void WriteArrayBlocks(ImageFile &image, const Geometry &geometry,
	const StreamLayout &layout, ByteOrder order, Progress &progress)
{
	if (!layout.indirect.IsZero()) {
		WriteRunArray(image, geometry, layout.indirect,
			layout.indirectEntries.data(), layout.indirectEntries.size(), order,
			progress);
	}
	if (!layout.doubleIndirect.IsZero()) {
		WriteRunArray(image, geometry, layout.doubleIndirect,
			layout.ddSecondArrays.data(), layout.ddSecondArrays.size(), order,
			progress);
		int64_t entriesPerSecond = layout.base * (static_cast<int64_t>(geometry.blockSize) / 8);
		size_t total = layout.ddDataRuns.size();
		for (size_t s = 0; s < layout.ddSecondArrays.size(); s++) {
			size_t offset = static_cast<size_t>(s * entriesPerSecond);
			size_t remaining = offset < total ? total - offset : 0;
			size_t count = std::min(remaining, static_cast<size_t>(entriesPerSecond));
			WriteRunArray(image, geometry, layout.ddSecondArrays[s],
				layout.ddDataRuns.data() + offset, count, order, progress);
		}
	}
}


// The scanned tree's size, which is the denominator of the planning phase.
// Attribute directories and index inodes are planned on top of this, so it is a
// lower bound on the inodes eventually created, not on the PlanNode calls it
// actually counts.
int64_t CountNodes(const Node &node)
{
	int64_t count = 1;
	for (const std::unique_ptr<Node> &child : node.children) {
		count += CountNodes(*child);
	}
	return count;
}


// Blocks WriteArrayBlocks will emit for a layout: the indirect array, the
// double-indirect top array, and every second-level array under it.
int64_t ArrayBlockCount(const StreamLayout &layout)
{
	int64_t blocks = layout.indirect.length + layout.doubleIndirect.length;
	for (const BlockRun &run : layout.ddSecondArrays) {
		blocks += run.length;
	}
	return blocks;
}


void WriteFileStream(ImageFile &image, const Geometry &geometry,
	const std::vector<BlockRun> &runs, const std::string &path, uint64_t size,
	Progress &progress)
{
	int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0) {
		throw std::system_error(errno, std::generic_category(), "open " + path);
	}

	uint32_t blockSize = geometry.blockSize;
	std::vector<uint8_t> block(blockSize);
	uint64_t offset = 0;

	for (const BlockRun &run : runs) {
		int64_t startBlock = geometry.ToBlock(run);
		for (uint16_t b = 0; b < run.length; b++) {
			::memset(block.data(), 0, blockSize);
			uint64_t remaining = size > offset ? size - offset : 0;
			size_t want = remaining < blockSize ? static_cast<size_t>(remaining) : blockSize;
			size_t got = 0;
			while (got < want) {
				ssize_t r = ::read(fd, block.data() + got, want - got);
				if (r < 0) {
					if (errno == EINTR) {
						continue;
					}
					int err = errno;
					::close(fd);
					throw std::system_error(err, std::generic_category(), "read " + path);
				}
				if (r == 0) {
					break;   // file shrank since scan; pad with zeros
				}
				got += static_cast<size_t>(r);
			}
			image.WriteBlock(startBlock + b, block.data());
			progress.Advance(blockSize);
			offset += blockSize;
		}
	}

	::close(fd);
}


} // unnamed namespace


BfsBuilder::BfsBuilder(const BuildOptions &options, Progress &progress):
	fOptions(options),
	fProgress(progress)
{
}


int BfsBuilder::NewInode()
{
	fInodes.emplace_back();
	return static_cast<int>(fInodes.size()) - 1;
}


int64_t BfsBuilder::ParentBlock(const InodePlan &plan) const
{
	if (plan.parentPlan < 0) {
		return plan.block;
	}
	return fInodes[plan.parentPlan].block;
}


int64_t BfsBuilder::FirstFreeBlock() const
{
	return 1 + fGeometry.bitmapBlocks + fGeometry.logBlocks.length;
}


int BfsBuilder::PlanNode(Node &node, int parentPlan, bool isRoot)
{
	fProgress.Advance();

	int index = NewInode();
	node.inodePlanIndex = index;
	size_t capacity = fOptions.blockSize - inode::kSmallDataStart;

	{
		InodePlan &plan = fInodes[index];
		plan.uid = node.uid;
		plan.gid = node.gid;
		plan.createTime = node.createTime;
		plan.modifiedTime = node.modifiedTime;
		plan.changeTime = node.changeTime;
		plan.parentPlan = isRoot ? -1 : parentPlan;
	}

	SmallDataResult smallData = BuildSmallData(node.name, node.attributes, capacity, fOptions.byteOrder);
	fInodes[index].smallData = std::move(smallData.bytes);
	if (!smallData.largeAttributes.empty()) {
		int attrDir = PlanAttributeDir(node, index, smallData.largeAttributes);
		fInodes[index].attrDirPlan = attrDir;
	}

	switch (node.kind) {
		case NodeKind::Directory: {
			// A directory's contents are a string-keyed B+tree, so it carries
			// S_STR_INDEX; Haiku derives the tree's key type from this bit.
			fInodes[index].mode = kSIfdir | kSStrIndex | node.posixMode;

			for (auto &child : node.children) {
				PlanNode(*child, index, false);
			}

			// Real BFS directories store "." (self) and ".." (parent) as tree
			// entries. They are NOT always first: a name whose bytes sort below
			// '.' (0x2e) — e.g. '!', '#', '+', '-' — precedes them, so the whole
			// entry set (including "." / "..") must be sorted together. Feeding
			// unsorted keys to the B+tree builder produces mis-ordered nodes that
			// crash Haiku's binary search.
			struct DirEntry {
				std::vector<uint8_t> key;
				int planIndex;
			};
			std::vector<DirEntry> entries;
			entries.push_back({{'.'}, index});
			entries.push_back({{'.', '.'}, isRoot ? index : parentPlan});
			for (auto &child : node.children) {
				entries.push_back({std::vector<uint8_t>(
					child->name.begin(), child->name.end()), child->inodePlanIndex});
			}
			std::sort(entries.begin(), entries.end(),
				[](const DirEntry &a, const DirEntry &b) {
					return CompareStringKey(a.key, b.key) < 0;
				});

			auto tree = std::make_shared<BPlusTreeBuilder>(kBPlusTreeStringType, fOptions.byteOrder);
			std::vector<std::vector<int>> groups;
			for (const DirEntry &entry : entries) {
				tree->AddKey(entry.key.data(),
					static_cast<uint16_t>(entry.key.size()), 1);
				groups.push_back({entry.planIndex});
			}
			tree->Finalize();

			InodePlan &plan = fInodes[index];
			plan.streamKind = StreamKind::Tree;
			plan.tree = tree;
			plan.treeGroups = std::move(groups);
			plan.streamSize = tree->StreamSize();
			plan.reportedSize = static_cast<int64_t>(plan.streamSize);
			break;
		}
		case NodeKind::File: {
			InodePlan &plan = fInodes[index];
			plan.mode = kSIfreg | node.posixMode;
			plan.streamKind = StreamKind::File;
			plan.filePath = node.sourcePath;
			plan.streamSize = node.dataSize;
			plan.reportedSize = static_cast<int64_t>(node.dataSize);
			break;
		}
		case NodeKind::Symlink: {
			InodePlan &plan = fInodes[index];
			plan.mode = kSIflnk | node.posixMode;
			plan.reportedSize = static_cast<int64_t>(node.linkTarget.size());
			if (node.linkTarget.size() < static_cast<size_t>(kShortSymlinkNameLength)) {
				plan.streamKind = StreamKind::ShortSymlink;
				plan.shortSymlink = node.linkTarget;
			} else {
				plan.streamKind = StreamKind::Bytes;
				plan.bytes.assign(node.linkTarget.begin(), node.linkTarget.end());
				plan.streamSize = node.linkTarget.size();
				plan.flags |= kInodeLongSymlink;
			}
			break;
		}
	}

	if (!isRoot) {
		fContentNodes.push_back(&node);
	}
	return index;
}


int BfsBuilder::PlanAttributeDir(Node &node, int ownerPlan,
	const std::vector<const Attribute *> &largeAttributes)
{
	size_t capacity = fOptions.blockSize - inode::kSmallDataStart;

	int dirIndex = NewInode();
	{
		InodePlan &plan = fInodes[dirIndex];
		plan.mode = kSAttrDir | kSIfdir | kSStrIndex | 0755u;
		plan.uid = node.uid;
		plan.gid = node.gid;
		plan.createTime = node.createTime;
		plan.modifiedTime = node.modifiedTime;
		plan.changeTime = node.changeTime;
		plan.parentPlan = ownerPlan;
		plan.smallData = BuildSmallData(node.name, {}, capacity, fOptions.byteOrder).bytes;
	}

	std::vector<const Attribute *> sorted(largeAttributes.begin(), largeAttributes.end());
	std::sort(sorted.begin(), sorted.end(),
		[](const Attribute *a, const Attribute *b) {
			std::vector<uint8_t> ka(a->name.begin(), a->name.end());
			std::vector<uint8_t> kb(b->name.begin(), b->name.end());
			return CompareStringKey(ka, kb) < 0;
		});

	auto tree = std::make_shared<BPlusTreeBuilder>(kBPlusTreeStringType, fOptions.byteOrder);
	std::vector<std::vector<int>> groups;
	for (const Attribute *attribute : sorted) {
		int attrIndex = NewInode();
		InodePlan &plan = fInodes[attrIndex];
		plan.mode = kSAttr | 0644u;
		plan.flags = kInodeInUse;
		plan.uid = node.uid;
		plan.gid = node.gid;
		plan.createTime = node.createTime;
		plan.modifiedTime = node.modifiedTime;
		plan.changeTime = node.changeTime;
		plan.type = attribute->type;
		plan.parentPlan = dirIndex;
		plan.smallData = BuildSmallData(attribute->name, {}, capacity, fOptions.byteOrder).bytes;
		plan.streamKind = StreamKind::Bytes;
		plan.bytes = attribute->data;
		plan.streamSize = attribute->data.size();
		plan.reportedSize = static_cast<int64_t>(attribute->data.size());

		tree->AddKey(reinterpret_cast<const uint8_t *>(attribute->name.data()),
			static_cast<uint16_t>(attribute->name.size()), 1);
		groups.push_back({attrIndex});
	}
	tree->Finalize();

	InodePlan &plan = fInodes[dirIndex];
	plan.streamKind = StreamKind::Tree;
	plan.tree = tree;
	plan.treeGroups = std::move(groups);
	plan.streamSize = tree->StreamSize();
	plan.reportedSize = static_cast<int64_t>(plan.streamSize);
	return dirIndex;
}


int BfsBuilder::PlanIndex(const std::string &name, uint32_t dataType,
	uint32_t modeTypeBit, int parentPlan,
	std::vector<std::pair<std::vector<uint8_t>, Node *>> &inputs)
{
	bool numeric = dataType == kBPlusTreeInt64Type
		|| dataType == kBPlusTreeUInt64Type
		|| dataType == kBPlusTreeInt32Type
		|| dataType == kBPlusTreeUInt32Type;

	// Numeric index keys were encoded in the volume's byte order (see Int64Key),
	// so decode them the same way to sort by value; the checker likewise compares
	// int keys numerically.
	ByteOrder order = fOptions.byteOrder;
	std::sort(inputs.begin(), inputs.end(),
		[numeric, order](const std::pair<std::vector<uint8_t>, Node *> &a,
			const std::pair<std::vector<uint8_t>, Node *> &b) {
			if (numeric) {
				return GetS64(a.first.data(), order) < GetS64(b.first.data(), order);
			}
			return CompareStringKey(a.first, b.first) < 0;
		});

	size_t capacity = fOptions.blockSize - inode::kSmallDataStart;
	auto tree = std::make_shared<BPlusTreeBuilder>(dataType, fOptions.byteOrder);
	std::vector<std::vector<int>> groups;

	size_t i = 0;
	while (i < inputs.size()) {
		size_t j = i + 1;
		while (j < inputs.size() && inputs[j].first == inputs[i].first) {
			j++;
		}
		tree->AddKey(inputs[i].first.data(),
			static_cast<uint16_t>(inputs[i].first.size()),
			static_cast<uint32_t>(j - i));
		std::vector<int> group;
		for (size_t k = i; k < j; k++) {
			group.push_back(inputs[k].second->inodePlanIndex);
		}
		groups.push_back(std::move(group));
		i = j;
	}
	tree->Finalize();

	int index = NewInode();
	InodePlan &plan = fInodes[index];
	plan.mode = kSIndexDir | modeTypeBit | kSIfdir;
	plan.uid = fInodes[fRootPlan].uid;
	plan.gid = fInodes[fRootPlan].gid;
	plan.createTime = fInodes[fRootPlan].createTime;
	plan.modifiedTime = fInodes[fRootPlan].modifiedTime;
	plan.changeTime = fInodes[fRootPlan].changeTime;
	plan.parentPlan = parentPlan;
	plan.smallData = BuildSmallData(name, {}, capacity, fOptions.byteOrder).bytes;
	plan.streamKind = StreamKind::Tree;
	plan.tree = tree;
	plan.treeGroups = std::move(groups);
	plan.streamSize = tree->StreamSize();
	plan.reportedSize = static_cast<int64_t>(plan.streamSize);
	return index;
}


void BfsBuilder::PlanIndices()
{
	if (!fOptions.generateIndices) {
		return;
	}

	size_t capacity = fOptions.blockSize - inode::kSmallDataStart;

	// The index directory is created first so each index can name it as parent.
	fIndexDirPlan = NewInode();
	{
		InodePlan &plan = fInodes[fIndexDirPlan];
		// The index directory's tree is keyed by index name (a string), so it
		// needs S_STR_INDEX just like a regular directory; without a key-type
		// bit Haiku rejects an S_INDEX_DIR inode's tree as corrupt. The
		// permission bits are what tell the index root apart from an
		// individual index, which carries none (see section 6/13).
		plan.mode = kSIndexDir | kSStrIndex | kSIfdir | 0700u;
		plan.uid = fInodes[fRootPlan].uid;
		plan.gid = fInodes[fRootPlan].gid;
		plan.createTime = fInodes[fRootPlan].createTime;
		plan.modifiedTime = fInodes[fRootPlan].modifiedTime;
		plan.changeTime = fInodes[fRootPlan].changeTime;
		plan.parentPlan = fRootPlan;
		plan.smallData = BuildSmallData("indices", {}, capacity, fOptions.byteOrder).bytes;
	}

	std::vector<std::pair<std::vector<uint8_t>, Node *>> nameInputs;
	std::vector<std::pair<std::vector<uint8_t>, Node *>> sizeInputs;
	std::vector<std::pair<std::vector<uint8_t>, Node *>> mtimeInputs;
	std::vector<std::pair<std::vector<uint8_t>, Node *>> appSigInputs;

	for (Node *node : fContentNodes) {
		const InodePlan &plan = fInodes[node->inodePlanIndex];

		if (!node->name.empty()) {
			nameInputs.push_back({std::vector<uint8_t>(
				node->name.begin(), node->name.end()), node});
		}
		sizeInputs.push_back({Int64Key(plan.reportedSize, fOptions.byteOrder), node});
		mtimeInputs.push_back({Int64Key(
			EncodeTime(node->modifiedTime.seconds, node->modifiedTime.nanoseconds),
			fOptions.byteOrder),
			node});

		for (const Attribute &attribute : node->attributes) {
			if (attribute.name == "BEOS:APP_SIG" && !attribute.data.empty()) {
				appSigInputs.push_back({std::vector<uint8_t>(
					attribute.data.begin(), attribute.data.end()), node});
				break;
			}
		}
	}

	std::vector<std::pair<std::string, int>> indexEntries;
	indexEntries.push_back({"name",
		PlanIndex("name", kBPlusTreeStringType, kSStrIndex, fIndexDirPlan, nameInputs)});
	indexEntries.push_back({"size",
		PlanIndex("size", kBPlusTreeInt64Type, kSLongLongIndex, fIndexDirPlan, sizeInputs)});
	indexEntries.push_back({"last_modified",
		PlanIndex("last_modified", kBPlusTreeInt64Type, kSLongLongIndex, fIndexDirPlan,
			mtimeInputs)});
	if (!appSigInputs.empty()) {
		indexEntries.push_back({"BEOS:APP_SIG",
			PlanIndex("BEOS:APP_SIG", kBPlusTreeStringType, kSStrIndex, fIndexDirPlan,
				appSigInputs)});
	}

	std::sort(indexEntries.begin(), indexEntries.end(),
		[](const std::pair<std::string, int> &a, const std::pair<std::string, int> &b) {
			std::vector<uint8_t> ka(a.first.begin(), a.first.end());
			std::vector<uint8_t> kb(b.first.begin(), b.first.end());
			return CompareStringKey(ka, kb) < 0;
		});

	auto tree = std::make_shared<BPlusTreeBuilder>(kBPlusTreeStringType, fOptions.byteOrder);
	std::vector<std::vector<int>> groups;
	for (auto &entry : indexEntries) {
		tree->AddKey(reinterpret_cast<const uint8_t *>(entry.first.data()),
			static_cast<uint16_t>(entry.first.size()), 1);
		groups.push_back({entry.second});
	}
	tree->Finalize();

	InodePlan &plan = fInodes[fIndexDirPlan];
	plan.streamKind = StreamKind::Tree;
	plan.tree = tree;
	plan.treeGroups = std::move(groups);
	plan.streamSize = tree->StreamSize();
	plan.reportedSize = static_cast<int64_t>(plan.streamSize);
}


int64_t BfsBuilder::DataBlocks(const InodePlan &plan) const
{
	if (plan.streamKind == StreamKind::None
		|| plan.streamKind == StreamKind::ShortSymlink) {
		return 0;
	}
	return StreamBlockCount(plan.streamSize, fOptions.blockSize);
}


int64_t BfsBuilder::StreamBlocks(const InodePlan &plan) const
{
	// Data blocks plus the indirect/double-indirect array + alignment blocks
	// reserved for this stream (filled in after the provisional geometry pass).
	return DataBlocks(plan) + plan.metadataBlocks;
}


int64_t BfsBuilder::BlocksToWrite() const
{
	// The superblock is a 512-byte write rather than a whole block; counting it
	// as one keeps the arithmetic simple and is lost in the rounding.
	int64_t blocks = 1 + fGeometry.bitmapBlocks;

	// plan.metadataBlocks is the pre-placement upper bound, so the actual array
	// blocks have to come from the layout Place produced.
	for (const InodePlan &plan : fInodes) {
		blocks += 1 + ArrayBlockCount(plan.layout);
		for (const BlockRun &run : plan.layout.DataRuns()) {
			blocks += run.length;
		}
	}
	return blocks;
}


void BfsBuilder::ComputeGeometry()
{
	uint32_t blockSize = fOptions.blockSize;
	uint32_t blockShift = Log2Exact(blockSize);
	int64_t bitsPerBlock = static_cast<int64_t>(blockSize) * 8;

	int64_t content = static_cast<int64_t>(fInodes.size());
	for (const InodePlan &plan : fInodes) {
		content += StreamBlocks(plan);
	}
	fContentBlocks = content;

	int64_t numBlocks = 0;
	int64_t bitmapBlocks = 0;
	int64_t logBlocks = 0;

	if (fOptions.sizeOverride > 0) {
		numBlocks = static_cast<int64_t>(fOptions.sizeOverride / blockSize);
		bitmapBlocks = (numBlocks + bitsPerBlock - 1) / bitsPerBlock;
		logBlocks = ChooseLogBlocks(static_cast<uint64_t>(numBlocks) * blockSize);
		int64_t reserved = 1 + bitmapBlocks + logBlocks;
		if (numBlocks < reserved + content) {
			throw std::runtime_error("requested image size is too small for the content");
		}
	} else {
		numBlocks = content + 16;
		for (int iteration = 0; iteration < 12; iteration++) {
			bitmapBlocks = (numBlocks + bitsPerBlock - 1) / bitsPerBlock;
			logBlocks = ChooseLogBlocks(static_cast<uint64_t>(numBlocks) * blockSize);
			numBlocks = 1 + bitmapBlocks + logBlocks + content;
		}
		bitmapBlocks = (numBlocks + bitsPerBlock - 1) / bitsPerBlock;
		logBlocks = ChooseLogBlocks(static_cast<uint64_t>(numBlocks) * blockSize);
		while (1 + bitmapBlocks + logBlocks + content > numBlocks) {
			numBlocks++;
			bitmapBlocks = (numBlocks + bitsPerBlock - 1) / bitsPerBlock;
			logBlocks = ChooseLogBlocks(static_cast<uint64_t>(numBlocks) * blockSize);
		}
	}

	if (numBlocks < 10) {
		numBlocks = 10;
		bitmapBlocks = (numBlocks + bitsPerBlock - 1) / bitsPerBlock;
	}

	int reservedTotal = static_cast<int>(1 + bitmapBlocks + logBlocks);

	// Choose the allocation-group geometry. Start from the smallest shift the
	// block size allows (blocks_per_ag == 1) and grow it to keep the group count
	// modest and to keep the reserved region inside the first group.
	uint32_t minAgShift = blockShift + 3;
	uint32_t agShift = minAgShift;
	while (agShift < 16) {
		int64_t groups = (numBlocks + (static_cast<int64_t>(1) << agShift) - 1)
			>> agShift;
		if (groups <= 512) {
			break;
		}
		agShift++;
	}
	while (agShift < 16 && (static_cast<int64_t>(1) << agShift) < reservedTotal) {
		agShift++;
	}
	int64_t groupSize = static_cast<int64_t>(1) << agShift;
	if (groupSize < reservedTotal) {
		throw std::runtime_error("volume too large: reserved region exceeds one group");
	}

	fGeometry.blockSize = blockSize;
	fGeometry.blockShift = blockShift;
	fGeometry.numBlocks = numBlocks;
	fGeometry.agShift = static_cast<int32_t>(agShift);
	fGeometry.blocksPerAg = static_cast<int32_t>(groupSize / bitsPerBlock);
	fGeometry.numAgs = static_cast<int32_t>((numBlocks + groupSize - 1) >> agShift);
	fGeometry.bitmapBlocks = bitmapBlocks;
	fGeometry.logBlocks = fGeometry.ToRun(1 + bitmapBlocks,
		static_cast<uint16_t>(logBlocks));
}


void BfsBuilder::Place(BlockAllocator &allocator)
{
	for (InodePlan &plan : fInodes) {
		plan.block = allocator.Allocate(1);
	}
	for (InodePlan &plan : fInodes) {
		int64_t data = DataBlocks(plan);
		if (plan.streamKind == StreamKind::None
			|| plan.streamKind == StreamKind::ShortSymlink) {
			continue;
		}
		plan.layout = BuildStreamLayout(allocator, fGeometry, data, plan.streamSize,
			fTuning);
	}
}


std::vector<int64_t> BfsBuilder::TreeValues(const InodePlan &plan) const
{
	std::vector<int64_t> values;
	for (const std::vector<int> &group : plan.treeGroups) {
		std::vector<int64_t> blocks;
		blocks.reserve(group.size());
		for (int planIndex : group) {
			blocks.push_back(fInodes[planIndex].block);
		}
		std::sort(blocks.begin(), blocks.end());
		for (int64_t block : blocks) {
			values.push_back(block);
		}
	}
	return values;
}


void BfsBuilder::SerializeInode(ImageFile &image, InodePlan &plan)
{
	uint32_t blockSize = fOptions.blockSize;
	std::vector<uint8_t> buffer(blockSize, 0);
	uint8_t *p = buffer.data();

	WU32(p + inode::kMagic1, kInodeMagic1);
	WRun(p + inode::kInodeNum, fGeometry.ToRun(plan.block, 1));
	WU32(p + inode::kUid, plan.uid);
	WU32(p + inode::kGid, plan.gid);
	WU32(p + inode::kMode, plan.mode);
	WU32(p + inode::kFlags, plan.flags);
	WS64(p + inode::kCreateTime,
		EncodeTime(plan.createTime.seconds, plan.createTime.nanoseconds));
	WS64(p + inode::kLastModifiedTime,
		EncodeTime(plan.modifiedTime.seconds, plan.modifiedTime.nanoseconds));
	WRun(p + inode::kParent, fGeometry.ToRun(ParentBlock(plan), 1));
	if (plan.attrDirPlan >= 0) {
		WRun(p + inode::kAttributes,
			fGeometry.ToRun(fInodes[plan.attrDirPlan].block, 1));
	}
	WU32(p + inode::kType, plan.type);
	WS32(p + inode::kInodeSize, static_cast<int32_t>(blockSize));

	if (plan.streamKind == StreamKind::ShortSymlink) {
		size_t n = std::min(plan.shortSymlink.size(),
			static_cast<size_t>(kShortSymlinkNameLength - 1));
		if (n > 0) {
			::memcpy(p + inode::kData, plan.shortSymlink.data(), n);
		}
	} else {
		WriteDataStream(p + inode::kData, fGeometry, plan.layout, fOptions.byteOrder);
	}

	WS64(p + inode::kStatusChangeTime,
		EncodeTime(plan.changeTime.seconds, plan.changeTime.nanoseconds));

	if (!plan.smallData.empty()) {
		if (inode::kSmallDataStart + plan.smallData.size() > blockSize) {
			throw std::runtime_error("small_data region overflows the inode block");
		}
		::memcpy(p + inode::kSmallDataStart, plan.smallData.data(),
			plan.smallData.size());
	}

	image.WriteBlock(plan.block, buffer.data());
	fProgress.Advance(blockSize);

	// Indirect / double-indirect array blocks (empty for direct-only streams).
	WriteArrayBlocks(image, fGeometry, plan.layout, fOptions.byteOrder, fProgress);

	std::vector<BlockRun> dataRuns = plan.layout.DataRuns();
	switch (plan.streamKind) {
		case StreamKind::File:
			fProgress.Note(plan.filePath);
			WriteFileStream(image, fGeometry, dataRuns, plan.filePath, plan.streamSize,
				fProgress);
			break;
		case StreamKind::Bytes:
			WriteBytesStream(image, fGeometry, dataRuns, plan.bytes.data(),
				plan.streamSize, fProgress);
			break;
		case StreamKind::Tree: {
			std::vector<uint8_t> treeBytes;
			plan.tree->Serialize(treeBytes, TreeValues(plan));
			WriteBytesStream(image, fGeometry, dataRuns, treeBytes.data(),
				treeBytes.size(), fProgress);
			break;
		}
		case StreamKind::None:
		case StreamKind::ShortSymlink:
			break;
	}
}


void BfsBuilder::SerializeSuperBlock(ImageFile &image, int64_t usedBlocks,
	const BlockRun &indicesRun)
{
	std::vector<uint8_t> buffer(512, 0);
	uint8_t *p = buffer.data();

	std::string name = fOptions.volumeName;
	if (name.size() >= static_cast<size_t>(kDiskNameLength)) {
		name.resize(kDiskNameLength - 1);
	}
	::memcpy(p + super::kName, name.data(), name.size());

	WU32(p + super::kMagic1, kSuperBlockMagic1);
	WU32(p + super::kFsByteOrder, kSuperBlockFsLendian);
	WU32(p + super::kBlockSize, fGeometry.blockSize);
	WU32(p + super::kBlockShift, fGeometry.blockShift);
	WS64(p + super::kNumBlocks, fGeometry.numBlocks);
	WS64(p + super::kUsedBlocks, usedBlocks);
	WS32(p + super::kInodeSize, static_cast<int32_t>(fGeometry.blockSize));
	WU32(p + super::kMagic2, kSuperBlockMagic2);
	WS32(p + super::kBlocksPerAg, fGeometry.blocksPerAg);
	WS32(p + super::kAgShift, fGeometry.agShift);
	WS32(p + super::kNumAgs, fGeometry.numAgs);
	WU32(p + super::kFlags, kSuperBlockDiskClean);
	WRun(p + super::kLogBlocks, fGeometry.logBlocks);
	WS64(p + super::kLogStart, fGeometry.ToBlock(fGeometry.logBlocks));
	WS64(p + super::kLogEnd, fGeometry.ToBlock(fGeometry.logBlocks));
	WU32(p + super::kMagic3, kSuperBlockMagic3);
	WRun(p + super::kRootDir, fGeometry.ToRun(fInodes[fRootPlan].block, 1));
	WRun(p + super::kIndices, indicesRun);

	image.WriteAt(kSuperBlockOffset, buffer.data(), buffer.size());
}


void BfsBuilder::Build(Node &root, const std::string &outputPath)
{
	// A device cannot be sized to fit, so the volume fills it unless an explicit
	// size was asked for. ImageFile rejects an explicit size that does not fit.
	if (fOptions.sizeOverride == 0) {
		uint64_t deviceBytes = DeviceSizeOf(outputPath);
		if (deviceBytes != 0) {
			fOptions.sizeOverride = deviceBytes;
		}
	}

	fProgress.BeginCountPhase("planning", "nodes", CountNodes(root));
	fRootPlan = PlanNode(root, -1, true);

	// The indices sort and build a B+tree over every content node, which is not
	// instant on a large tree but has no natural per-item unit to count.
	fProgress.BeginPhase("indexing");
	PlanIndices();

	fProgress.BeginPhase("laying out");

	// Provisional geometry (data only) fixes the block size and group geometry the
	// indirect-tier reservation depends on; then reserve array/alignment blocks and
	// recompute. A smaller provisional group size only over-estimates the reserved
	// metadata, so the final geometry always has room and Place uses the exact
	// amount (leaving any slack as free tail).
	ComputeGeometry();
	fTuning = StreamTuning::FromEnvironment(fGeometry);
	for (InodePlan &plan : fInodes) {
		plan.metadataBlocks = StreamMetadataUpperBound(fGeometry, DataBlocks(plan),
			fTuning);
	}
	ComputeGeometry();

	BlockAllocator allocator(fGeometry, FirstFreeBlock());
	Place(allocator);

	uint64_t imageBytes = static_cast<uint64_t>(fGeometry.numBlocks) * fGeometry.blockSize;
	ImageFile image(outputPath, imageBytes, fGeometry.blockSize);

	if (image.IsDevice()) {
		// A fresh sparse file reads as zeroes everywhere the builder does not
		// write; a device holds whatever the previous volume left. Two regions are
		// never written and must be cleared explicitly:
		//
		//  - block 0 outside the superblock: an old boot sector, and the ext2
		//    superblock at offset 1024 that Haiku scrubs when it formats. Left
		//    behind, they make the volume ambiguous to anything that probes it.
		//  - the log area, which is only reserved and pointed at. Format leaves it
		//    empty (log_start == log_end), so a driver never replays it, but stale
		//    log entries have no business sitting there.
		image.ZeroRange(0, fGeometry.blockSize);
		image.ZeroRange(
			static_cast<uint64_t>(fGeometry.ToBlock(fGeometry.logBlocks))
				* fGeometry.blockSize,
			static_cast<uint64_t>(fGeometry.logBlocks.length) * fGeometry.blockSize);

		if (fOptions.zeroFree) {
			// Free data blocks keep the old volume's contents. That is invisible to
			// BFS -- nothing reads an unallocated block -- but it does leave the
			// previous filesystem's data recoverable on the device.
			image.ZeroRange(0, imageBytes);
		}
	}

	BlockRun indicesRun;
	if (fIndexDirPlan >= 0) {
		indicesRun = fGeometry.ToRun(fInodes[fIndexDirPlan].block, 1);
	}

	// Every block written counts the same, whatever it holds; the byte figures on
	// screen are just that count scaled by the block size.
	fProgress.BeginBytePhase("writing", BlocksToWrite() * fGeometry.blockSize);

	SerializeSuperBlock(image, allocator.UsedBlocks(), indicesRun);
	fProgress.Advance(fGeometry.blockSize);

	allocator.WriteBitmap(image, fOptions.byteOrder);
	fProgress.Advance(fGeometry.bitmapBlocks * fGeometry.blockSize);

	for (InodePlan &plan : fInodes) {
		SerializeInode(image, plan);
	}
	image.Flush();
	fProgress.Finish();
}


} // bfs
