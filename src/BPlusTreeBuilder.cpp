#include "BPlusTreeBuilder.h"

#include <string.h>

#include <stdexcept>

#include "Endian.h"


namespace bfs {


namespace {


int64_t NodeUsed(int64_t keyCount, int64_t keyLength)
{
	return KeyAlign(btreenode::kSize + keyLength)
		+ keyCount * (static_cast<int64_t>(sizeof(uint16_t)) + sizeof(int64_t));
}


int64_t MakeDuplicateLink(int64_t nodeOffset)
{
	return (kBPlusTreeDuplicateNode << 62)
		| (nodeOffset & 0x3ffffffffffffc00LL);
}


} // unnamed namespace


BPlusTreeBuilder::BPlusTreeBuilder(uint32_t dataType, ByteOrder order):
	fDataType(dataType),
	fOrder(order)
{
}


void BPlusTreeBuilder::AddKey(const uint8_t *key, uint16_t keyLength, uint32_t valueCount)
{
	if (valueCount == 0) {
		throw std::runtime_error("B+tree key must have at least one value");
	}
	if (keyLength == 0 || keyLength > kBPlusTreeMaxKeyLength) {
		throw std::runtime_error("B+tree key length out of range");
	}

	Entry entry;
	entry.key.assign(key, key + keyLength);
	entry.valueCount = valueCount;
	fEntries.push_back(std::move(entry));
}


void BPlusTreeBuilder::Finalize()
{
	// Assign each entry's slice of the eventual flat value array and plan the
	// duplicate nodes needed for entries with more than one value.
	fEntryDupFirst.assign(fEntries.size(), -1);
	uint64_t valueCursor = 0;
	for (size_t e = 0; e < fEntries.size(); e++) {
		fEntries[e].valueStart = valueCursor;
		valueCursor += fEntries[e].valueCount;

		if (fEntries[e].valueCount > 1) {
			fEntryDupFirst[e] = static_cast<int>(fDupNodes.size());
			uint32_t remaining = fEntries[e].valueCount;
			uint32_t local = 0;
			while (remaining > 0) {
				DupNode dup;
				dup.entry = static_cast<int>(e);
				dup.localStart = local;
				dup.count = remaining < static_cast<uint32_t>(kNumDuplicateValues)
					? remaining : static_cast<uint32_t>(kNumDuplicateValues);
				fDupNodes.push_back(dup);
				remaining -= dup.count;
				local += dup.count;
			}
		}
	}

	// Build leaf nodes by greedily packing keys until a node is full.
	if (fEntries.empty()) {
		PlanNode leaf;
		leaf.isLeaf = true;
		fNodes.push_back(std::move(leaf));
	} else {
		size_t i = 0;
		while (i < fEntries.size()) {
			PlanNode leaf;
			leaf.isLeaf = true;
			int64_t keyLength = 0;
			while (i < fEntries.size()) {
				uint16_t kl = static_cast<uint16_t>(fEntries[i].key.size());
				int64_t used = NodeUsed(static_cast<int64_t>(leaf.keyLengths.size()) + 1,
					keyLength + kl);
				if (used > kBPlusTreeNodeSize && !leaf.keyLengths.empty()) {
					break;
				}
				if (used > kBPlusTreeNodeSize) {
					throw std::runtime_error("single B+tree key too large for a node");
				}
				leaf.keyBytes.insert(leaf.keyBytes.end(),
					fEntries[i].key.begin(), fEntries[i].key.end());
				leaf.keyLengths.push_back(kl);
				leaf.leafEntry.push_back(static_cast<int>(i));
				keyLength += kl;
				i++;
			}
			leaf.maxKey = fEntries[leaf.leafEntry.back()].key;
			fNodes.push_back(std::move(leaf));
		}
	}

	int leafCount = static_cast<int>(fNodes.size());

	// Every level of a BFS B+tree is a doubly-linked list: each node's left/right
	// links point to its siblings at the same level (not just the leaves). Haiku's
	// checkfs validates these links at every level, so all levels must be chained.
	auto chainLevel = [this](const std::vector<int> &nodes) {
		for (size_t j = 0; j < nodes.size(); j++) {
			if (j > 0) {
				fNodes[nodes[j]].leftLink = NodeOffset(nodes[j - 1]);
			}
			if (j + 1 < nodes.size()) {
				fNodes[nodes[j]].rightLink = NodeOffset(nodes[j + 1]);
			}
		}
	};

	// Build internal levels bottom-up.
	std::vector<int> level;
	level.reserve(leafCount);
	for (int j = 0; j < leafCount; j++) {
		level.push_back(j);
	}

	// Chain the leaves left-to-right for in-order iteration.
	chainLevel(level);

	fLevels = 1;
	while (level.size() > 1) {
		std::vector<int> parents;
		size_t c = 0;
		while (c < level.size()) {
			PlanNode node;
			node.isLeaf = false;
			std::vector<int> children;
			int64_t sepKeyLength = 0;
			while (c < level.size()) {
				if (children.empty()) {
					children.push_back(level[c]);
					c++;
					continue;
				}
				// Adding another child promotes the current last child to a
				// separator key.
				const std::vector<uint8_t> &promote = fNodes[children.back()].maxKey;
				int64_t newKeyLength = sepKeyLength
					+ static_cast<int64_t>(promote.size());
				int64_t sepCount = static_cast<int64_t>(children.size());
				int64_t used = NodeUsed(sepCount, newKeyLength);
				if (used > kBPlusTreeNodeSize) {
					break;
				}
				node.keyBytes.insert(node.keyBytes.end(),
					promote.begin(), promote.end());
				node.keyLengths.push_back(static_cast<uint16_t>(promote.size()));
				node.childValue.push_back(NodeOffset(children.back()));
				sepKeyLength = newKeyLength;
				children.push_back(level[c]);
				c++;
			}

			node.overflowLink = NodeOffset(children.back());
			node.maxKey = fNodes[children.back()].maxKey;
			parents.push_back(static_cast<int>(fNodes.size()));
			fNodes.push_back(std::move(node));
		}
		// Chain this newly built internal level's siblings left-to-right.
		chainLevel(parents);
		level = std::move(parents);
		fLevels++;
	}

	fRootNode = level.front();

	int64_t totalNodes = static_cast<int64_t>(fNodes.size())
		+ static_cast<int64_t>(fDupNodes.size());
	fStreamSize = static_cast<uint64_t>((1 + totalNodes) * kBPlusTreeNodeSize);

	// Resolve duplicate-node chain links now that global node indices are known.
	int64_t dupBase = static_cast<int64_t>(fNodes.size());
	for (size_t d = 0; d < fDupNodes.size(); d++) {
		int64_t globalIndex = dupBase + static_cast<int64_t>(d);
		bool prevSameEntry = d > 0 && fDupNodes[d - 1].entry == fDupNodes[d].entry;
		bool nextSameEntry = d + 1 < fDupNodes.size()
			&& fDupNodes[d + 1].entry == fDupNodes[d].entry;
		if (prevSameEntry) {
			fDupNodes[d].leftLink = NodeOffset(globalIndex - 1);
		}
		if (nextSameEntry) {
			fDupNodes[d].rightLink = NodeOffset(globalIndex + 1);
		}
	}

	fFinalized = true;
}


void BPlusTreeBuilder::Serialize(std::vector<uint8_t> &out,
	const std::vector<int64_t> &valuesFlat) const
{
	if (!fFinalized) {
		throw std::runtime_error("BPlusTreeBuilder::Serialize before Finalize");
	}

	out.assign(fStreamSize, 0);

	// Tree header.
	WU32(&out[btreehdr::kMagic], kBPlusTreeMagic);
	WU32(&out[btreehdr::kNodeSize], kBPlusTreeNodeSize);
	WU32(&out[btreehdr::kMaxNumberOfLevels], fLevels);
	WU32(&out[btreehdr::kDataType], fDataType);
	WS64(&out[btreehdr::kRootNodePointer], NodeOffset(fRootNode));
	WS64(&out[btreehdr::kFreeNodePointer], kBPlusTreeNull);
	WS64(&out[btreehdr::kMaximumSize], static_cast<int64_t>(fStreamSize));

	// Regular nodes.
	for (size_t idx = 0; idx < fNodes.size(); idx++) {
		const PlanNode &node = fNodes[idx];
		uint8_t *base = &out[static_cast<size_t>(NodeOffset(static_cast<int64_t>(idx)))];
		uint16_t keyCount = static_cast<uint16_t>(node.keyLengths.size());
		uint16_t keyLength = static_cast<uint16_t>(node.keyBytes.size());

		WS64(base + btreenode::kLeftLink, node.leftLink);
		WS64(base + btreenode::kRightLink, node.rightLink);
		WS64(base + btreenode::kOverflowLink, node.overflowLink);
		WU16(base + btreenode::kAllKeyCount, keyCount);
		WU16(base + btreenode::kAllKeyLength, keyLength);

		if (keyLength > 0) {
			::memcpy(base + btreenode::kSize, node.keyBytes.data(), keyLength);
		}

		uint8_t *lengthTable = base
			+ KeyAlign(btreenode::kSize + keyLength);
		uint8_t *values = lengthTable + keyCount * sizeof(uint16_t);

		uint16_t cumulative = 0;
		for (uint16_t k = 0; k < keyCount; k++) {
			cumulative = static_cast<uint16_t>(cumulative + node.keyLengths[k]);
			WU16(lengthTable + k * sizeof(uint16_t), cumulative);

			int64_t value;
			if (node.isLeaf) {
				int entryIdx = node.leafEntry[k];
				const Entry &entry = fEntries[entryIdx];
				if (entry.valueCount == 1) {
					value = valuesFlat[entry.valueStart];
				} else {
					int dupListIndex = fEntryDupFirst[entryIdx];
					int64_t dupGlobal = static_cast<int64_t>(fNodes.size())
						+ dupListIndex;
					value = MakeDuplicateLink(NodeOffset(dupGlobal));
				}
			} else {
				value = node.childValue[k];
			}
			WS64(values + k * sizeof(int64_t), value);
		}
	}

	// Duplicate nodes: count at the overflow_link slot (int64 index 2), values
	// from int64 index 3 onward.
	int64_t dupBase = static_cast<int64_t>(fNodes.size());
	for (size_t d = 0; d < fDupNodes.size(); d++) {
		const DupNode &dup = fDupNodes[d];
		uint8_t *base = &out[static_cast<size_t>(
			NodeOffset(dupBase + static_cast<int64_t>(d)))];
		WS64(base + 0 * sizeof(int64_t), dup.leftLink);
		WS64(base + 1 * sizeof(int64_t), dup.rightLink);
		WS64(base + 2 * sizeof(int64_t), static_cast<int64_t>(dup.count));

		const Entry &entry = fEntries[dup.entry];
		for (uint32_t v = 0; v < dup.count; v++) {
			int64_t value = valuesFlat[entry.valueStart + dup.localStart + v];
			WS64(base + (3 + v) * sizeof(int64_t), value);
		}
	}
}


} // bfs
