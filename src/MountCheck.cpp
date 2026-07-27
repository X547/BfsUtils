#include "MountCheck.h"

#include <sys/stat.h>

#if defined(__linux__)
#	include <stdio.h>
#	include <sys/sysmacros.h>
#elif defined(__HAIKU__)
#	include <fs_info.h>
#	include <string.h>
#endif


namespace bfs {


bool IsDeviceMounted(const std::string &path, std::string &where)
{
	where.clear();

#if defined(__linux__)
	// Match on the device's major:minor rather than its path: /dev/sda1,
	// /dev/disk/by-uuid/... and a symlink to either are all the same device, and
	// only the numbers are canonical.
	struct stat info;
	if (::stat(path.c_str(), &info) != 0 || !S_ISBLK(info.st_mode)) {
		return false;
	}
	FILE *file = ::fopen("/proc/self/mountinfo", "r");
	if (file == nullptr) {
		return false;
	}
	char line[4096];
	bool mounted = false;
	while (!mounted && ::fgets(line, sizeof(line), file) != nullptr) {
		// "<id> <parent> <major>:<minor> <root> <mount point> ..."
		int major = 0;
		int minor = 0;
		char point[4096];
		if (::sscanf(line, "%*d %*d %d:%d %*s %4095s", &major, &minor, point) != 3) {
			continue;
		}
		if (static_cast<unsigned>(major) == ::major(info.st_rdev)
			&& static_cast<unsigned>(minor) == ::minor(info.st_rdev)) {
			where = std::string("at ") + point;
			mounted = true;
		}
	}
	::fclose(file);
	return mounted;
#elif defined(__HAIKU__)
	// Haiku records the device each volume was mounted from as a path string.
	int32 cookie = 0;
	dev_t device;
	while ((device = next_dev(&cookie)) >= 0) {
		fs_info info;
		if (fs_stat_dev(device, &info) != 0) {
			continue;
		}
		if (path == info.device_name) {
			// fs_info records no mount point, so the volume name is the best
			// identification available without pulling in libbe.
			where = std::string("as volume \"") + info.volume_name + "\"";
			return true;
		}
	}
	return false;
#else
	(void)path;
	return false;
#endif
}


} // bfs
