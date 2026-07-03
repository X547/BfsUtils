#include <stdio.h>

#include <stdexcept>
#include <string>

#include "BfsReader.h"
#include "Checker.h"
#include "Findings.h"
#include "ImageFile.h"


namespace {


void PrintUsage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [options] <image>\n"
		"\n"
		"Check a BFS image for integrity problems (read-only; never modifies).\n"
		"\n"
		"Options:\n"
		"      --scan-orphans   scan all blocks for in-use inodes unreachable\n"
		"                       from the root (classifies leaked allocated blocks)\n"
		"      --replay-log     check the post-replay state of an unclean volume\n"
		"      --strict         treat warnings as failures too\n"
		"  -v, --verbose        verbose output\n"
		"  -h, --help           show this help\n"
		"\n"
		"Exit status: 0 = clean, 1 = problems found, 2 = could not open/parse.\n",
		program);
}


} // unnamed namespace


int main(int argc, char **argv)
{
	bfs::CheckOptions options;
	std::string imagePath;

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			PrintUsage(argv[0]);
			return 0;
		} else if (arg == "--scan-orphans") {
			options.scanOrphans = true;
		} else if (arg == "--replay-log") {
			options.replayLog = true;
		} else if (arg == "--strict") {
			options.strict = true;
		} else if (arg == "-v" || arg == "--verbose") {
			options.verbose = true;
		} else if (!arg.empty() && arg[0] == '-') {
			fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg.c_str());
			PrintUsage(argv[0]);
			return 2;
		} else if (imagePath.empty()) {
			imagePath = arg;
		} else {
			fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], arg.c_str());
			return 2;
		}
	}

	if (imagePath.empty()) {
		PrintUsage(argv[0]);
		return 2;
	}

	try {
		bfs::ImageFile image(imagePath);
		bfs::BfsReader reader(image);

		printf("Checking BFS volume '%s' (%lld blocks of %u bytes)\n",
			reader.VolumeName().c_str(),
			static_cast<long long>(reader.GetGeometry().numBlocks),
			reader.GetGeometry().blockSize);

		bfs::Findings findings;
		bfs::Checker checker(reader, findings, options);
		bool ok = checker.Run();
		return ok ? 0 : 1;
	} catch (const std::exception &error) {
		fprintf(stderr, "bfscheck: %s\n", error.what());
		return 2;
	}
}
