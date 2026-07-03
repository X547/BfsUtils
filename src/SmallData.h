#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "Node.h"


namespace bfs {


struct SmallDataResult {
	// Bytes to copy into the inode block at inode::kSmallDataStart.
	std::vector<uint8_t> bytes;
	// Attributes that did not fit inline and must be promoted to attribute
	// inodes in the node's attribute directory.
	std::vector<const Attribute *> largeAttributes;
};


// Build the small_data region for an inode: the mandatory name record followed
// by as many attributes as fit in the remaining inode space. 'smallDataCapacity'
// is blockSize - inode::kSmallDataStart.
SmallDataResult BuildSmallData(const std::string &name,
	const std::vector<Attribute> &attributes, size_t smallDataCapacity);


} // bfs
