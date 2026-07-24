#include <stdio.h>
#include <stdlib.h>

#include <stdexcept>
#include <string>

#include "BfsReader.h"
#include "BlockMap.h"
#include "ImageFile.h"
#include "MapRenderer.h"
#include "Png.h"


namespace {


void PrintUsage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [options] <image> [output.png]\n"
		"\n"
		"Render a BFS image's block usage as a color-coded PNG (read-only).\n"
		"Each block becomes one cell, colored by what it holds: free, reserved,\n"
		"bitmap, journal, inode, metadata, index, indirect, file data,\n"
		"fragmented file data, attribute data, or leaked.\n"
		"\n"
		"If no output path is given, the image path with a '.png' suffix is used.\n"
		"\n"
		"Options:\n"
		"      --width N        blocks per row in the grid (default: auto)\n"
		"      --scale N        pixels per block cell (default: auto)\n"
		"      --replay-log     map the post-replay state of an unclean volume\n"
		"  -h, --help           show this help\n"
		"\n"
		"Exit status: 0 = ok, 2 = could not open/parse/write.\n",
		program);
}


} // unnamed namespace


int main(int argc, char **argv)
{
	bfs::RenderOptions render;
	bool replayLog = false;
	std::string imagePath;
	std::string outputPath;

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			PrintUsage(argv[0]);
			return 0;
		} else if (arg == "--width") {
			if (++i >= argc) {
				fprintf(stderr, "%s: --width needs a value\n", argv[0]);
				return 2;
			}
			render.blocksPerRow = atoi(argv[i]);
		} else if (arg == "--scale") {
			if (++i >= argc) {
				fprintf(stderr, "%s: --scale needs a value\n", argv[0]);
				return 2;
			}
			render.scale = atoi(argv[i]);
		} else if (arg == "--replay-log") {
			replayLog = true;
		} else if (!arg.empty() && arg[0] == '-') {
			fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg.c_str());
			PrintUsage(argv[0]);
			return 2;
		} else if (imagePath.empty()) {
			imagePath = arg;
		} else if (outputPath.empty()) {
			outputPath = arg;
		} else {
			fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], arg.c_str());
			return 2;
		}
	}

	if (imagePath.empty()) {
		PrintUsage(argv[0]);
		return 2;
	}
	if (outputPath.empty()) {
		outputPath = imagePath + ".png";
	}

	try {
		bfs::ImageFile image(imagePath);
		bfs::BfsReader reader(image);

		if (replayLog && !reader.IsClean()) {
			reader.ReplayLog();
		}

		bfs::BlockMap map(reader);
		map.Build();

		bfs::Image picture = bfs::RenderBlockMap(map, reader.VolumeName(),
			reader.GetGeometry().blockSize, reader.UsedBlocks(), render);
		bfs::WritePng(outputPath, picture);

		printf("Wrote %s (%dx%d) for '%s': %lld blocks of %u bytes\n",
			outputPath.c_str(), picture.width, picture.height,
			reader.VolumeName().c_str(),
			static_cast<long long>(reader.GetGeometry().numBlocks),
			reader.GetGeometry().blockSize);
		return 0;
	} catch (const std::exception &error) {
		fprintf(stderr, "bfsmap: %s\n", error.what());
		return 2;
	}
}
