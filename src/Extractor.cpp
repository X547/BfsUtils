#include "Extractor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdexcept>
#include <system_error>

#include "AttributeWriter.h"
#include "TimeEncoding.h"


namespace bfs {


namespace {


bool IsSafeName(const std::string &name)
{
	if (name.empty() || name == "." || name == "..") {
		return false;
	}
	return name.find('/') == std::string::npos;
}


void MakeDirectory(const std::string &path)
{
	if (::mkdir(path.c_str(), 0777) != 0 && errno != EEXIST) {
		throw std::system_error(errno, std::generic_category(), "mkdir " + path);
	}
}


void SetTimes(const std::string &path, int64_t modifiedTime, bool isSymlink)
{
	int64_t seconds = 0;
	uint32_t nanoseconds = 0;
	DecodeTime(modifiedTime, seconds, nanoseconds);

	struct timespec times[2];
	times[0].tv_sec = static_cast<time_t>(seconds);   // atime
	times[0].tv_nsec = nanoseconds;
	times[1].tv_sec = static_cast<time_t>(seconds);   // mtime
	times[1].tv_nsec = nanoseconds;
	::utimensat(AT_FDCWD, path.c_str(), times,
		isSymlink ? AT_SYMLINK_NOFOLLOW : 0);
}


} // unnamed namespace


Extractor::Extractor(BfsReader &reader, const ExtractOptions &options):
	fReader(reader),
	fOptions(options)
{
}


void Extractor::Extract(const std::string &outputDir)
{
	MakeDirectory(outputDir);
	Inode root = fReader.ReadInode(fReader.GetGeometry().ToBlock(
		fReader.RootDirectory()));
	ExtractDirectory(root, outputDir);
	RestoreMetadata(root, outputDir, Kind::Directory);
}


void Extractor::ExtractDirectory(const Inode &directory, const std::string &path)
{
	for (const DirEntry &entry : fReader.ReadDirectory(directory)) {
		if (!IsSafeName(entry.name)) {
			fprintf(stderr, "bfsextract: skipping unsafe entry name '%s'\n",
				entry.name.c_str());
			continue;
		}
		std::string childPath = path + "/" + entry.name;
		Inode child = fReader.ReadInode(entry.inode);

		if (fOptions.verbose) {
			fprintf(stderr, "%s\n", childPath.c_str());
		}

		if (child.IsDirectory()) {
			MakeDirectory(childPath);
			ExtractDirectory(child, childPath);
			RestoreMetadata(child, childPath, Kind::Directory);
		} else if (child.IsSymlink()) {
			ExtractSymlink(child, childPath);
			RestoreMetadata(child, childPath, Kind::Symlink);
		} else if (child.IsRegularFile()) {
			ExtractFile(child, childPath);
			RestoreMetadata(child, childPath, Kind::File);
		} else {
			fprintf(stderr, "bfsextract: skipping unsupported node '%s'\n",
				childPath.c_str());
		}
	}
}


void Extractor::ExtractFile(const Inode &inode, const std::string &path)
{
	int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		throw std::system_error(errno, std::generic_category(), "create " + path);
	}

	try {
		fReader.ReadStream(inode.stream, [&](const uint8_t *data, size_t length) {
			size_t written = 0;
			while (written < length) {
				ssize_t n = ::write(fd, data + written, length - written);
				if (n < 0) {
					if (errno == EINTR) {
						continue;
					}
					throw std::system_error(errno, std::generic_category(),
						"write " + path);
				}
				written += static_cast<size_t>(n);
			}
		});
	} catch (...) {
		::close(fd);
		throw;
	}
	::close(fd);
}


void Extractor::ExtractSymlink(const Inode &inode, const std::string &path)
{
	std::string target = fReader.ReadSymlink(inode);
	::unlink(path.c_str());
	if (::symlink(target.c_str(), path.c_str()) != 0) {
		throw std::system_error(errno, std::generic_category(), "symlink " + path);
	}
}


void Extractor::RestoreMetadata(const Inode &inode, const std::string &path,
	Kind kind)
{
	if (fOptions.restoreAttributes) {
		WriteNodeAttributes(path, fReader.ReadAttributes(inode));
	}

	if (fOptions.restoreOwnership) {
		// Best effort: extraction as non-root cannot chown.
		::lchown(path.c_str(), static_cast<uid_t>(inode.uid),
			static_cast<gid_t>(inode.gid));
	}

	if (kind != Kind::Symlink) {
		::chmod(path.c_str(), inode.mode & 07777);
	}

	// Set timestamps last: writing data and attributes bumps them.
	SetTimes(path, inode.modifiedTime, kind == Kind::Symlink);
}


} // bfs
