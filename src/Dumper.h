#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "BfsReader.h"


namespace bfs {


class JsonWriter;
struct TreeKey;


enum class Section {
	Superblock,
	Inode,
	DataStream,
	Directory,
	Index,
	BTreeMap,
	BTreeNodes,
};


// One thing the user asked to see. 'spec' selects an inode (a block number, an
// absolute path, a block_run, or one of the well-known names) except for
// Section::Index, where it is an index name and may be empty to mean "list
// them all".
struct DumpRequest {
	Section section;
	std::string spec;
};


struct DumpOptions {
	std::vector<DumpRequest> requests;
	int depth = 1;
	bool attributes = false;
	int64_t maxEntries = -1;   // -1 == unlimited
	int64_t maxNodes = -1;
	int64_t maxData = 256;
};


// Renders a read-only view of a BFS volume as JSON. A request that cannot be
// satisfied -- an unreadable inode, a corrupt tree -- is reported as an "error"
// member on that section rather than aborting, so one damaged structure still
// leaves a complete, parseable document.
class Dumper {
public:
	Dumper(BfsReader &reader, JsonWriter &json, const DumpOptions &options);

	void Dump();

private:
	bool Wants(Section section) const;

	int64_t ResolveInode(const std::string &spec);
	int64_t ResolvePath(const std::string &path);

	void WriteBlockRun(const char *name, const BlockRun &run);
	void WriteTime(const char *name, int64_t encoded);
	void WriteMode(uint32_t mode);
	void WriteFlags(uint32_t flags);
	void WriteTypeCode(const char *name, uint32_t type);
	void WriteBlob(const std::vector<uint8_t> &data);
	void WriteKey(uint32_t keyType, const TreeKey &key);
	void WriteAttributeList(const std::vector<Attribute> &attributes);

	void DumpSuperblock();
	void DumpInode(const std::string &spec);
	void DumpInodeBody(const Inode &inode);
	void DumpDataStream(const std::string &spec);
	void DumpStreamBody(const DataStreamInfo &stream);
	void DumpDirectory(const std::string &spec);
	void DumpDirectoryBody(int64_t block, int depth);
	void DumpIndices();
	void DumpIndexEntries(const std::string &name);
	void DumpBTree(const std::string &spec, bool nodes);
	void DumpBTreeMapBody(const Inode &inode, const std::vector<uint8_t> &tree);
	void DumpBTreeNodesBody(const Inode &inode, const std::vector<uint8_t> &tree);

	BfsReader &fReader;
	JsonWriter &fJson;
	DumpOptions fOptions;
	Geometry fGeo;
	ByteOrder fOrder;
};


} // bfs
