#pragma once

#include <string>

#include "BfsReader.h"


namespace bfs {


struct ExtractOptions {
	bool restoreAttributes = true;   // effective on Haiku
	bool restoreOwnership = true;    // best effort; ignores EPERM
	bool verbose = false;
};


// Walks a BFS volume and writes its contents to a destination directory.
class Extractor {
public:
	Extractor(BfsReader &reader, const ExtractOptions &options);

	void Extract(const std::string &outputDir);

private:
	enum class Kind {
		File,
		Directory,
		Symlink,
	};

	void ExtractDirectory(const Inode &directory, const std::string &path);
	void ExtractFile(const Inode &inode, const std::string &path);
	void ExtractSymlink(const Inode &inode, const std::string &path);
	void RestoreMetadata(const Inode &inode, const std::string &path, Kind kind);

	BfsReader &fReader;
	ExtractOptions fOptions;
};


} // bfs
