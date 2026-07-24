#include "Checker.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "Endian.h"


namespace bfs {


namespace {


int CompareKey(uint32_t keyType, const uint8_t *a, uint16_t aLen,
	const uint8_t *b, uint16_t bLen)
{
	switch (keyType) {
		case kBPlusTreeInt32Type: {
			int32_t x = GetS32(a), y = GetS32(b);
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		case kBPlusTreeUInt32Type: {
			uint32_t x = GetU32(a), y = GetU32(b);
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		case kBPlusTreeInt64Type: {
			int64_t x = GetS64(a), y = GetS64(b);
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		case kBPlusTreeUInt64Type: {
			uint64_t x = GetU64(a), y = GetU64(b);
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		case kBPlusTreeFloatType: {
			uint32_t xb = GetU32(a), yb = GetU32(b);
			float x, y;
			::memcpy(&x, &xb, 4);
			::memcpy(&y, &yb, 4);
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		case kBPlusTreeDoubleType: {
			uint64_t xb = GetU64(a), yb = GetU64(b);
			double x, y;
			::memcpy(&x, &xb, 8);
			::memcpy(&y, &yb, 8);
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		default: {   // string / unknown: unsigned bytewise, shorter first
			size_t n = std::min(aLen, bLen);
			int c = n == 0 ? 0 : ::memcmp(a, b, n);
			if (c != 0) {
				return c < 0 ? -1 : 1;
			}
			return aLen < bLen ? -1 : (aLen > bLen ? 1 : 0);
		}
	}
}


struct KeyBound {
	const uint8_t *data = nullptr;
	uint16_t length = 0;
	bool present = false;
};


// Recursively validates one B+tree's structure, key ordering, separator bounds,
// and (for directory trees) collects leaf entries in key order.
class TreeValidator {
public:
	TreeValidator(Findings &findings, const std::vector<uint8_t> &tree,
		std::string path, uint32_t keyType, bool isDirectory, int64_t numBlocks):
		fFindings(findings),
		fTree(tree),
		fPath(std::move(path)),
		fKeyType(keyType),
		fIsDirectory(isDirectory),
		fNumBlocks(numBlocks)
	{
	}

	void Validate(std::vector<DirEntry> *entries)
	{
		fEntries = entries;
		if (fTree.size() < static_cast<size_t>(btreehdr::kSize)) {
			Error("tree stream smaller than a header");
			return;
		}
		const uint8_t *base = fTree.data();
		if (GetU32(base + btreehdr::kMagic) != kBPlusTreeMagic) {
			Error("bad B+tree header magic");
			return;
		}
		fNodeSize = GetU32(base + btreehdr::kNodeSize);
		fMaxSize = GetS64(base + btreehdr::kMaximumSize);
		uint32_t levels = GetU32(base + btreehdr::kMaxNumberOfLevels);
		int64_t rootPointer = GetS64(base + btreehdr::kRootNodePointer);

		if (fNodeSize < static_cast<int64_t>(btreenode::kSize)
			|| fNodeSize > static_cast<int64_t>(fTree.size())) {
			Error("bad node_size");
			return;
		}
		if (fMaxSize != static_cast<int64_t>(fTree.size())) {
			Error("maximum_size does not match the stream size");
		}
		if (levels < 1) {
			Error("max_number_of_levels is zero");
		}
		if (!ValidLink(rootPointer)) {
			Error("root_node_pointer out of range");
			return;
		}

		// Per-level "previous node offset" state for sibling-link checking.
		// Indexed by node depth (root == 0); sized past the depth guard below.
		fPrevAtLevel.assign(128, kBPlusTreeNull);

		KeyBound none;
		ValidateNode(rootPointer, none, none, 0);
	}

private:
	void Error(const std::string &message, int64_t block = -1)
	{
		fFindings.Error("btree", message, block, fPath);
	}

	bool ValidLink(int64_t link) const
	{
		return link >= fNodeSize && link + fNodeSize <= fMaxSize
			&& (link % fNodeSize) == 0;
	}

	void ValidateNode(int64_t offset, KeyBound lower, KeyBound upper, int depth)
	{
		if (depth > 64) {
			Error("tree deeper than 64 levels (possible cycle)");
			return;
		}
		if (!ValidLink(offset)) {
			Error("node link out of range");
			return;
		}
		if (!fVisited.insert(offset).second) {
			Error("node reached twice (cycle in tree)");
			return;
		}

		const uint8_t *node = fTree.data() + offset;
		int64_t leftLink = GetS64(node + btreenode::kLeftLink);
		int64_t rightLink = GetS64(node + btreenode::kRightLink);
		int64_t overflow = GetS64(node + btreenode::kOverflowLink);
		uint16_t keyCount = GetU16(node + btreenode::kAllKeyCount);
		uint16_t keyLength = GetU16(node + btreenode::kAllKeyLength);

		int64_t used = KeyAlign(btreenode::kSize + keyLength)
			+ static_cast<int64_t>(keyCount) * (2 + 8);
		if (used > fNodeSize) {
			Error("node Used() exceeds node_size");
			return;
		}

		if (leftLink != kBPlusTreeNull && !ValidLink(leftLink)) {
			Error("bad left_link");
		}
		if (rightLink != kBPlusTreeNull && !ValidLink(rightLink)) {
			Error("bad right_link");
		}

		const uint8_t *keys = node + btreenode::kSize;
		const uint8_t *lengthTable = node + KeyAlign(btreenode::kSize + keyLength);
		const uint8_t *values = lengthTable + keyCount * sizeof(uint16_t);

		std::vector<std::pair<const uint8_t *, uint16_t>> keyList;
		uint16_t previous = 0;
		for (uint16_t k = 0; k < keyCount; k++) {
			uint16_t cumulative = GetU16(lengthTable + k * sizeof(uint16_t));
			if (cumulative < previous) {
				Error("key-length table not monotonic");
				return;
			}
			uint16_t length = static_cast<uint16_t>(cumulative - previous);
			if (length == 0 || length > kBPlusTreeMaxKeyLength) {
				Error("key length out of range");
				return;
			}
			keyList.push_back({keys + previous, length});
			previous = cumulative;
		}
		if (previous != keyLength) {
			Error("key-length table end does not match all_key_length");
		}

		for (uint16_t k = 0; k < keyCount; k++) {
			const uint8_t *kp = keyList[k].first;
			uint16_t kl = keyList[k].second;
			if (k > 0 && CompareKey(fKeyType, keyList[k - 1].first,
					keyList[k - 1].second, kp, kl) >= 0) {
				Error("keys not strictly increasing within node");
			}
			if (lower.present && CompareKey(fKeyType, kp, kl, lower.data, lower.length) <= 0) {
				Error("key is not above the lower separator bound");
			}
			if (upper.present && CompareKey(fKeyType, kp, kl, upper.data, upper.length) > 0) {
				Error("key exceeds the upper separator bound");
			}
		}

		bool leaf = overflow == kBPlusTreeNull;
		if (leaf) {
			for (uint16_t k = 0; k < keyCount; k++) {
				int64_t value = GetS64(values + k * sizeof(int64_t));
				if (fIsDirectory) {
					if (value == kBPlusTreeNull) {
						Error("directory leaf value is -1");
					} else if (value < 0 || value >= fNumBlocks) {
						Error("directory leaf value (inode) out of range", value);
					} else if (fEntries != nullptr) {
						DirEntry entry;
						entry.name.assign(
							reinterpret_cast<const char *>(keyList[k].first),
							keyList[k].second);
						entry.inode = value;
						fEntries->push_back(std::move(entry));
					}
				} else {
					ValidateIndexValue(value);
				}
			}
		} else {
			if (!ValidLink(overflow)) {
				Error("bad overflow_link");
				return;
			}
			// Children live one level down; verify their sibling links as we
			// walk them in key order (see CheckSiblingLinks).
			int childDepth = depth + 1;
			KeyBound previousBound = lower;
			for (uint16_t k = 0; k < keyCount; k++) {
				int64_t child = GetS64(values + k * sizeof(int64_t));
				KeyBound separator;
				separator.data = keyList[k].first;
				separator.length = keyList[k].second;
				separator.present = true;
				if (ValidLink(child)) {
					int64_t next = (k + 1 < keyCount)
						? GetS64(values + (k + 1) * sizeof(int64_t))
						: overflow;
					CheckSiblingLinks(childDepth, child, next, k == 0);
					ValidateNode(child, previousBound, separator, depth + 1);
				} else {
					Error("internal child link out of range");
				}
				previousBound = separator;
			}
			if (ValidLink(overflow)) {
				// The rightmost child's right_link is checked from the next
				// parent's first child, so pass next == 0 (no right check here).
				CheckSiblingLinks(childDepth, overflow, 0, false);
			}
			ValidateNode(overflow, previousBound, upper, depth + 1);
		}
	}

	// Every level of the tree is a doubly-linked list. Verify that 'child' (a node
	// at 'childDepth') links to its in-order neighbours: its left_link must equal
	// the previous node seen at this level, and its right_link must equal 'next'
	// (its next sibling under the same parent; 0 means "not checked here"). When
	// 'child' is the first child of a parent, also confirm the previous node's
	// right_link reaches across the parent boundary to this node.
	void CheckSiblingLinks(int childDepth, int64_t child, int64_t next,
		bool isFirstChild)
	{
		int64_t prev = fPrevAtLevel[static_cast<size_t>(childDepth)];
		const uint8_t *node = fTree.data() + child;
		int64_t left = GetS64(node + btreenode::kLeftLink);
		int64_t right = GetS64(node + btreenode::kRightLink);

		if (isFirstChild && prev != kBPlusTreeNull) {
			int64_t prevRight = GetS64(fTree.data() + prev + btreenode::kRightLink);
			if (prevRight != child) {
				Error("broken level chain: node's right_link does not reach the "
					"next node at its level", prev);
			}
		}
		if (left != prev) {
			Error("broken level chain: node's left_link does not point to the "
				"previous node at its level", child);
		}
		if (next != 0 && right != next) {
			Error("broken level chain: node's right_link does not point to the "
				"next node at its level", child);
		}

		fPrevAtLevel[static_cast<size_t>(childDepth)] = child;
	}

	void ValidateIndexValue(int64_t link)
	{
		uint64_t type = static_cast<uint64_t>(link) >> 62;
		if (type == 0) {
			if (link < 0 || link >= fNumBlocks) {
				fFindings.Warning("btree", "index leaf value out of range", link, fPath);
			}
			return;
		}
		int64_t offset = link & 0x3ffffffffffffc00LL;
		if (!ValidLink(offset)) {
			Error("duplicate link offset out of range");
			return;
		}
		if (type == static_cast<uint64_t>(kBPlusTreeDuplicateNode)) {
			int64_t count = GetS64(fTree.data() + offset + 2 * sizeof(int64_t));
			if (count < 0 || count > kNumDuplicateValues) {
				Error("duplicate-node count out of range");
			}
			int64_t link2 = GetS64(fTree.data() + offset + 1 * sizeof(int64_t));
			int guard = 0;
			while (link2 != kBPlusTreeNull) {
				if (!ValidLink(link2) || ++guard > 100000) {
					Error("bad duplicate-node chain");
					break;
				}
				link2 = GetS64(fTree.data() + link2 + 1 * sizeof(int64_t));
			}
		}
	}

	Findings &fFindings;
	const std::vector<uint8_t> &fTree;
	std::string fPath;
	uint32_t fKeyType;
	bool fIsDirectory;
	int64_t fNumBlocks;
	int64_t fNodeSize = 0;
	int64_t fMaxSize = 0;
	std::set<int64_t> fVisited;
	std::vector<int64_t> fPrevAtLevel;   // per-depth last node offset (sibling chain)
	std::vector<DirEntry> *fEntries = nullptr;
};


} // unnamed namespace


Checker::Checker(BfsReader &reader, Findings &findings, const CheckOptions &options):
	fReader(reader),
	fFindings(findings),
	fOptions(options),
	fGeo(reader.GetGeometry()),
	fReferenced(reader.GetGeometry().numBlocks)
{
}


uint32_t Checker::KeyTypeFromMode(uint32_t mode) const
{
	if (mode & kSStrIndex) {
		return kBPlusTreeStringType;
	}
	if (mode & kSIntIndex) {
		return kBPlusTreeInt32Type;
	}
	if (mode & kSUIntIndex) {
		return kBPlusTreeUInt32Type;
	}
	if (mode & kSLongLongIndex) {
		return kBPlusTreeInt64Type;
	}
	if (mode & kSULongLongIndex) {
		return kBPlusTreeUInt64Type;
	}
	if (mode & kSFloatIndex) {
		return kBPlusTreeFloatType;
	}
	if (mode & kSDoubleIndex) {
		return kBPlusTreeDoubleType;
	}
	return kBPlusTreeStringType;
}


void Checker::SetPhase(const char *phase)
{
	fPhase = phase;
	fprintf(stderr, "\r%-22s %lld nodes checked", fPhase,
		static_cast<long long>(fNodesChecked));
	fflush(stderr);
}


void Checker::Tick()
{
	fNodesChecked++;
	if ((fNodesChecked & 127) == 0) {
		fprintf(stderr, "\r%-22s %lld nodes checked", fPhase,
			static_cast<long long>(fNodesChecked));
		fflush(stderr);
	}
}


std::vector<uint8_t> Checker::ReadRunBytes(const BlockRun &run)
{
	std::vector<uint8_t> buffer(static_cast<size_t>(run.length) * fGeo.blockSize);
	int64_t base = fGeo.ToBlock(run);
	for (uint16_t i = 0; i < run.length; i++) {
		fReader.ReadBlock(base + i, buffer.data()
			+ static_cast<size_t>(i) * fGeo.blockSize);
	}
	return buffer;
}


bool Checker::ValidRun(const BlockRun &run, const std::string &category,
	const std::string &path, int64_t owner)
{
	if (run.length == 0) {
		fFindings.Error(category, "zero-length block_run", owner, path);
		return false;
	}
	if (run.group < 0 || run.group >= fGeo.numAgs) {
		fFindings.Error(category, "block_run group out of range", owner, path);
		return false;
	}
	int64_t groupSize = static_cast<int64_t>(1) << fGeo.agShift;
	if (static_cast<int64_t>(run.start) >= groupSize) {
		fFindings.Error(category, "block_run start beyond group", owner, path);
		return false;
	}
	if (static_cast<int64_t>(run.start) + run.length > groupSize) {
		fFindings.Error(category, "block_run crosses an allocation-group boundary",
			owner, path);
		return false;
	}
	int64_t block = fGeo.ToBlock(run);
	if (block < 0 || block + run.length > fGeo.numBlocks) {
		fFindings.Error(category, "block_run extends past the end of the volume",
			owner, path);
		return false;
	}
	return true;
}


void Checker::Reference(int64_t block, const std::string &path, int64_t owner)
{
	if (!InRange(block)) {
		fFindings.Error("reference", "referenced block out of range", block, path);
		return;
	}
	if (fReferenced.SetAndTest(block)) {
		fFindings.Error("crosslink", "block referenced by more than one object",
			block, path);
	}
	(void)owner;
}


void Checker::LoadBitmap()
{
	fBitmap.resize(static_cast<size_t>(fGeo.bitmapBlocks) * fGeo.blockSize);
	for (int64_t i = 0; i < fGeo.bitmapBlocks; i++) {
		try {
			fReader.ReadBlock(1 + i, fBitmap.data()
				+ static_cast<size_t>(i) * fGeo.blockSize);
		} catch (const std::exception &error) {
			fFindings.Error("bitmap",
				std::string("cannot read bitmap block: ") + error.what(), 1 + i);
		}
	}
}


bool Checker::BitmapAllocated(int64_t block) const
{
	size_t index = static_cast<size_t>(block >> 3);
	if (index >= fBitmap.size()) {
		return true;   // out of bitmap range: treat as used
	}
	return (fBitmap[index] >> (block & 7)) & 1;
}


void Checker::MarkReserved()
{
	int64_t reserved = std::min(fReserved, fGeo.numBlocks);
	for (int64_t block = 0; block < reserved; block++) {
		fReferenced.Set(block);
	}
}


void Checker::CheckSuperblock()
{
	if (fGeo.numBlocks < 10) {
		fFindings.Error("superblock", "num_blocks < 10");
	}
	int64_t groupSize = static_cast<int64_t>(1) << fGeo.agShift;
	int64_t expectedAgs = (fGeo.numBlocks + groupSize - 1) >> fGeo.agShift;
	if (fGeo.numAgs != expectedAgs) {
		fFindings.Error("superblock",
			"num_ags does not equal ceil(num_blocks / (1 << ag_shift))");
	}
	if (static_cast<int64_t>(fGeo.blocksPerAg) * fGeo.blockSize * 8 != groupSize) {
		fFindings.Error("superblock",
			"blocks_per_ag * block_size * 8 != (1 << ag_shift)");
	}
	if (fReader.UsedBlocks() < 0 || fReader.UsedBlocks() > fGeo.numBlocks) {
		fFindings.Error("superblock", "used_blocks is out of range");
	}
	if (!ValidRun(fGeo.logBlocks, "superblock", "log_blocks", -1)) {
		fFindings.Error("superblock", "log_blocks is not a valid run");
	}
}


void Checker::CheckDataStream(const Inode &inode, const std::string &path)
{
	const DataStreamInfo &s = inode.stream;

	if (s.maxIndirectRange != 0 && s.maxIndirectRange < s.maxDirectRange) {
		fFindings.Error("stream", "max_indirect_range < max_direct_range",
			inode.block, path);
	}
	if (s.maxDoubleIndirectRange != 0
		&& s.maxDoubleIndirectRange < s.maxIndirectRange) {
		fFindings.Error("stream", "max_double_indirect_range < max_indirect_range",
			inode.block, path);
	}

	int64_t covered = 0;
	int shift = fGeo.blockShift;

	for (int i = 0; i < kNumDirectBlocks; i++) {
		if (s.direct[i].IsZero()) {
			break;
		}
		if (ValidRun(s.direct[i], "stream", path, inode.block)) {
			int64_t base = fGeo.ToBlock(s.direct[i]);
			for (uint16_t b = 0; b < s.direct[i].length; b++) {
				Reference(base + b, path, inode.block);
			}
			covered += static_cast<int64_t>(s.direct[i].length) << shift;
		}
	}

	if (!s.indirect.IsZero() && ValidRun(s.indirect, "stream", path, inode.block)) {
		int64_t base = fGeo.ToBlock(s.indirect);
		for (uint16_t b = 0; b < s.indirect.length; b++) {
			Reference(base + b, path, inode.block);
		}
		try {
			std::vector<uint8_t> array = ReadRunBytes(s.indirect);
			int64_t entries = static_cast<int64_t>(array.size()) / 8;
			for (int64_t j = 0; j < entries; j++) {
				BlockRun run = GetBlockRun(array.data() + j * 8);
				if (run.IsZero()) {
					break;
				}
				if (ValidRun(run, "stream", path, inode.block)) {
					int64_t rbase = fGeo.ToBlock(run);
					for (uint16_t b = 0; b < run.length; b++) {
						Reference(rbase + b, path, inode.block);
					}
					covered += static_cast<int64_t>(run.length) << shift;
				}
			}
		} catch (const std::exception &error) {
			fFindings.Error("stream",
				std::string("cannot read indirect array: ") + error.what(),
				inode.block, path);
		}
	}

	if (!s.doubleIndirect.IsZero()
		&& ValidRun(s.doubleIndirect, "stream", path, inode.block)) {
		int64_t base = fGeo.ToBlock(s.doubleIndirect);
		for (uint16_t b = 0; b < s.doubleIndirect.length; b++) {
			Reference(base + b, path, inode.block);
		}
		try {
			std::vector<uint8_t> top = ReadRunBytes(s.doubleIndirect);
			int64_t topEntries = static_cast<int64_t>(top.size()) / 8;
			for (int64_t t = 0; t < topEntries; t++) {
				BlockRun array = GetBlockRun(top.data() + t * 8);
				if (array.IsZero()) {
					break;
				}
				if (!ValidRun(array, "stream", path, inode.block)) {
					continue;
				}
				int64_t abase = fGeo.ToBlock(array);
				for (uint16_t b = 0; b < array.length; b++) {
					Reference(abase + b, path, inode.block);
				}
				std::vector<uint8_t> level = ReadRunBytes(array);
				int64_t entries = static_cast<int64_t>(level.size()) / 8;
				for (int64_t j = 0; j < entries; j++) {
					BlockRun run = GetBlockRun(level.data() + j * 8);
					if (run.IsZero()) {
						break;
					}
					if (ValidRun(run, "stream", path, inode.block)) {
						int64_t rbase = fGeo.ToBlock(run);
						for (uint16_t b = 0; b < run.length; b++) {
							Reference(rbase + b, path, inode.block);
						}
						covered += static_cast<int64_t>(run.length) << shift;
					}
				}
			}
		} catch (const std::exception &error) {
			fFindings.Error("stream",
				std::string("cannot read double-indirect arrays: ") + error.what(),
				inode.block, path);
		}
	}

	if (covered < s.size) {
		fFindings.Error("stream", "allocated runs do not cover the stream size",
			inode.block, path);
	}
}


void Checker::CheckBTree(const std::vector<uint8_t> &tree, uint32_t keyType,
	bool isDirectory, const std::string &path, std::vector<DirEntry> *entriesOut)
{
	TreeValidator validator(fFindings, tree, path, keyType, isDirectory,
		fGeo.numBlocks);
	validator.Validate(entriesOut);
}


void Checker::CheckInode(int64_t block, const std::string &path, int64_t expectedParent)
{
	if (!InRange(block)) {
		fFindings.Error("inode", "inode block out of range", block, path);
		return;
	}
	if (block < fReserved) {
		fFindings.Error("inode", "inode inside the reserved region", block, path);
		return;
	}
	if (!fVisitedInodes.insert(block).second) {
		fFindings.Error("hierarchy", "inode reachable more than once", block, path);
		return;
	}
	Reference(block, path, block);

	Inode inode;
	try {
		inode = fReader.ReadInode(block);
	} catch (const std::exception &error) {
		fFindings.Error("inode", error.what(), block, path);
		return;
	}
	Tick();

	BlockRun self = GetBlockRun(inode.raw.data() + inode::kInodeNum);
	if (fGeo.ToBlock(self) != block || self.length != 1) {
		fFindings.Error("inode", "inode_num does not match the inode's own block",
			block, path);
	}
	if (fGeo.ToBlock(inode.parent) != expectedParent) {
		fFindings.Error("hierarchy", "parent pointer does not match the containing "
			"directory", block, path);
	}

	bool shortSymlink = inode.IsSymlink() && !inode.IsLongSymlink();
	if (!shortSymlink) {
		CheckDataStream(inode, path);
	}

	if (!inode.attributes.IsZero()) {
		if (ValidRun(inode.attributes, "inode", path, block)) {
			CheckAttributeDir(fGeo.ToBlock(inode.attributes), path);
		}
	}

	if (inode.IsDirectory()) {
		CheckDirectory(inode, path);
	} else if (inode.IsSymlink()) {
		if (inode.IsLongSymlink() && inode.stream.size == 0) {
			fFindings.Warning("symlink", "long symlink has zero-length target",
				block, path);
		}
	} else if (inode.IsRegularFile()) {
		// nothing beyond the data stream
	} else {
		fFindings.Warning("mode",
			"inode in the directory hierarchy is not a file, dir, or symlink",
			block, path);
	}
}


void Checker::CheckDirectory(const Inode &inode, const std::string &path)
{
	if (!(inode.mode & kSStrIndex)) {
		fFindings.Warning("mode", "directory is missing S_STR_INDEX", inode.block, path);
	}

	std::vector<uint8_t> tree;
	try {
		tree = fReader.ReadStreamFully(inode.stream);
	} catch (const std::exception &error) {
		fFindings.Error("btree",
			std::string("cannot read directory tree: ") + error.what(),
			inode.block, path);
		return;
	}

	std::vector<DirEntry> entries;
	CheckBTree(tree, kBPlusTreeStringType, true, path, &entries);

	bool hasDot = false;
	bool hasDotDot = false;
	int64_t parentBlock = fGeo.ToBlock(inode.parent);
	for (const DirEntry &entry : entries) {
		if (entry.name == ".") {
			hasDot = true;
			if (entry.inode != inode.block) {
				fFindings.Error("dir", "'.' does not point to the directory itself",
					inode.block, path);
			}
			continue;
		}
		if (entry.name == "..") {
			hasDotDot = true;
			if (entry.inode != parentBlock) {
				fFindings.Error("dir", "'..' does not match the parent pointer",
					inode.block, path);
			}
			continue;
		}
		if (entry.name.find('/') != std::string::npos) {
			fFindings.Error("dir", "entry name contains '/'", inode.block, path);
			continue;
		}
		std::string childPath = path == "/" ? "/" + entry.name : path + "/" + entry.name;
		CheckInode(entry.inode, childPath, inode.block);
	}
	if (!hasDot) {
		fFindings.Error("dir", "directory is missing its '.' entry", inode.block, path);
	}
	if (!hasDotDot) {
		fFindings.Error("dir", "directory is missing its '..' entry", inode.block, path);
	}
}


void Checker::CheckAttributeDir(int64_t block, const std::string &path)
{
	std::string attrPath = path + " (attributes)";
	if (!InRange(block)) {
		fFindings.Error("attr", "attribute directory out of range", block, attrPath);
		return;
	}
	Reference(block, attrPath, block);

	Inode dir;
	try {
		dir = fReader.ReadInode(block);
	} catch (const std::exception &error) {
		fFindings.Error("attr", error.what(), block, attrPath);
		return;
	}
	Tick();
	if (!(dir.mode & kSAttrDir)) {
		fFindings.Warning("mode", "attribute directory missing S_ATTR_DIR", block, attrPath);
	}
	CheckDataStream(dir, attrPath);

	std::vector<uint8_t> tree;
	try {
		tree = fReader.ReadStreamFully(dir.stream);
	} catch (const std::exception &error) {
		fFindings.Error("attr",
			std::string("cannot read attribute directory tree: ") + error.what(),
			block, attrPath);
		return;
	}

	std::vector<DirEntry> entries;
	CheckBTree(tree, kBPlusTreeStringType, true, attrPath, &entries);
	for (const DirEntry &entry : entries) {
		if (entry.name == "." || entry.name == "..") {
			fFindings.Warning("dir", "attribute directory contains '.'/'..'", block, attrPath);
			continue;
		}
		if (!InRange(entry.inode)) {
			fFindings.Error("attr", "attribute inode out of range", entry.inode, attrPath);
			continue;
		}
		Reference(entry.inode, attrPath + "/" + entry.name, entry.inode);
		Inode attrInode;
		try {
			attrInode = fReader.ReadInode(entry.inode);
		} catch (const std::exception &error) {
			fFindings.Error("attr", error.what(), entry.inode, attrPath);
			continue;
		}
		Tick();
		if (!(attrInode.mode & kSAttr)) {
			fFindings.Warning("mode", "attribute inode missing S_ATTR", entry.inode, attrPath);
		}
		CheckDataStream(attrInode, attrPath + "/" + entry.name);
	}
}


void Checker::CheckIndexDirectory(int64_t block)
{
	if (!InRange(block)) {
		fFindings.Error("index", "index directory out of range", block, "indices");
		return;
	}
	Reference(block, "indices", block);

	Inode dir;
	try {
		dir = fReader.ReadInode(block);
	} catch (const std::exception &error) {
		fFindings.Error("index", error.what(), block, "indices");
		return;
	}
	Tick();
	if (!(dir.mode & kSIndexDir)) {
		fFindings.Warning("mode", "index directory missing S_INDEX_DIR", block, "indices");
	}
	CheckDataStream(dir, "indices");

	std::vector<uint8_t> tree;
	try {
		tree = fReader.ReadStreamFully(dir.stream);
	} catch (const std::exception &error) {
		fFindings.Error("index",
			std::string("cannot read index directory tree: ") + error.what(),
			block, "indices");
		return;
	}

	std::vector<DirEntry> entries;
	CheckBTree(tree, kBPlusTreeStringType, true, "indices", &entries);
	for (const DirEntry &entry : entries) {
		if (entry.name == "." || entry.name == "..") {
			continue;
		}
		CheckIndex(entry.inode, entry.name);
	}
}


void Checker::CheckIndex(int64_t block, const std::string &name)
{
	std::string path = "indices/" + name;
	if (!InRange(block)) {
		fFindings.Error("index", "index inode out of range", block, path);
		return;
	}
	Reference(block, path, block);

	Inode index;
	try {
		index = fReader.ReadInode(block);
	} catch (const std::exception &error) {
		fFindings.Error("index", error.what(), block, path);
		return;
	}
	Tick();
	if (!(index.mode & kSIndexDir)) {
		fFindings.Warning("mode", "index missing S_INDEX_DIR", block, path);
	}
	CheckDataStream(index, path);

	std::vector<uint8_t> tree;
	try {
		tree = fReader.ReadStreamFully(index.stream);
	} catch (const std::exception &error) {
		fFindings.Error("index",
			std::string("cannot read index tree: ") + error.what(), block, path);
		return;
	}
	// Structure and ordering only; index-vs-data content coherence is out of scope.
	CheckBTree(tree, KeyTypeFromMode(index.mode), false, path, nullptr);
}


void Checker::OrphanScan()
{
	std::vector<uint8_t> buffer(fGeo.blockSize);
	for (int64_t block = fReserved; block < fGeo.numBlocks; block++) {
		if (!BitmapAllocated(block) || fReferenced.Test(block)) {
			continue;
		}
		try {
			fReader.ReadBlock(block, buffer.data());
		} catch (const std::exception &) {
			continue;
		}
		if (GetU32(buffer.data() + inode::kMagic1) != kInodeMagic1) {
			continue;
		}
		if ((GetU32(buffer.data() + inode::kFlags) & kInodeInUse) == 0) {
			continue;
		}
		BlockRun self = GetBlockRun(buffer.data() + inode::kInodeNum);
		if (fGeo.ToBlock(self) != block) {
			continue;
		}
		if (GetS32(buffer.data() + inode::kInodeSize)
			!= static_cast<int32_t>(fGeo.blockSize)) {
			continue;
		}
		fOrphanBlocks.insert(block);
		fFindings.Error("orphan", "in-use inode not reachable from the root", block);
	}
}


void Checker::CrossCheckBitmap()
{
	int64_t allocated = 0;
	for (int64_t block = 0; block < fGeo.numBlocks; block++) {
		bool isAllocated = BitmapAllocated(block);
		bool isReferenced = fReferenced.Test(block);
		if (isAllocated) {
			allocated++;
		}
		if (isReferenced && !isAllocated) {
			fFindings.Error("bitmap",
				"block is referenced but not marked allocated", block);
		}
		if (isAllocated && !isReferenced && fOrphanBlocks.count(block) == 0) {
			fFindings.Error("bitmap",
				"block is allocated but not referenced (leak or orphan)", block);
		}
	}
	if (allocated != fReader.UsedBlocks()) {
		char message[128];
		snprintf(message, sizeof(message),
			"used_blocks is %lld but %lld blocks are marked allocated",
			static_cast<long long>(fReader.UsedBlocks()),
			static_cast<long long>(allocated));
		fFindings.Error("bitmap", message);
	}
}


bool Checker::Run()
{
	fReserved = fReader.ReservedBlocks();

	CheckSuperblock();

	if (!fReader.IsClean()) {
		if (fOptions.replayLog) {
			try {
				fReader.ReplayLog();
			} catch (const std::exception &error) {
				fFindings.Error("journal",
					std::string("log replay failed: ") + error.what());
			}
		} else {
			fFindings.Warning("journal",
				"volume not cleanly unmounted; checking committed on-disk state "
				"(use --replay-log to check the post-replay state)");
		}
	}

	LoadBitmap();
	MarkReserved();

	SetPhase("walking tree");
	if (ValidRun(fReader.RootDirectory(), "superblock", "root_dir", -1)) {
		int64_t rootBlock = fGeo.ToBlock(fReader.RootDirectory());
		CheckInode(rootBlock, "/", rootBlock);
	} else {
		fFindings.Error("superblock", "root_dir is not a valid block_run");
	}

	if (!fReader.IndexDirectory().IsZero()) {
		if (ValidRun(fReader.IndexDirectory(), "superblock", "indices", -1)) {
			CheckIndexDirectory(fGeo.ToBlock(fReader.IndexDirectory()));
		} else {
			fFindings.Error("superblock", "indices is not a valid block_run");
		}
	}

	if (fOptions.scanOrphans) {
		SetPhase("orphan scan");
		OrphanScan();
	}

	SetPhase("bitmap cross-check");
	CrossCheckBitmap();

	fprintf(stderr, "\n");
	fFindings.PrintSummary();

	if (fOptions.strict) {
		return fFindings.Errors() == 0 && fFindings.Warnings() == 0;
	}
	return fFindings.Errors() == 0;
}


} // bfs
