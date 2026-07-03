#include "Attributes.h"

#ifdef __HAIKU__

#include <dirent.h>
#include <fcntl.h>
#include <fs_attr.h>
#include <unistd.h>

#include <stdio.h>


namespace bfs {


void ReadNodeAttributes(const std::string &path, std::vector<Attribute> &out)
{
	// Operate on the node itself, never the symlink target.
	int fd = ::open(path.c_str(), O_RDONLY | O_NOTRAVERSE);
	if (fd < 0) {
		return;
	}

	DIR *dir = ::fs_fopen_attr_dir(fd);
	if (dir == nullptr) {
		::close(fd);
		return;
	}

	struct dirent *entry;
	while ((entry = ::fs_read_attr_dir(dir)) != nullptr) {
		attr_info info;
		if (::fs_stat_attr(fd, entry->d_name, &info) != 0) {
			continue;
		}

		Attribute attribute;
		attribute.name = entry->d_name;
		attribute.type = info.type;
		attribute.data.resize(static_cast<size_t>(info.size));

		if (info.size > 0) {
			ssize_t read = ::fs_read_attr(fd, entry->d_name, info.type, 0,
				attribute.data.data(), static_cast<size_t>(info.size));
			if (read < 0) {
				continue;
			}
			attribute.data.resize(static_cast<size_t>(read));
		}

		out.push_back(std::move(attribute));
	}

	::fs_close_attr_dir(dir);
	::close(fd);
}


} // bfs

#else

namespace bfs {


void ReadNodeAttributes(const std::string &, std::vector<Attribute> &)
{
	// No BFS attributes on non-Haiku platforms.
}


} // bfs

#endif
