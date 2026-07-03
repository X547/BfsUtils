#include "SourceScanner.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "Attributes.h"


namespace bfs {


namespace {


ScanTime TimeFromSpec(const struct timespec &spec)
{
	ScanTime time;
	time.seconds = static_cast<int64_t>(spec.tv_sec);
	time.nanoseconds = static_cast<uint32_t>(spec.tv_nsec);
	return time;
}


std::string ReadLinkTarget(const std::string &path)
{
	std::vector<char> buffer(256);
	while (true) {
		ssize_t length = ::readlink(path.c_str(), buffer.data(), buffer.size());
		if (length < 0) {
			throw std::system_error(errno, std::generic_category(),
				"readlink " + path);
		}
		if (static_cast<size_t>(length) < buffer.size()) {
			return std::string(buffer.data(), static_cast<size_t>(length));
		}
		buffer.resize(buffer.size() * 2);
	}
}


std::unique_ptr<Node> ScanEntry(const std::string &path, const std::string &name,
	bool readAttributes);


void ScanChildren(Node &node, bool readAttributes)
{
	DIR *dir = ::opendir(node.sourcePath.c_str());
	if (dir == nullptr) {
		throw std::system_error(errno, std::generic_category(),
			"opendir " + node.sourcePath);
	}

	struct dirent *entry;
	while ((entry = ::readdir(dir)) != nullptr) {
		std::string name = entry->d_name;
		if (name == "." || name == "..") {
			continue;
		}
		std::string childPath = node.sourcePath + "/" + name;
		std::unique_ptr<Node> child = ScanEntry(childPath, name, readAttributes);
		if (child != nullptr) {
			node.children.push_back(std::move(child));
		}
	}

	::closedir(dir);

	// Deterministic output regardless of readdir order.
	std::sort(node.children.begin(), node.children.end(),
		[](const std::unique_ptr<Node> &a, const std::unique_ptr<Node> &b) {
			return a->name < b->name;
		});
}


std::unique_ptr<Node> ScanEntry(const std::string &path, const std::string &name,
	bool readAttributes)
{
	struct stat info;
	if (::lstat(path.c_str(), &info) != 0) {
		throw std::system_error(errno, std::generic_category(), "lstat " + path);
	}

	auto node = std::make_unique<Node>();
	node->name = name;
	node->sourcePath = path;
	node->posixMode = info.st_mode & 07777;
	node->uid = static_cast<uint32_t>(info.st_uid);
	node->gid = static_cast<uint32_t>(info.st_gid);
	node->modifiedTime = TimeFromSpec(info.st_mtim);
	node->changeTime = TimeFromSpec(info.st_ctim);
#ifdef __HAIKU__
	node->createTime = TimeFromSpec(info.st_crtim);
#else
	node->createTime = TimeFromSpec(info.st_mtim);
#endif

	if (S_ISDIR(info.st_mode)) {
		node->kind = NodeKind::Directory;
		ScanChildren(*node, readAttributes);
	} else if (S_ISLNK(info.st_mode)) {
		node->kind = NodeKind::Symlink;
		node->linkTarget = ReadLinkTarget(path);
		node->dataSize = node->linkTarget.size();
	} else if (S_ISREG(info.st_mode)) {
		node->kind = NodeKind::File;
		node->dataSize = static_cast<uint64_t>(info.st_size);
	} else {
		// Skip sockets, fifos, and device nodes: BFS archives regular content.
		fprintf(stderr, "makebfs: skipping unsupported node %s\n", path.c_str());
		return nullptr;
	}

	if (readAttributes) {
		ReadNodeAttributes(path, node->attributes);
	}

	return node;
}


} // unnamed namespace


std::unique_ptr<Node> ScanSource(const std::string &rootPath, bool readAttributes)
{
	struct stat info;
	if (::stat(rootPath.c_str(), &info) != 0) {
		throw std::system_error(errno, std::generic_category(), "stat " + rootPath);
	}
	if (!S_ISDIR(info.st_mode)) {
		throw std::runtime_error(rootPath + " is not a directory");
	}

	auto root = std::make_unique<Node>();
	root->kind = NodeKind::Directory;
	root->name.clear();
	root->sourcePath = rootPath;
	root->posixMode = info.st_mode & 07777;
	root->uid = static_cast<uint32_t>(info.st_uid);
	root->gid = static_cast<uint32_t>(info.st_gid);
	root->modifiedTime = TimeFromSpec(info.st_mtim);
	root->changeTime = TimeFromSpec(info.st_ctim);
#ifdef __HAIKU__
	root->createTime = TimeFromSpec(info.st_crtim);
#else
	root->createTime = TimeFromSpec(info.st_mtim);
#endif

	if (readAttributes) {
		ReadNodeAttributes(rootPath, root->attributes);
	}
	ScanChildren(*root, readAttributes);
	return root;
}


} // bfs
