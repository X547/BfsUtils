#pragma once

#include <stdint.h>

#include <functional>
#include <vector>

#include "BfsFormat.h"
#include "Endian.h"


namespace bfs {


// What a parse step found. Parsing reports rather than throws because the
// callers disagree on what a malformed tree means: BfsReader aborts the read,
// while Checker records a finding and keeps walking so one bad node does not
// hide the rest of the volume. Each caller maps these to its own diagnostics.
enum class TreeStatus {
	Ok,
	StreamTooSmall,
	BadMagic,
	BadNodeSize,
	LinkOutOfRange,
	UsedExceedsNodeSize,
	KeyTableNotMonotonic,
	KeyLengthOutOfRange,
	KeyTableEndMismatch,
};


// Stable machine-readable slug for a status, for tools that report structure
// rather than prose (bfsdump). Human-facing wording stays with each caller.
const char *TreeStatusName(TreeStatus status);


// Display name for a bplustree_types key type.
const char *KeyTypeName(uint32_t dataType);


// Order two keys as the given key type does. String and unknown types compare
// as unsigned bytes with the shorter key first.
int CompareKeys(uint32_t keyType, const uint8_t *a, uint16_t aLength,
	const uint8_t *b, uint16_t bLength, ByteOrder order);


struct TreeHeader {
	uint32_t magic = 0;
	int64_t nodeSize = 0;
	uint32_t maxNumberOfLevels = 0;
	uint32_t dataType = 0;
	int64_t rootNodePointer = 0;
	int64_t freeNodePointer = 0;
	int64_t maximumSize = 0;
};


// One key of a node, pointing into the tree stream, paired with the value that
// follows it in the node's value array (a child link in an internal node, a
// leaf value otherwise).
struct TreeKey {
	const uint8_t *data = nullptr;
	uint16_t length = 0;
	int64_t value = 0;
};


struct TreeNode {
	int64_t offset = 0;
	int64_t leftLink = 0;
	int64_t rightLink = 0;
	int64_t overflowLink = 0;
	uint16_t keyCount = 0;
	uint16_t keyLength = 0;
	int64_t used = 0;
	std::vector<TreeKey> keys;

	bool IsLeaf() const {return overflowLink == kBPlusTreeNull;}
};


enum class ValueKind {
	Inode,
	DuplicateNode,
	DuplicateFragment,
};


// A leaf value after duplicate resolution. A plain value carries a single inode
// in 'values'; a duplicate container carries every inode it holds.
struct TreeValue {
	ValueKind kind = ValueKind::Inode;
	int64_t raw = 0;
	int64_t containerOffset = 0;   // duplicate containers only: the head
	int64_t fragmentIndex = 0;     // fragments only
	std::vector<int64_t> values;

	// Every node-sized slot this value occupies -- the head plus each link of a
	// duplicate-node chain. Duplicate containers live in the same stream as the
	// tree's own nodes, so anything auditing which slots are in use has to count
	// these as well.
	std::vector<int64_t> containers;
	bool chainOk = true;
};


// Read-only parser for a B+tree that has already been read out of its inode's
// data stream. It decodes structure and leaves policy to the caller: which link
// values count as valid, and whether a malformed node aborts or is merely
// noted, differ between the checker and the ordinary readers.
class BPlusTreeReader {
public:
	BPlusTreeReader(const std::vector<uint8_t> &tree, ByteOrder order);

	// Parse the header and check what makes the tree unreadable at all: the
	// stream must hold a header, the magic must match, and node_size must be
	// usable. Everything else a caller might object to -- maximum_size
	// disagreeing with the stream, max_number_of_levels being zero, an
	// unreachable root_node_pointer -- is left to the caller, because the two
	// existing walkers judge those differently and report them in their own
	// order. Must succeed before anything else is called.
	TreeStatus Open();

	bool IsOpen() const {return fOpen;}
	const TreeHeader &Header() const {return fHeader;}

	// Whether the header's maximum_size agrees with the stream actually read.
	bool MaxSizeMatchesStream() const;

	// Total node-sized slots the header claims, whether or not they are in use.
	int64_t NodeCount() const;

	// The strict policy: a link must be node-aligned and sit inside the header's
	// maximum_size, past the header's own first node slot. Used where a link is
	// being judged rather than merely followed.
	bool ValidLink(int64_t link) const;

	// The permissive policy: the node merely has to lie inside the stream that
	// was actually read. Used where a link only needs to be safe to dereference.
	bool InBounds(int64_t link) const;

	// Decode a node's fixed header. Reports LinkOutOfRange for an unreachable
	// offset and UsedExceedsNodeSize when the key and value arrays do not fit,
	// which is what makes the key table safe to read afterwards.
	TreeStatus ReadNodeHeader(int64_t offset, TreeNode &node) const;

	// Decode the key-length table into 'node.keys'. Split from the node header
	// so a caller can still report header-level problems when the key table is
	// malformed. On KeyTableEndMismatch the keys are fully populated and usable;
	// on the other failures they are partial.
	TreeStatus ReadNodeKeys(TreeNode &node) const;

	// Header plus keys, for callers that treat any malformed node as fatal.
	TreeStatus ReadNode(int64_t offset, TreeNode &node) const;

	// Resolve a leaf value, expanding a duplicate container into every inode it
	// holds (see BFS_On-Disk_Format.md section 10, "Duplicate handling").
	TreeStatus ResolveValue(int64_t link, TreeValue &value) const;

	// Walk the leaf chain in key order: descend to the leftmost leaf, then follow
	// right links. 'visitor' returns false to stop early. A stream too small to
	// hold a header yields no entries and is not an error, matching a tree that
	// has never been written to.
	TreeStatus ForEachLeafEntry(
		const std::function<bool(const TreeKey &key)> &visitor) const;

	// Walk every node reachable from the root, breadth-first, so 'level' and
	// 'indexInLevel' describe each node's place in the tree. Cycle-guarded.
	// Malformed nodes are still visited, with 'status' saying what is wrong and
	// the node's fields left as far as they could be decoded, so a caller can
	// report the damage instead of the walk silently ending. 'visitor' returns
	// false to stop early.
	TreeStatus ForEachNode(const std::function<bool(const TreeNode &node,
		int level, int64_t indexInLevel, TreeStatus status)> &visitor) const;

	// Walk the free-node list from free_node_pointer. A free node stores the
	// next free offset in its left_link (section 10, "Free nodes").
	TreeStatus ForEachFreeNode(
		const std::function<bool(int64_t offset)> &visitor) const;

private:
	uint16_t U16(const uint8_t *p) const {return GetU16(p, fOrder);}
	uint32_t U32(const uint8_t *p) const {return GetU32(p, fOrder);}
	int64_t S64(const uint8_t *p) const {return GetS64(p, fOrder);}

	const std::vector<uint8_t> &fTree;
	ByteOrder fOrder;
	TreeHeader fHeader;
	bool fOpen = false;
};


} // bfs
