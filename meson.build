project('bfsutils', 'cpp',
	version : '0.1.0',
	default_options : ['cpp_std=c++17', 'warning_level=2'])


makebfs_sources = [
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
	'src/AttributeWriter.cpp',
	'src/Extractor.cpp',
	'src/extract_main.cpp',
]

executable('bfsextract', bfsextract_sources,
	install : true)


bfscheck_sources = [
	'src/ImageFile.cpp',
	'src/BfsReader.cpp',
	'src/Findings.cpp',
	'src/Checker.cpp',
	'src/check_main.cpp',
]

executable('bfscheck', bfscheck_sources,
	install : true)


bfsresize_sources = [
	'src/ImageFile.cpp',
	'src/BfsReader.cpp',
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
	'src/BlockMap.cpp',
	'src/Png.cpp',
	'src/MapRenderer.cpp',
	'src/map_main.cpp',
]

executable('bfsmap', bfsmap_sources,
	dependencies : [libpng_dep],
	install : true)
