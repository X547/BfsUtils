#pragma once

#include <stdint.h>

#include <map>
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
	bool resolveValues = false;
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
	// How much of a B+tree key a dump writes. The map dump keeps to the decoded
	// key, which is what an entry is looked up by; the node dump also documents
	// the bytes, being the view that accounts for a node's contents.
	enum class KeyDetail {
		Decoded,
		WithBytes,
	};

	bool Wants(Section section) const;

	int64_t ResolveInode(const std::string &spec);
	int64_t ResolvePath(const std::string &path);

	void WriteBlockRun(const char *name, const BlockRun &run);
	void WriteTime(const char *name, int64_t encoded);
	void WriteMode(uint32_t mode);
	void WriteFlags(uint32_t flags);
	void WriteTypeCode(const char *name, uint32_t type);
	void WriteBlob(const std::vector<uint8_t> &data);
	void WriteKey(uint32_t keyType, const TreeKey &key, KeyDetail detail);
	void WriteAttributeList(const std::vector<Attribute> &attributes);
	void WriteTreeValues(const std::vector<int64_t> &values);

	const std::map<int64_t, std::string> &NamesIn(int64_t block);
	const std::string &PathOf(int64_t block);

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

	// Caches for --resolve-values, which asks the same questions over and over: a
	// whole index's worth of values tends to live in a handful of directories, and
	// every one of their paths shares the same ancestors. Both cache misses too --
	// an empty name map for a directory that will not read, an empty path for an
	// inode that cannot be reached -- so a damaged volume costs one attempt.
	std::map<int64_t, std::map<int64_t, std::string>> fDirNames;
	std::map<int64_t, std::string> fPaths;
};


} // bfs
