#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>


namespace bfs {


enum class NodeKind {
	File,
	Directory,
	Symlink,
};


struct ScanTime {
	int64_t seconds = 0;
	uint32_t nanoseconds = 0;
};


// A BFS attribute read from the source (Haiku only). 'type' is the BFS/BeOS
// type code as returned by the attribute API and is copied through verbatim.
struct Attribute {
	std::string name;
	uint32_t type = 0;
	std::vector<uint8_t> data;
};


// One node of the source tree. The root Node represents the source directory
// itself. Fields under "assigned during build" are filled by BfsBuilder.
struct Node {
	NodeKind kind = NodeKind::File;
	std::string name;         // entry name; empty for the root
	std::string sourcePath;   // path on the host filesystem
	std::string linkTarget;   // symlink target text
	uint64_t dataSize = 0;    // file size, or symlink target length

	uint32_t posixMode = 0;   // permission bits from the source
	uint32_t uid = 0;
	uint32_t gid = 0;
	ScanTime createTime;
	ScanTime modifiedTime;
	ScanTime changeTime;

	std::vector<Attribute> attributes;
	std::vector<std::unique_ptr<Node>> children;

	int inodePlanIndex = -1;  // assigned during build
};


} // bfs
