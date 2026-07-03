#include <stdio.h>
#include <string.h>

#include <stdexcept>
#include <string>

#include "BfsReader.h"
#include "Extractor.h"
#include "ImageFile.h"


namespace {


void PrintUsage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [options] <image> <output-directory>\n"
		"\n"
		"Extract the contents of a BFS image into a directory.\n"
		"\n"
		"Options:\n"
		"      --no-attributes  do not restore BFS attributes (Haiku only)\n"
		"      --no-owner       do not restore uid/gid ownership\n"
		"      --replay-log     replay the journal if the volume is not clean\n"
		"  -v, --verbose        print each extracted path\n"
		"  -h, --help           show this help\n",
		program);
}


} // unnamed namespace


int main(int argc, char **argv)
{
	bfs::ExtractOptions options;
	bool replayLog = false;
	std::string imagePath;
	std::string outputDir;

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			PrintUsage(argv[0]);
			return 0;
		} else if (arg == "--no-attributes") {
			options.restoreAttributes = false;
		} else if (arg == "--no-owner") {
			options.restoreOwnership = false;
		} else if (arg == "--replay-log") {
			replayLog = true;
		} else if (arg == "-v" || arg == "--verbose") {
			options.verbose = true;
		} else if (!arg.empty() && arg[0] == '-') {
			fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg.c_str());
			PrintUsage(argv[0]);
			return 1;
		} else if (imagePath.empty()) {
			imagePath = arg;
		} else if (outputDir.empty()) {
			outputDir = arg;
		} else {
			fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], arg.c_str());
			return 1;
		}
	}

	if (imagePath.empty() || outputDir.empty()) {
		PrintUsage(argv[0]);
		return 1;
	}

	try {
		bfs::ImageFile image(imagePath);
		bfs::BfsReader reader(image);

		if (!reader.IsClean()) {
			if (!replayLog) {
				fprintf(stderr,
					"bfsextract: volume '%s' is not clean (journal has pending "
					"transactions).\n           Pass --replay-log to replay it "
					"before extracting.\n",
					reader.VolumeName().c_str());
				return 1;
			}
			reader.ReplayLog();
		}

		bfs::Extractor extractor(reader, options);
		extractor.Extract(outputDir);
	} catch (const std::exception &error) {
		fprintf(stderr, "bfsextract: %s\n", error.what());
		return 1;
	}

	printf("Extracted '%s' into '%s'.\n", imagePath.c_str(), outputDir.c_str());
	return 0;
}
