project('bfsutils', 'cpp',
	version : '0.1.0',
	default_options : ['cpp_std=c++20', 'warning_level=2'])


makebfs_sources = [
	'src/Progress.cpp',
	'src/ImageFile.cpp',
	'src/BlockAllocator.cpp',
	'src/DataStream.cpp',
	'src/SmallData.cpp',
	'src/BPlusTreeBuilder.cpp',
	'src/Attributes.cpp',
	'src/SourceScanner.cpp',
	'src/BfsBuilder.cpp',
	'src/main.cpp',
]

executable('makebfs', makebfs_sources,
	install : true)


bfsextract_sources = [
	'src/ImageFile.cpp',
	'src/BfsReader.cpp',
	'src/BPlusTreeReader.cpp',
	'src/AttributeWriter.cpp',
	'src/Extractor.cpp',
	'src/extract_main.cpp',
]

executable('bfsextract', bfsextract_sources,
	install : true)


bfscheck_sources = [
	'src/ImageFile.cpp',
	'src/BfsReader.cpp',
	'src/BPlusTreeReader.cpp',
	'src/Findings.cpp',
	'src/Checker.cpp',
	'src/check_main.cpp',
]

executable('bfscheck', bfscheck_sources,
	install : true)


bfsresize_sources = [
	'src/ImageFile.cpp',
	'src/BfsReader.cpp',
	'src/BPlusTreeReader.cpp',
	'src/ResizeJournal.cpp',
	'src/Resizer.cpp',
	'src/resize_main.cpp',
]

executable('bfsresize', bfsresize_sources,
	install : true)


libpng_dep = dependency('libpng')

bfsmap_sources = [
	'src/ImageFile.cpp',
	'src/BfsReader.cpp',
	'src/BPlusTreeReader.cpp',
	'src/BlockMap.cpp',
	'src/Png.cpp',
	'src/MapRenderer.cpp',
	'src/map_main.cpp',
]

executable('bfsmap', bfsmap_sources,
	dependencies : [libpng_dep],
	install : true)


# RapidJSON is header-only and ships its pkg-config file under either spelling
# depending on the distribution. Mark it as a system include so its own headers
# do not trip this project's warning level.
rapidjson_dep = dependency('RapidJSON', required : false)
if not rapidjson_dep.found()
	rapidjson_dep = dependency('rapidjson')
endif

bfsdump_sources = [
	'src/ImageFile.cpp',
	'src/BfsReader.cpp',
	'src/BPlusTreeReader.cpp',
	'src/JsonWriter.cpp',
	'src/Dumper.cpp',
	'src/dump_main.cpp',
]

executable('bfsdump', bfsdump_sources,
	dependencies : [rapidjson_dep.as_system('system')],
	install : true)
