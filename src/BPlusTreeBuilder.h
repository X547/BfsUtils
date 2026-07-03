#pragma once

#include <stdint.h>
#include <vector>

#include "BfsFormat.h"


namespace bfs {


// Bulk-loads an on-disk BFS B+tree from a set of keys supplied in ascending
// order. Used for directory trees (unique string keys) and index trees (typed
// keys that may repeat). Values (inode numbers) are not needed to determine the
// tree's layout, so the structure is fixed at Finalize() time and the concrete
// values are supplied later at Serialize() time. This lets the builder report
// the exact stream size during the measure phase and emit bytes during the
// place phase.
//
// Duplicate handling: a key with a single value stores that inode number inline
// in the leaf. A key with several values stores a link to one or more dedicated
// duplicate nodes (BPLUSTREE_DUPLICATE_NODE), chained when a key has more than
// NUM_DUPLICATE_VALUES values. (Fragment packing is a space optimization this
// builder does not perform.)
class BPlusTreeBuilder {
public:
	explicit BPlusTreeBuilder(uint32_t dataType);

	// Add a key that will hold 'valueCount' (>= 1) values. Keys must be added in
	// ascending order according to the tree's key type.
	void AddKey(const uint8_t *key, uint16_t keyLength, uint32_t valueCount);

	// Fix the node layout. Must be called after all AddKey() calls.
	void Finalize();

	// Total tree stream size in bytes (header + all nodes). Valid after
	// Finalize(). This is the value stored as the owning inode's data_stream.size
	// and as the tree header's maximum_size.
	uint64_t StreamSize() const {return fStreamSize;}

	uint32_t Levels() const {return fLevels;}

	// Serialize the whole tree into 'out'. 'valuesFlat' holds every value grouped
	// by key in the order keys were added; each group must contain exactly the
	// key's valueCount entries, sorted ascending for duplicated keys.
	void Serialize(std::vector<uint8_t> &out,
		const std::vector<int64_t> &valuesFlat) const;

private:
	struct Entry {
		std::vector<uint8_t> key;
		uint32_t valueCount = 0;
		uint64_t valueStart = 0;   // index into valuesFlat
	};

	struct PlanNode {
		bool isLeaf = false;
		int64_t leftLink = kBPlusTreeNull;
		int64_t rightLink = kBPlusTreeNull;
		int64_t overflowLink = kBPlusTreeNull;
		std::vector<uint16_t> keyLengths;
		std::vector<uint8_t> keyBytes;
		std::vector<int> leafEntry;      // leaf: entry index per key
		std::vector<int64_t> childValue; // internal: child offset per key
		std::vector<uint8_t> maxKey;     // largest key in this node's subtree
	};

	struct DupNode {
		int entry = 0;
		uint32_t localStart = 0;   // value offset within the entry's group
		uint32_t count = 0;
		int64_t leftLink = kBPlusTreeNull;
		int64_t rightLink = kBPlusTreeNull;
	};

	int64_t NodeOffset(int64_t nodeIndex) const
	{
		return (nodeIndex + 1) * kBPlusTreeNodeSize;
	}

	std::vector<Entry> fEntries;
	std::vector<PlanNode> fNodes;   // leaves first, then internal levels, root last
	std::vector<DupNode> fDupNodes; // appended after fNodes in the stream
	std::vector<int> fEntryDupFirst; // per entry: first dup-node list index, or -1

	uint32_t fDataType = 0;
	uint32_t fLevels = 1;
	int fRootNode = 0;
	uint64_t fStreamSize = 0;
	bool fFinalized = false;
};


} // bfs
