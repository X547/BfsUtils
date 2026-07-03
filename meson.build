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
