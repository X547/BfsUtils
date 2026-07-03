#include "AttributeWriter.h"

#ifdef __HAIKU__

#include <fcntl.h>
#include <fs_attr.h>
#include <unistd.h>


namespace bfs {


void WriteNodeAttributes(const std::string &path,
	const std::vector<Attribute> &attributes)
{
	if (attributes.empty()) {
		return;
	}

	// O_RDONLY | O_NOTRAVERSE opens files, directories, and the symlink itself;
	// attribute writes go to the node regardless of the data open mode.
	int fd = ::open(path.c_str(), O_RDONLY | O_NOTRAVERSE);
	if (fd < 0) {
		return;
	}

	for (const Attribute &attribute : attributes) {
		::fs_write_attr(fd, attribute.name.c_str(), attribute.type, 0,
			attribute.data.data(), attribute.data.size());
	}

	::close(fd);
}


} // bfs

#else

namespace bfs {


void WriteNodeAttributes(const std::string &, const std::vector<Attribute> &)
{
	// No BFS attributes on non-Haiku platforms.
}


} // bfs

#endif
