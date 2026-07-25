#include "BPlusTreeReader.h"

#include <string.h>

#include <algorithm>
#include <set>


namespace bfs {


namespace {


// Depth limit for a root-to-leaf descent. A tree that claims more levels than
// this is corrupt, and the bound keeps a damaged tree from looping forever.
constexpr int kMaxDepth = 64;

// Guard for the duplicate-node chain, which is a linked list on disk and so can
// be made circular by corruption.
constexpr int kMaxDuplicateChain = 100000;


} // unnamed namespace


const char *TreeStatusName(TreeStatus status)
{
	switch (status) {
		case TreeStatus::Ok:
			return "ok";
		case TreeStatus::StreamTooSmall:
			return "stream_too_small";
		case TreeStatus::BadMagic:
			return "bad_magic";
		case TreeStatus::BadNodeSize:
			return "bad_node_size";
		case TreeStatus::LinkOutOfRange:
			return "link_out_of_range";
		case TreeStatus::UsedExceedsNodeSize:
			return "used_exceeds_node_size";
		case TreeStatus::KeyTableNotMonotonic:
			return "key_table_not_monotonic";
		case TreeStatus::KeyLengthOutOfRange:
			return "key_length_out_of_range";
		case TreeStatus::KeyTableEndMismatch:
			return "key_table_end_mismatch";
		default:
			return "unknown";
	}
}


const char *KeyTypeName(uint32_t dataType)
{
	switch (dataType) {
		case kBPlusTreeStringType:
			return "string";
		case kBPlusTreeInt32Type:
			return "int32";
		case kBPlusTreeUInt32Type:
			return "uint32";
		case kBPlusTreeInt64Type:
			return "int64";
		case kBPlusTreeUInt64Type:
			return "uint64";
		case kBPlusTreeFloatType:
			return "float";
		case kBPlusTreeDoubleType:
			return "double";
		default:
			return "unknown";
	}
}


int CompareKeys(uint32_t keyType, const uint8_t *a, uint16_t aLength,
	const uint8_t *b, uint16_t bLength, ByteOrder order)
{
	switch (keyType) {
		case kBPlusTreeInt32Type: {
			int32_t x = GetS32(a, order), y = GetS32(b, order);
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		case kBPlusTreeUInt32Type: {
			uint32_t x = GetU32(a, order), y = GetU32(b, order);
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		case kBPlusTreeInt64Type: {
			int64_t x = GetS64(a, order), y = GetS64(b, order);
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		case kBPlusTreeUInt64Type: {
			uint64_t x = GetU64(a, order), y = GetU64(b, order);
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		case kBPlusTreeFloatType: {
			uint32_t xb = GetU32(a, order), yb = GetU32(b, order);
			float x, y;
			::memcpy(&x, &xb, 4);
			::memcpy(&y, &yb, 4);
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		case kBPlusTreeDoubleType: {
			uint64_t xb = GetU64(a, order), yb = GetU64(b, order);
			double x, y;
			::memcpy(&x, &xb, 8);
			::memcpy(&y, &yb, 8);
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		default: {   // string / unknown: unsigned bytewise, shorter first
			size_t n = std::min(aLength, bLength);
			int c = n == 0 ? 0 : ::memcmp(a, b, n);
			if (c != 0) {
				return c < 0 ? -1 : 1;
			}
			return aLength < bLength ? -1 : (aLength > bLength ? 1 : 0);
		}
	}
}


BPlusTreeReader::BPlusTreeReader(const std::vector<uint8_t> &tree, ByteOrder order):
	fTree(tree),
	fOrder(order)
{
}


TreeStatus BPlusTreeReader::Open()
{
	if (fTree.size() < static_cast<size_t>(btreehdr::kSize)) {
		return TreeStatus::StreamTooSmall;
	}

	const uint8_t *base = fTree.data();
	fHeader.magic = U32(base + btreehdr::kMagic);
	if (fHeader.magic != kBPlusTreeMagic) {
		return TreeStatus::BadMagic;
	}

	fHeader.nodeSize = U32(base + btreehdr::kNodeSize);
	fHeader.maxNumberOfLevels = U32(base + btreehdr::kMaxNumberOfLevels);
	fHeader.dataType = U32(base + btreehdr::kDataType);
	fHeader.rootNodePointer = S64(base + btreehdr::kRootNodePointer);
	fHeader.freeNodePointer = S64(base + btreehdr::kFreeNodePointer);
	fHeader.maximumSize = S64(base + btreehdr::kMaximumSize);

	// A node has to be at least large enough for its own fixed header, or none
	// of the field offsets below it mean anything.
	if (fHeader.nodeSize < static_cast<int64_t>(btreenode::kSize)
		|| fHeader.nodeSize > static_cast<int64_t>(fTree.size())) {
		return TreeStatus::BadNodeSize;
	}

	fOpen = true;
	return TreeStatus::Ok;
}


bool BPlusTreeReader::MaxSizeMatchesStream() const
{
	return fHeader.maximumSize == static_cast<int64_t>(fTree.size());
}


int64_t BPlusTreeReader::NodeCount() const
{
	if (!fOpen || fHeader.nodeSize <= 0) {
		return 0;
	}
	return fHeader.maximumSize / fHeader.nodeSize;
}


bool BPlusTreeReader::ValidLink(int64_t link) const
{
	return fOpen && link >= fHeader.nodeSize
		&& link + fHeader.nodeSize <= fHeader.maximumSize
		&& (link % fHeader.nodeSize) == 0;
}


bool BPlusTreeReader::InBounds(int64_t link) const
{
	return fOpen && link >= 0
		&& link + fHeader.nodeSize <= static_cast<int64_t>(fTree.size());
}


TreeStatus BPlusTreeReader::ReadNodeHeader(int64_t offset, TreeNode &node) const
{
	if (!InBounds(offset)) {
		return TreeStatus::LinkOutOfRange;
	}

	const uint8_t *p = fTree.data() + offset;
	node.offset = offset;
	node.leftLink = S64(p + btreenode::kLeftLink);
	node.rightLink = S64(p + btreenode::kRightLink);
	node.overflowLink = S64(p + btreenode::kOverflowLink);
	node.keyCount = U16(p + btreenode::kAllKeyCount);
	node.keyLength = U16(p + btreenode::kAllKeyLength);
	node.keys.clear();

	// The key data, the key-length table, and the value array all live inside
	// the node. Rejecting a node whose arrays do not fit is what makes reading
	// the key table below a bounded operation.
	node.used = KeyAlign(btreenode::kSize + node.keyLength)
		+ static_cast<int64_t>(node.keyCount) * (sizeof(uint16_t) + sizeof(int64_t));
	if (node.used > fHeader.nodeSize) {
		return TreeStatus::UsedExceedsNodeSize;
	}

	return TreeStatus::Ok;
}


TreeStatus BPlusTreeReader::ReadNodeKeys(TreeNode &node) const
{
	node.keys.clear();
	if (!InBounds(node.offset)) {
		return TreeStatus::LinkOutOfRange;
	}

	const uint8_t *p = fTree.data() + node.offset;
	const uint8_t *keys = p + btreenode::kSize;
	const uint8_t *lengthTable = p + KeyAlign(btreenode::kSize + node.keyLength);
	const uint8_t *values = lengthTable + node.keyCount * sizeof(uint16_t);

	node.keys.reserve(node.keyCount);
	uint16_t previous = 0;
	for (uint16_t k = 0; k < node.keyCount; k++) {
		uint16_t cumulative = U16(lengthTable + k * sizeof(uint16_t));
		if (cumulative < previous) {
			return TreeStatus::KeyTableNotMonotonic;
		}
		uint16_t length = static_cast<uint16_t>(cumulative - previous);
		if (length == 0 || length > kBPlusTreeMaxKeyLength) {
			return TreeStatus::KeyLengthOutOfRange;
		}

		TreeKey key;
		key.data = keys + previous;
		key.length = length;
		key.value = S64(values + k * sizeof(int64_t));
		node.keys.push_back(key);
		previous = cumulative;
	}

	// The keys are complete and usable even here, so callers that only want the
	// content can carry on; the mismatch still says the node is malformed.
	if (previous != node.keyLength) {
		return TreeStatus::KeyTableEndMismatch;
	}
	return TreeStatus::Ok;
}


TreeStatus BPlusTreeReader::ReadNode(int64_t offset, TreeNode &node) const
{
	TreeStatus status = ReadNodeHeader(offset, node);
	if (status != TreeStatus::Ok) {
		return status;
	}
	return ReadNodeKeys(node);
}


TreeStatus BPlusTreeReader::ResolveValue(int64_t link, TreeValue &value) const
{
	value = TreeValue();
	value.raw = link;

	uint64_t type = static_cast<uint64_t>(link) >> 62;
	if (type == 0) {
		value.kind = ValueKind::Inode;
		value.values.push_back(link);
		return TreeStatus::Ok;
	}

	int64_t offset = link & 0x3ffffffffffffc00LL;
	value.containerOffset = offset;
	if (!InBounds(offset)) {
		value.chainOk = false;
		return TreeStatus::LinkOutOfRange;
	}
	value.containers.push_back(offset);

	const uint8_t *base = fTree.data();
	if (type == static_cast<uint64_t>(kBPlusTreeDuplicateFragment)) {
		// Fragment 'index' starts at int64 index index * (kNumFragmentValues + 1)
		// within the node, its count first (section 10, "Duplicate handling").
		value.kind = ValueKind::DuplicateFragment;
		value.fragmentIndex = link & 0x3ff;

		int64_t slot = value.fragmentIndex * (kNumFragmentValues + 1);
		int64_t start = offset + slot * static_cast<int64_t>(sizeof(int64_t));
		if (start + (kNumFragmentValues + 1) * static_cast<int64_t>(sizeof(int64_t))
				> static_cast<int64_t>(fTree.size())) {
			value.chainOk = false;
			return TreeStatus::LinkOutOfRange;
		}

		int64_t count = S64(base + start);
		if (count < 0 || count > kNumFragmentValues) {
			value.chainOk = false;
			return TreeStatus::LinkOutOfRange;
		}
		for (int64_t i = 0; i < count; i++) {
			value.values.push_back(S64(base + start + (i + 1) * sizeof(int64_t)));
		}
		return TreeStatus::Ok;
	}

	// A duplicate node reuses the header words: the count sits where
	// overflow_link would be, values start at int64 index 3, and further nodes
	// for the same key chain through right_link.
	value.kind = ValueKind::DuplicateNode;
	int64_t chain = offset;
	int guard = 0;
	while (chain != kBPlusTreeNull) {
		if (!InBounds(chain) || ++guard > kMaxDuplicateChain) {
			value.chainOk = false;
			return TreeStatus::LinkOutOfRange;
		}

		if (chain != offset) {
			value.containers.push_back(chain);
		}
		const uint8_t *node = base + chain;
		int64_t count = S64(node + 2 * sizeof(int64_t));
		if (count < 0 || count > kNumDuplicateValues) {
			value.chainOk = false;
			return TreeStatus::LinkOutOfRange;
		}
		int64_t available = (fHeader.nodeSize / static_cast<int64_t>(sizeof(int64_t))) - 3;
		for (int64_t i = 0; i < std::min(count, available); i++) {
			value.values.push_back(S64(node + (3 + i) * sizeof(int64_t)));
		}

		chain = S64(node + 1 * sizeof(int64_t));
	}
	return TreeStatus::Ok;
}


TreeStatus BPlusTreeReader::ForEachLeafEntry(
	const std::function<bool(const TreeKey &key)> &visitor) const
{
	if (!fOpen) {
		return TreeStatus::StreamTooSmall;
	}

	// Descend to the leftmost leaf. Every node is validated on the way down,
	// including the internal ones: the value array of a node whose key count
	// does not fit would otherwise be read past the end of the stream.
	int64_t offset = fHeader.rootNodePointer;
	TreeNode node;
	for (int depth = 0; depth < kMaxDepth; depth++) {
		TreeStatus status = ReadNode(offset, node);
		if (status != TreeStatus::Ok) {
			return status;
		}
		if (node.IsLeaf()) {
			break;
		}
		if (node.keyCount == 0) {
			offset = node.overflowLink;
			continue;
		}
		offset = node.keys.front().value;   // leftmost child
	}

	// Walk the leaf chain left to right.
	std::set<int64_t> visited;
	while (offset != kBPlusTreeNull) {
		if (!visited.insert(offset).second) {
			return TreeStatus::LinkOutOfRange;   // cycle in the leaf chain
		}
		TreeStatus status = ReadNode(offset, node);
		if (status != TreeStatus::Ok) {
			return status;
		}
		for (const TreeKey &key : node.keys) {
			if (!visitor(key)) {
				return TreeStatus::Ok;
			}
		}
		offset = node.rightLink;
	}
	return TreeStatus::Ok;
}


TreeStatus BPlusTreeReader::ForEachNode(const std::function<bool(const TreeNode &node,
	int level, int64_t indexInLevel, TreeStatus status)> &visitor) const
{
	if (!fOpen) {
		return TreeStatus::StreamTooSmall;
	}

	std::set<int64_t> visited;
	std::vector<int64_t> level = {fHeader.rootNodePointer};

	for (int depth = 0; depth < kMaxDepth && !level.empty(); depth++) {
		std::vector<int64_t> next;
		int64_t indexInLevel = 0;

		for (int64_t offset : level) {
			if (!visited.insert(offset).second) {
				continue;   // already seen: a cycle, not a second real node
			}
			TreeNode node;
			node.offset = offset;
			TreeStatus status = ReadNodeHeader(offset, node);
			if (status == TreeStatus::Ok) {
				// Partial keys are still worth reporting, so a key-table fault
				// is surfaced rather than replacing the node's other fields.
				status = ReadNodeKeys(node);
			}
			if (!visitor(node, depth, indexInLevel, status)) {
				return TreeStatus::Ok;
			}
			indexInLevel++;

			if (status != TreeStatus::LinkOutOfRange
				&& status != TreeStatus::UsedExceedsNodeSize && !node.IsLeaf()) {
				for (const TreeKey &key : node.keys) {
					next.push_back(key.value);
				}
				next.push_back(node.overflowLink);
			}
		}
		level = std::move(next);
	}
	return TreeStatus::Ok;
}


TreeStatus BPlusTreeReader::ForEachFreeNode(
	const std::function<bool(int64_t offset)> &visitor) const
{
	if (!fOpen) {
		return TreeStatus::StreamTooSmall;
	}

	std::set<int64_t> visited;
	int64_t offset = fHeader.freeNodePointer;
	while (offset != kBPlusTreeNull) {
		if (!InBounds(offset)) {
			return TreeStatus::LinkOutOfRange;
		}
		if (!visited.insert(offset).second) {
			return TreeStatus::LinkOutOfRange;   // cycle in the free list
		}
		if (!visitor(offset)) {
			return TreeStatus::Ok;
		}
		offset = S64(fTree.data() + offset + btreenode::kLeftLink);
	}
	return TreeStatus::Ok;
}


} // bfs
