# BFS On-Disk Format

This document describes the on-disk layout, data structures, semantics, and
core algorithms of BFS (the "Be File System"). It is written to be
self-contained and implementation-neutral so that it can be used as a reference
for writing a driver, a read-only extractor, a data-recovery tool, or a
consistency checker on any operating system.

All structures shown here are the *on-disk* representation. Every multi-byte
integer field is stored in a fixed byte order recorded in the superblock (see
[Byte Order](#byte-order)); the accessor semantics below always describe the
logical (host) value after byte-swapping.

---

## Table of Contents

1. [Conventions and Primitive Types](#1-conventions-and-primitive-types)
2. [Volume Layout Overview](#2-volume-layout-overview)
3. [Block Addressing: `block_run`](#3-block-addressing-block_run)
4. [The Superblock](#4-the-superblock)
5. [The Block Bitmap and Allocation Groups](#5-the-block-bitmap-and-allocation-groups)
6. [Inodes](#6-inodes)
7. [Data Streams](#7-data-streams)
8. [The `small_data` Region and Attributes](#8-the-small_data-region-and-attributes)
9. [Symbolic Links](#9-symbolic-links)
10. [B+trees](#10-btrees)
11. [Directories](#11-directories)
12. [Attributes Stored as Inodes](#12-attributes-stored-as-inodes)
13. [Indices](#13-indices)
14. [The Journal (Log)](#14-the-journal-log)
15. [Timestamp Encoding](#15-timestamp-encoding)
16. [Mounting, Validation, and Recovery](#16-mounting-validation-and-recovery)
17. [Creating a Fresh Volume](#17-creating-a-fresh-volume)

---

## 1. Conventions and Primitive Types

### Integer types

The following fixed-width types are used throughout:

| Name     | Meaning                          |
|----------|----------------------------------|
| `int8`   | signed 8-bit                     |
| `uint8`  | unsigned 8-bit                   |
| `int16`  | signed 16-bit                    |
| `uint16` | unsigned 16-bit                  |
| `int32`  | signed 32-bit                    |
| `uint32` | unsigned 32-bit                  |
| `int64`  | signed 64-bit                    |
| `off_t`  | signed 64-bit file/block offset  |

All structures are packed (no implicit padding inserted by the compiler); the
byte offsets are exactly as declared.

### Byte order

BFS stores a byte-order marker in the superblock. On disk the value is the
four-character constant:

```c
#define SUPER_BLOCK_FS_LENDIAN   'BIGE'   /* the four ASCII bytes B, I, G, E */
```

Despite the name of the constant, this marker records the byte order in which
the volume's multi-byte fields are stored, fixed when the volume was created.
The marker is itself a 32-bit integer written in that order, so its bytes read
as `BIGE` on a big-endian volume and `EGIB` on a little-endian one. Both orders
are valid, and all multi-byte fields are swapped accordingly. Every accessor
described in this document returns the *logical* value after any required swap.

Some four-character constants (magic numbers, type tags) are themselves stored
as 32-bit integers whose bytes spell ASCII. For example `'BFS1'` is the 32-bit
value formed from the bytes `B`, `F`, `S`, `1`.

### Terminology

- **Block** — the fundamental allocation and I/O unit. Its size is a power of
  two between 1024 and 8192 bytes, recorded in the superblock.
- **Block number** — a zero-based index of a block from the start of the volume.
  Byte offset = `blockNumber << block_shift`.
- **Allocation group (AG)** — a contiguous span of blocks used to localize
  allocations. See [Section 5](#5-the-block-bitmap-and-allocation-groups).
- **Inode** — the on-disk metadata record for a file, directory, symlink,
  attribute, or index. Every inode occupies exactly one block.
- **`block_run`** — a compact (group, start, length) reference to a run of
  consecutive blocks; also used as an inode identifier. See
  [Section 3](#3-block-addressing-block_run).

---

## 2. Volume Layout Overview

A BFS volume is laid out as follows, from the start of the partition:

```
byte offset 0                          the "boot block" (512 bytes, not used by BFS)
byte offset 512                        the superblock (see note below)

block 0                                also covers the boot block / superblock region
block 1 .. (1 + bitmapBlocks - 1)      the block allocation bitmap
following blocks                       the journal / log (location from superblock)
remaining blocks                       inodes, directory/index B+trees, file data
```

Key points:

- The **superblock** is located at **byte offset 512** from the start of the
  device. (For historical reasons on some platforms it may instead be found at
  byte offset 0; a robust reader checks offset 512 first, then offset 0.)
- Block 0 conceptually contains the first 512-byte boot area plus the
  superblock; the superblock structure is padded out to a full block.
- The **block bitmap** begins immediately at **block 1** and spans
  `NumBitmapBlocks()` blocks (see the formula in
  [Section 5](#5-the-block-bitmap-and-allocation-groups)).
- The **log** immediately follows the bitmap. Its exact location and length are
  given by the `log_blocks` field of the superblock, and its head/tail pointers
  by `log_start` / `log_end`.
- All blocks up to and including the log are *reserved* at creation time (marked
  used in the bitmap): the boot block, the bitmap itself, and the log.

The block number of a data structure is always derived from a `block_run` or a
raw block number stored elsewhere; there is no fixed location for the root
directory other than what the superblock records.

---

## 3. Block Addressing: `block_run`

A `block_run` is the universal way BFS names a run of consecutive blocks. It is
also used as an inode number.

```c
struct block_run {
    int32   allocation_group;   // index of the allocation group
    uint16  start;              // first block within the group
    uint16  length;             // number of consecutive blocks
};

typedef block_run inode_addr;
```

- `allocation_group` selects an allocation group.
- `start` is the block index *within* that group.
- `length` is the count of consecutive blocks covered by the run.

Because `length` is 16 bits, the maximum run length is 65535 blocks:

```c
#define MAX_BLOCK_RUN_LENGTH   65535
```

A run of all zeros (`allocation_group == 0 && start == 0 && length == 0`) is the
canonical "empty / null" run. Note that block number 0 is always reserved, so a
valid data run never legitimately starts at group 0 / start 0 with the reserved
region; a zero run is therefore unambiguous as a terminator.

### Converting a `block_run` to a block number

Let `ag_shift` be the superblock's `ag_shift` field. Then:

```c
blockNumber = ((int64)allocation_group << ag_shift) | (int64)start;
byteOffset  = blockNumber << block_shift;
```

Conversely, to convert a block number to a `block_run` of length 1:

```c
allocation_group = blockNumber >> ag_shift;
start            = blockNumber & ((1 << ag_shift) - 1);
length           = 1;
```

### Inode numbers

The inode number (a.k.a. "vnode id") of a file *is* the block number of its
inode block — i.e. the `ToBlock()` conversion of the inode's `block_run`. An
inode's own address is stored inside the inode as `inode_num` (see
[Section 6](#6-inodes)).

### Validating a `block_run`

A run is well-formed when all of the following hold (used both when mounting and
when checking):

```c
allocation_group >= 0
allocation_group <= num_ags
start            <= (1 << ag_shift)
length           != 0
(start + length) <= (1 << ag_shift)
```

Two runs are *mergeable* when they are in the same group, adjacent
(`a.start + a.length == b.start`), and their combined length does not exceed
`MAX_BLOCK_RUN_LENGTH`.

---

## 4. The Superblock

The superblock is the root of everything. It occupies one block but its
meaningful fields fit well within the first 512 bytes after the boot area.

```c
#define BFS_DISK_NAME_LENGTH   32

struct disk_super_block {
    char        name[BFS_DISK_NAME_LENGTH]; // volume name, NUL-terminated
    int32       magic1;                     // == 'BFS1'
    int32       fs_byte_order;              // == 'BIGE'
    uint32      block_size;                 // bytes per block
    uint32      block_shift;                // log2(block_size)
    int64       num_blocks;                 // total blocks in the volume
    int64       used_blocks;                // allocated blocks
    int32       inode_size;                 // bytes per inode (== block_size)
    int32       magic2;                     // == 0xdd121031
    int32       blocks_per_ag;              // bitmap blocks per allocation group
    int32       ag_shift;                   // log2(blocks per allocation group)
    int32       num_ags;                    // number of allocation groups
    int32       flags;                      // 'CLEN' (clean) or 'DIRT' (dirty)
    block_run   log_blocks;                 // location + length of the log area
    int64       log_start;                  // log head (see journal section)
    int64       log_end;                    // log tail
    int32       magic3;                     // == 0x15b6830e
    inode_addr  root_dir;                   // block_run of the root directory
    inode_addr  indices;                    // block_run of the index directory
    int32       _reserved[8];
    int32       pad_to_block[87];           // pad the structure out to a block
};
```

### Magic constants

```c
#define SUPER_BLOCK_MAGIC1     'BFS1'
#define SUPER_BLOCK_MAGIC2     0xdd121031
#define SUPER_BLOCK_MAGIC3     0x15b6830e

#define SUPER_BLOCK_FS_LENDIAN 'BIGE'

#define SUPER_BLOCK_DISK_CLEAN 'CLEN'   // volume was unmounted cleanly
#define SUPER_BLOCK_DISK_DIRTY 'DIRT'   // volume may need log replay
```

### Field semantics

- **`name`** — volume label, up to 31 characters plus a NUL. `/` is disallowed.
- **`block_size` / `block_shift`** — `block_size == (1 << block_shift)`. Valid
  block sizes are 1024, 2048, 4096, 8192.
- **`num_blocks`** — total number of blocks. The device must be at least
  `num_blocks << block_shift` bytes.
- **`used_blocks`** — number of currently allocated blocks. Free blocks =
  `num_blocks - used_blocks`.
- **`inode_size`** — size of one inode, in bytes. It is always equal to
  `block_size` (one inode per block).
- **`blocks_per_ag`** — number of *bitmap* blocks assigned to each allocation
  group. Note this is the size of the group's slice of the bitmap, not the
  number of data blocks it governs.
- **`ag_shift`** — `log2` of the number of blocks in one allocation group. Thus
  one allocation group governs `1 << ag_shift` blocks. (`blocks_per_ag` bitmap
  blocks × `block_size` × 8 bits equals `1 << ag_shift`, except possibly for the
  last, partial group.)
- **`num_ags`** — number of allocation groups. Must equal
  `ceil(num_blocks / (1 << ag_shift))`.
- **`flags`** — `'CLEN'` if the volume was cleanly unmounted, `'DIRT'` otherwise.
- **`log_blocks`** — a `block_run` giving the location and length (in blocks) of
  the journal.
- **`log_start` / `log_end`** — offsets (in blocks, relative to the start of the
  log area) of the head and tail of the active log. When they are equal, the log
  is empty and the volume is consistent.
- **`root_dir`** — the `block_run` of the root directory inode.
- **`indices`** — the `block_run` of the index directory inode (may be a zero run
  if the volume has no indices).

### Validity checks

The superblock's magic is valid when `magic1 == 'BFS1' && magic2 == 0xdd121031 &&
magic3 == 0x15b6830e`. Beyond magic, a fully valid superblock also satisfies:

```c
block_size == inode_size
fs_byte_order == 'BIGE'
(1 << block_shift) == block_size
num_ags   >= 1
ag_shift  >= 1
blocks_per_ag >= 1
num_blocks >= 10
num_ags == ceil(num_blocks / (1 << ag_shift))
```

The superblock is written back to disk at byte offset 512.

---

## 5. The Block Bitmap and Allocation Groups

BFS tracks free/used blocks with a single flat bitmap that starts at **block 1**
(immediately after the superblock block). Each bit represents one block of the
volume: bit set = allocated, bit clear = free.

### Bitmap size

The number of blocks occupied by the bitmap is:

```c
bitsPerBlock  = block_size * 8;
bitmapBlocks  = ceil(num_blocks / bitsPerBlock);
```

Bit *N* corresponds to block *N* of the whole volume. The bitmap is an array of
32-bit words stored in the volume's byte order: within word `w = N / 32`, block
`N` is represented by bit `N % 32` (the least-significant bit is the lowest block
number). Each word is byte-swapped according to the volume's byte order before
its bits are tested.

### Allocation groups

The bitmap is partitioned into `num_ags` allocation groups. Group *i* owns a
contiguous slice of the bitmap:

```c
group[i].start_bitmap_block = 1 + i * blocks_per_ag;   // block number of its bitmap slice
group[i].bitmap_blocks      = blocks_per_ag;           // except the last group (see below)
group[i].num_bits           = blocks_per_ag * block_size * 8;
```

That is, each group governs `num_bits == (1 << ag_shift)` volume blocks. The
**last** group may be smaller: its `num_bits` equals the number of blocks that
remain (`num_blocks - i * bitsPerGroup`), and its bitmap block count is
`1 + ((num_bits - 1) >> (block_shift + 3))`.

The mapping from a `block_run` back to the bitmap is implicit: the run's
`allocation_group` selects the group, and `start` is the bit index within that
group.

### Reserved blocks

At volume creation, the boot block, the entire bitmap, and the entire log area
are marked used. Concretely, the first `ToBlock(log_blocks) + log_blocks.length`
blocks are reserved and `used_blocks` is set accordingly.

### Allocation semantics (informational)

Allocation is bit-scanning within a group: to allocate a run of *n* blocks the
allocator finds *n* consecutive clear bits (preferably near a hint, e.g. near the
parent directory), sets them, and returns a `block_run`. Freeing clears the
corresponding bits. Bitmap changes are journaled like any other metadata (see
[Section 14](#14-the-journal-log)). A read-only or recovery tool never needs to
allocate; it only needs to interpret bits when validating.

---

## 6. Inodes

Every file, directory, symlink, attribute, and index is described by a
`bfs_inode` that occupies exactly one block. The inode block number is the
inode's identity.

```c
#define INODE_MAGIC1              0x3bbe0ad9
#define SHORT_SYMLINK_NAME_LENGTH 144   // includes the terminating NUL
#define NUM_DIRECT_BLOCKS         12

struct bfs_inode {
    int32       magic1;             // == INODE_MAGIC1
    inode_addr  inode_num;          // this inode's own block_run
    int32       uid;                // owning user id
    int32       gid;                // owning group id
    int32       mode;               // POSIX mode + BFS type bits (see below)
    int32       flags;              // inode_flags (see below)
    int64       create_time;        // encoded time (see Section 15)
    int64       last_modified_time; // encoded time
    inode_addr  parent;            // block_run of the parent directory
    inode_addr  attributes;        // block_run of the attribute directory (or 0)
    uint32      type;               // attribute type code (attribute inodes only)

    int32       inode_size;         // == superblock inode_size
    uint32      etc;                // reserved / in-memory scratch

    union {
        data_stream data;                            // for files/dirs/indices
        char        short_symlink[SHORT_SYMLINK_NAME_LENGTH]; // inline symlink
    };

    int64       status_change_time; // encoded time (ctime)
    int32       pad[2];

    small_data  small_data_start[0]; // variable-length small_data region follows
};
```

### Field semantics

- **`magic1`** — must equal `0x3bbe0ad9` for a valid inode.
- **`inode_num`** — the inode's own `block_run`. Its `length` must be 1 (one
  block per inode).
- **`uid` / `gid`** — POSIX owner and group.
- **`mode`** — POSIX permission bits and file-type bits, extended with BFS type
  bits (see [Mode bits](#mode-bits) below).
- **`flags`** — see [inode flags](#inode-flags).
- **`create_time`, `last_modified_time`, `status_change_time`** — timestamps in
  BFS's encoded format ([Section 15](#15-timestamp-encoding)).
- **`parent`** — `block_run` of the directory that contains this inode.
- **`attributes`** — `block_run` of this inode's *attribute directory* (a hidden
  directory of attribute inodes), or a zero run if there is none. See
  [Section 12](#12-attributes-stored-as-inodes).
- **`type`** — for an attribute inode, the attribute's data type code; unused for
  ordinary files.
- **`inode_size`** — equals the superblock `inode_size`; used as a consistency
  check and to locate the end of the `small_data` region.
- **`data` / `short_symlink`** — a union: files, directories, and indices use the
  `data_stream` ([Section 7](#7-data-streams)); short symlinks store the link
  text inline here.
- **`small_data_start`** — the start of the variable-length `small_data` region
  that fills the remainder of the inode block ([Section 8](#8-the-small_data-region-and-attributes)).

### Mode bits

`mode` combines standard POSIX bits (`S_IFREG`, `S_IFDIR`, `S_IFLNK`, permission
bits) with BFS-specific type bits used to distinguish the different flavors of
"directory-like" inodes and index types:

```c
#define S_ATTR_DIR         01000000000   // attribute directory
#define S_ATTR             02000000000   // attribute (inode is an attribute)
#define S_INDEX_DIR        04000000000   // index, or the index directory

#define S_STR_INDEX        00100000000   // string index
#define S_INT_INDEX        00200000000   // int32 index
#define S_UINT_INDEX       00400000000   // uint32 index
#define S_LONG_LONG_INDEX  00010000000   // int64 index
#define S_ULONG_LONG_INDEX 00020000000   // uint64 index
#define S_FLOAT_INDEX      00040000000   // float index
#define S_DOUBLE_INDEX     00001000000   // double index

#define S_ALLOW_DUPS       00002000000   // allow duplicate entries

// standard POSIX type mask/values:
#define S_IFMT             00000170000
#define S_IFLNK            00000120000
#define S_IFREG            00000100000
#define S_IFDIR            00000040000
```

Classification rules:

- **Regular file** — `(mode & (S_IFMT | S_ATTR_DIR | S_ATTR | S_INDEX_DIR)) ==
  S_IFREG`.
- **Directory (any kind)** — `S_ISDIR(mode)` (contains a directory B+tree).
- **Plain directory** — `(mode & (S_ATTR_DIR | S_ATTR | S_INDEX_DIR | S_IFDIR))
  == S_IFDIR`.
- **Attribute directory** — `(mode & (S_ATTR_DIR|S_ATTR|S_INDEX_DIR)) ==
  S_ATTR_DIR`.
- **Attribute** — `(mode & (S_ATTR_DIR|S_ATTR|S_INDEX_DIR)) == S_ATTR`.
- **Index directory / index** — carries `S_INDEX_DIR`. The index *root* directory
  is **not** distinguished by its mode (it carries permission bits and a type bit
  just like an individual index); it is identified structurally, as the inode
  referenced by `superblock.indices`.
- **Symlink** — `S_ISLNK(mode)`.

### The key-type bit on containers

Any inode whose data stream holds a B+tree also carries the index-type bit that
describes that tree's **key type**, in addition to its category bit(s):

- A **plain directory** is `S_IFDIR | S_STR_INDEX` (its tree is keyed by name, a
  string), plus permission bits.
- An **attribute directory** is `S_ATTR_DIR | S_IFDIR | S_STR_INDEX`.
- The **index directory** is `S_INDEX_DIR | S_IFDIR | S_STR_INDEX`, plus
  permission bits.
- An **individual index** is `S_INDEX_DIR | S_IFDIR | <type>`, where `<type>` is
  the bit matching the indexed attribute (`S_STR_INDEX`, `S_LONG_LONG_INDEX`,
  etc.).

A conforming implementation derives a container's B+tree key type from this bit,
so it must be present and correct. (This is why `S_STR_INDEX` — nominally an
"index" bit — appears on ordinary directories: a directory *is* a string index of
its entry names.)

### Inode flags

```c
enum inode_flags {
    INODE_IN_USE          = 0x00000001,  // always set for a live inode
    INODE_LOGGED          = 0x00000008,  // data stream changes are journaled
    INODE_DELETED         = 0x00000010,  // inode is deleted
    INODE_NOT_READY       = 0x00000020,  // inode under construction; fields invalid
    INODE_LONG_SYMLINK    = 0x00000040,  // symlink text is in the data stream

    INODE_PERMANENT_FLAGS = 0x0000ffff,  // mask of flags stored persistently

    // the following are transient / in-memory only and should be ignored
    // by tools reading a quiescent volume:
    INODE_WAS_WRITTEN     = 0x00020000,
    INODE_IN_TRANSACTION  = 0x00040000,
    INODE_DONT_FREE_SPACE = 0x00080000,  // used by the checker only
};
```

Only the low 16 bits (`INODE_PERMANENT_FLAGS`) are meaningful on disk.

### Inode validity checks

A structurally valid inode satisfies:

```c
magic1 == INODE_MAGIC1
(flags & INODE_IN_USE) != 0
(flags & INODE_NOT_READY) == 0
inode_num.length == 1
inode_size == superblock.inode_size
parent      is a well-formed block_run within the volume
attributes  is a well-formed block_run within the volume (or a zero run)
```

A candidate block only holds an inode if its block number is greater than the
last log block and less than `num_blocks`.

---

## 7. Data Streams

The `data_stream` embedded in an inode maps a linear byte range (the file
contents, a directory's B+tree, a long symlink's text, or an attribute's value)
onto disk blocks. It uses a three-tier scheme: direct runs, an indirect block,
and a double-indirect block.

```c
#define NUM_DIRECT_BLOCKS  12

struct data_stream {
    block_run direct[NUM_DIRECT_BLOCKS];   // 12 direct runs
    int64     max_direct_range;            // bytes covered by direct[] (exclusive)
    block_run indirect;                    // run of blocks holding block_run arrays
    int64     max_indirect_range;          // bytes covered up to end of indirect
    block_run double_indirect;             // run of blocks holding arrays of arrays
    int64     max_double_indirect_range;   // bytes covered up to end of double indirect
    int64     size;                        // logical size of the stream, in bytes
};
```

### Meaning of the ranges

- **`size`** is the logical length of the stream in bytes.
- **`max_direct_range`** is the number of bytes covered by the direct runs (the
  sum of `direct[i].length` blocks, in bytes). File position `pos` is served from
  the direct runs when `max_direct_range == 0` *or* `pos < max_direct_range`.
- **`max_indirect_range`** is the byte offset up to which the indirect block adds
  coverage.
- **`max_double_indirect_range`** is the byte offset up to which the
  double-indirect block adds coverage.

These "max range" fields let a reader decide which tier to consult without
walking every run.

### Resolving a file position to a `block_run`

Given a byte position `pos` within `[0, size)`:

**1. Direct range** (`max_direct_range == 0 || pos < max_direct_range`):

Walk `direct[0..11]`, accumulating each run's byte length until the accumulated
end exceeds `pos`. The matching run and the byte offset of the run's start are
found:

```c
runBlockEnd = 0;
for (current = 0; current < NUM_DIRECT_BLOCKS; current++) {
    if (direct[current].IsZero())        // zero run terminates the list
        break;
    runBlockEnd += direct[current].length << block_shift;
    if (runBlockEnd > pos) {
        run    = direct[current];
        offset = runBlockEnd - (run.length << block_shift); // stream offset of run start
        break;
    }
}
```

**2. Indirect range** (`pos >= max_direct_range` and
`pos < max_indirect_range`):

`indirect` is a `block_run` whose blocks form a flat array of `block_run`
entries: `runs_per_block = block_size / sizeof(block_run)` per block, across
`indirect.length` blocks. Walk these entries in order, accumulating byte lengths
starting from `max_direct_range`, until the accumulated end exceeds `pos`. A zero
run terminates the list.

**3. Double-indirect range** (`max_double_indirect_range > 0` and
`pos >= max_indirect_range`):

The double-indirect tier uses fixed-size accounting. With
`base = double_indirect.length`:

```c
runsPerBlock  = block_size / sizeof(block_run);
directSize    = base * block_size;                 // bytes covered by one "direct" run here
indirectSize  = base * directSize * runsPerBlock;  // bytes covered by one indirect array
```

Then, with `start = pos - max_indirect_range`:

```c
index   = start / indirectSize;                    // which top-level entry
// read block:  double_indirect block  + index / runsPerBlock
// the entry at (index % runsPerBlock) is itself a block_run of an array of runs
current = (start % indirectSize) / directSize;     // which entry in that array
// read block:  that array's block + current / runsPerBlock
// the entry at (current % runsPerBlock) is the target data run
run    = <that entry>;
offset = max_indirect_range + index * indirectSize + current * directSize;
```

Every run in the double-indirect tier has the same fixed block length
(`double_indirect.length`), which is why the arithmetic is exact rather than
requiring a scan.

### Reading bytes

Once the containing `run` and the stream `offset` of that run's start are known,
the byte offset within the run is `pos - offset`. Convert the run to a block
number, add `(pos - offset) >> block_shift` blocks, and read from
`blockNumber << block_shift + ((pos - offset) & (block_size - 1))`. Reads that
cross run boundaries repeat the resolution for the next position. Reads are
clamped to `size`.

### Array block sizing constants

```c
#define NUM_ARRAY_BLOCKS           4
#define DOUBLE_INDIRECT_ARRAY_SIZE 4096
```

---

## 8. The `small_data` Region and Attributes

The bytes of the inode block after the fixed `bfs_inode` header form the
`small_data` region. It stores short attributes inline — most importantly the
entry's **name** — as a packed sequence of variable-length records.

```c
struct small_data {
    uint32  type;        // attribute type code
    uint16  name_size;   // length of name, in bytes (excluding terminators)
    uint16  data_size;   // length of data, in bytes
    char    name[0];     // name_size bytes, then padding, then data, then padding
};
```

### Record layout

Each `small_data` record is laid out as:

```
+--------------------------------------------------+
| type        (4 bytes)                            |
| name_size   (2 bytes)                            |
| data_size   (2 bytes)                            |
| name        (name_size bytes)                    |
| padding     (3 bytes: name NUL terminator + pad) |
| data        (data_size bytes)                    |
| padding     (1 byte: data NUL terminator)        |
+--------------------------------------------------+
```

Accessor arithmetic:

```c
Name() = (char*)&name[0];
Data() = (uint8*)Name() + name_size + 3;
Size() = sizeof(small_data) /*8*/ + name_size + 3 + data_size + 1;
Next() = (small_data*)((uint8*)this + Size());
```

`sizeof(small_data)` is 8 bytes (the fixed header). The `+3` after the name and
`+1` after the data account for NUL terminators plus alignment padding.

### Iterating

Start at `small_data_start` and repeatedly follow `Next()`. The region ends at
the first record that is *last*, which is defined as:

```c
IsLast(inode) == ( (addr)record > (addr)inode + inode.inode_size - sizeof(small_data)
                   || record.name_size == 0 );
```

In other words, iteration stops when a record's `name_size` is zero or the record
pointer runs past the end of the inode block. The area between the last real
record and the end of the block is zeroed.

### The name record

The entry's file name is itself a `small_data` record with a special one-byte
name tag rather than a textual key:

```c
#define FILE_NAME_TYPE        'CSTR'   // type code for a string
#define FILE_NAME_NAME        0x13     // the single-byte "name" tag stored in name[]
#define FILE_NAME_NAME_LENGTH 1        // name_size for the name record
```

To read a node's name: iterate the `small_data` records and find the one whose
first name byte equals `0x13` and whose `name_size` equals 1; its `data` is the
NUL-terminated name string.

### Small attributes vs. large attributes

Small attributes (a name/type/value that fits in the remaining inode space) live
entirely in the `small_data` region. When an attribute is too large, or the inode
runs out of `small_data` space, the attribute is promoted to a full attribute
inode stored in the node's attribute directory
([Section 12](#12-attributes-stored-as-inodes)).

The maximum length of an attribute value indexed in an index is:

```c
#define MAX_INDEX_KEY_LENGTH  255   // excluding a terminating NUL
```

---

## 9. Symbolic Links

A symlink stores its target path in one of two ways, selected by the
`INODE_LONG_SYMLINK` flag:

- **Short symlink** (flag clear): the target text is stored inline in the inode's
  `short_symlink` field, up to `SHORT_SYMLINK_NAME_LENGTH` (144) bytes including
  the terminating NUL. This shares storage (a union) with the `data_stream`.
- **Long symlink** (`INODE_LONG_SYMLINK` set): the target text is stored in the
  inode's data stream, exactly like file contents, and read with the normal data
  stream read path ([Section 7](#7-data-streams)). Its length is
  `data_stream.size`.

To read a link: if `INODE_LONG_SYMLINK` is set, read `size` bytes from the data
stream; otherwise copy the NUL-terminated `short_symlink` string.

---

## 10. B+trees

Directories and indices are stored as on-disk B+trees. A B+tree lives *inside* an
inode's data stream: the tree's bytes are the file contents of a directory or
index inode. The tree begins with a header, followed by fixed-size nodes.

### Tree header

```c
#define BPLUSTREE_MAGIC          0x69f6c2e8
#define BPLUSTREE_NODE_SIZE      1024
#define BPLUSTREE_MAX_KEY_LENGTH 256
#define BPLUSTREE_MIN_KEY_LENGTH 1

struct bplustree_header {
    uint32  magic;                // == BPLUSTREE_MAGIC
    uint32  node_size;            // size of each node in bytes (typically 1024)
    uint32  max_number_of_levels; // height bound of the tree
    uint32  data_type;            // key type (see bplustree_types)
    int64   root_node_pointer;    // byte offset (within the stream) of the root node
    int64   free_node_pointer;    // head of the free-node list, or BPLUSTREE_NULL
    int64   maximum_size;         // total size of the tree stream, in bytes
};
```

Node pointers (`root_node_pointer`, `free_node_pointer`, node links, etc.) are
**byte offsets within the tree's own data stream**, always multiples of
`node_size`.

Sentinel offsets:

```c
#define BPLUSTREE_NULL  -1LL   // "no node"
#define BPLUSTREE_FREE  -2LL   // node is on the free list
```

A header is valid when its magic matches, `node_size` is a supported size,
`max_number_of_levels` is at least 1, and `root_node_pointer` is a valid link.

### Key types

```c
enum bplustree_types {
    BPLUSTREE_STRING_TYPE = 0,
    BPLUSTREE_INT32_TYPE  = 1,
    BPLUSTREE_UINT32_TYPE = 2,
    BPLUSTREE_INT64_TYPE  = 3,
    BPLUSTREE_UINT64_TYPE = 4,
    BPLUSTREE_FLOAT_TYPE  = 5,
    BPLUSTREE_DOUBLE_TYPE = 6,
};
```

Directory trees always use `BPLUSTREE_STRING_TYPE`. Index trees use the type
matching the indexed attribute.

### Tree node

```c
struct bplustree_node {
    int64   left_link;      // sibling to the left  (same level), or NULL
    int64   right_link;     // sibling to the right (same level), or NULL
    int64   overflow_link;  // rightmost child (internal), or NULL for a leaf
    uint16  all_key_count;  // number of keys in this node
    uint16  all_key_length; // total bytes of all concatenated keys
    // followed by: key data, key-length table, and value array (see below)
};
```

A node is a **leaf** when `overflow_link == BPLUSTREE_NULL`; otherwise it is an
internal node.

> **Note on `sizeof(bplustree_node)`.** Per the packing convention in
> [Section 1](#byte-order), the fixed header is exactly **28 bytes** (three
> 8-byte links + two 2-byte counts) with **no tail padding** — the key data
> begins at offset 28. Do not use a compiler's natural `sizeof` of the struct
> as written, which (because of 8-byte alignment) would be 32 and would place
> the keys, key-length table, and values 4 bytes too far.

The three variable-length arrays that follow the fixed header are laid out in
this order:

1. **Keys** — the concatenated key bytes, starting immediately after the fixed
   header (at offset `sizeof(bplustree_node)`), `all_key_length` bytes total.
2. **Key-length table** — an array of `all_key_count` `uint16` values, located at
   `round_up(sizeof(bplustree_node) + all_key_length, 8)` (aligned up to an 8-byte
   boundary). Entry *i* holds the *cumulative* end offset of key *i* within the
   key data. Thus:
   - `keyLength[i] = table[i] - (i > 0 ? table[i-1] : 0)`
   - `keyStart[i]  = keys + (i > 0 ? table[i-1] : 0)`
3. **Values** — an array of `all_key_count` `int64` values, immediately after the
   key-length table. Value *i* corresponds to key *i*.

Alignment helper:

```c
// round up to the next 8-byte (sizeof(off_t)) boundary
key_align(x) = (x + 7) & ~7;

KeyLengths() = (uint16*)((char*)node + key_align(sizeof(bplustree_node) + all_key_length));
Values()     = (int64*) ((char*)KeyLengths() + all_key_count * sizeof(uint16));
Keys()       = (uint8*) node + sizeof(bplustree_node);
```

The bytes used by a node are therefore:

```c
Used() = key_align(sizeof(bplustree_node) + all_key_length)
       + all_key_count * (sizeof(uint16) + sizeof(int64));
```

### Meaning of values

For an **internal** node, value *i* is the byte offset (link) of the child
subtree whose keys are all `<= key[i]`. Keys greater than the last key descend
via `overflow_link` (the rightmost child).

For a **leaf** node, value *i* is the payload for key *i*:

- In a **directory** tree, the payload is the inode number (block number) of the
  named entry.
- In an **index** tree, the payload is either a single inode number, or a
  reference to a *duplicate* structure when several entries share the same key
  (see [Duplicates](#duplicate-handling)).

Every level of the tree is a doubly-linked list: a node's `left_link` /
`right_link` point to its immediate siblings **at the same level** (the previous
and next node in left-to-right key order), or `BPLUSTREE_NULL` at the ends. This
is not limited to the leaf level — internal levels are chained the same way. The
leaf chain in particular allows in-order iteration without repeatedly descending
the tree. A writer must chain all levels, not just the leaves.

### Key comparison / ordering

Keys are ordered by type:

- **Integer / float / double types**: numeric comparison of the fixed-width
  value.
- **String type**: byte-wise `strncmp` over `min(len1, len2)` bytes; if equal on
  the common prefix, the shorter string sorts first (compare lengths).

String keys in a directory are ordinary UTF-8 file names. The maximum key length
that participates in indexing is `MAX_INDEX_KEY_LENGTH` (255) bytes; the tree
structure itself allows keys up to `BPLUSTREE_MAX_KEY_LENGTH` (256).

### Searching

To find a key, start at `root_node_pointer`. Within each internal node do a
binary/linear search for the smallest key `>= target`; descend to that key's
value (child link), or to `overflow_link` if the target exceeds all keys. At a
leaf, a matching key yields its value (or the duplicate structure).

### Duplicate handling

When an index permits multiple entries with identical keys, the leaf value does
not point directly at an inode; instead it encodes a reference to a duplicate
container. The top two bits of a 64-bit link encode its type:

```c
#define BPLUSTREE_DUPLICATE_NODE     2
#define BPLUSTREE_DUPLICATE_FRAGMENT 3

LinkType(link)      = (uint64)link >> 62;                 // top 2 bits
IsDuplicate(link)   = (LinkType(link) & (2|3)) != 0;
FragmentOffset(link)= link & 0x3ffffffffffffc00;          // node byte offset
FragmentIndex(link) = link & 0x3ff;                       // which fragment in the node
MakeLink(type, off, fragIndex)
      = ((int64)type << 62) | (off & 0x3ffffffffffffc00) | (fragIndex & 0x3ff);
```

A **duplicate array** is the on-disk container of the actual values:

```c
struct duplicate_array {
    int64 count;        // number of valid entries
    int64 values[0];    // 'count' inode numbers, sorted ascending
};
```

Two granularities of duplicate storage exist, both hard-coded for a 1024-byte
node:

- **Fragment** (`BPLUSTREE_DUPLICATE_FRAGMENT`): a small duplicate array packed
  into part of a shared duplicate node. Each fragment holds up to
  `NUM_FRAGMENT_VALUES` values:

  ```c
  #define NUM_FRAGMENT_VALUES 7   // 1 count slot + 7 value slots == 8 int64 == 64 bytes
  ```

  A node used for fragments packs `MaxFragments = node_size / ((NUM_FRAGMENT_VALUES+1) *
  sizeof(int64))` fragments. Fragment *k* begins at int64 index `k *
  (NUM_FRAGMENT_VALUES + 1)` within the node.

- **Duplicate node** (`BPLUSTREE_DUPLICATE_NODE`): a whole node dedicated to one
  key's duplicates, holding up to `NUM_DUPLICATE_VALUES` values, and reusing the
  ordinary node header words for the array. When a key accumulates more
  duplicates than a node can hold, duplicate nodes are chained via the node's
  `left_link` / `right_link`.

  ```c
  #define NUM_DUPLICATE_VALUES 125
  ```

  In a duplicate node, the count is stored where `overflow_link` would be, and
  the values begin at int64 index 3 (after the three link words).

To enumerate all inodes for a duplicated key, resolve the link's type, read the
`count`, and read `count` values from the appropriate offset.

### Free nodes

Deleted tree nodes are put on a singly linked free list headed by
`free_node_pointer`; a free node's `left_link` stores the next free offset and it
is marked with `BPLUSTREE_FREE`. A reader can ignore the free list; it matters
only for writers reusing space.

---

## 11. Directories

A directory is an inode whose data stream contains a `BPLUSTREE_STRING_TYPE`
B+tree. The tree maps **entry name → inode number**.

- Keys are the UTF-8 entry names.
- Leaf values are the inode numbers (block numbers) of the entries.
- The special entries `.` and `..` **are** stored as ordinary tree keys: `.`'s
  value is the directory's own inode number and `..`'s value is the parent
  directory's inode number (for the root directory, `..` maps to the root
  itself). They are ordinary keys subject to the normal ordering, so `.` (byte
  `0x2e`) is **not** necessarily the first key: any name whose leading byte is
  below `0x2e` (for example `!`, `#`, `+`, `,`, `-`) sorts before it. Like every
  B+tree, the keys — including `.` and `..` — are held in full sorted order.
- To list a directory, open the B+tree in its data stream and iterate leaves in
  key order. For each entry, the value is the child inode number; load that inode
  to obtain its type, size, timestamps, etc. The `parent` `block_run` in each
  inode also records the parent independently, which aids recovery.
- Name lookup is a B+tree `Find` on the name key.

Because a directory's contents *are* a data stream, large directories transparently
use the indirect / double-indirect tiers just like large files.

The root directory's inode is located from `superblock.root_dir`.

---

## 12. Attributes Stored as Inodes

Every inode may own an **attribute directory**: a hidden directory of attribute
inodes, referenced by the owning inode's `attributes` `block_run`. It is created
lazily — `attributes` is a zero run until the first large attribute is added.

- The attribute directory is itself an inode with `S_ATTR_DIR` set in its `mode`;
  its data stream holds a directory B+tree keyed by attribute name, whose values
  are the attribute inode numbers.
- Each **attribute inode** has `S_ATTR` set in its `mode`,
  stores the attribute's data-type code in its `type` field, and stores the
  attribute value in its data stream (`data_stream.size` bytes).
- Small attributes are *not* promoted to inodes; they remain in the owner's
  `small_data` region ([Section 8](#8-the-small_data-region-and-attributes)).
  Reading "all attributes" of a node therefore requires walking both the
  `small_data` records and the attribute directory (if present).

---

## 13. Indices

Indices provide fast, ordered lookup of files by an attribute value (for
example, by name, by size, or by last-modified time). All indices live under a
single **index directory**, referenced by `superblock.indices`.

- The index directory is an inode carrying `S_INDEX_DIR | S_IFDIR | S_STR_INDEX`
  (plus permission bits). Its data stream is a directory B+tree keyed by index
  name, whose values are the individual index inodes. It is not distinguished
  from an individual index by its mode — its mode is the same as a string index —
  but structurally, as the inode `superblock.indices` points to.
- Each **index** is an inode carrying `S_INDEX_DIR | S_IFDIR` together with one of
  the index-type bits (`S_STR_INDEX`, `S_INT_INDEX`, `S_LONG_LONG_INDEX`, etc.).
  Its data stream is a B+tree whose key type matches both that type bit and the
  indexed attribute's data type, and whose leaf values are inode numbers (using
  the duplicate mechanism, since many files can share the same attribute value).
- Standard indices commonly created at volume initialization include:
  - `name` — string index
  - `size` — int64 index
  - `last_modified` — int64 index
  - `BEOS:APP_SIG` — string index

  (These are ordinary indices; there is nothing structurally special about them.)
- For time-based indices, the key is the encoded timestamp value
  ([Section 15](#15-timestamp-encoding)); the low bits used to disambiguate
  identical timestamps mean the index key is the full encoded value, not just the
  whole-second part.

### Discovering which fields are indexed

There is no separate table of indexed fields; the index directory *is* that
table. To determine which fields are currently indexed, enumerate the index
directory's B+tree: each entry name is an indexed field, and the corresponding
index inode's mode bits give that field's key type (`S_STR_INDEX`,
`S_INT_INDEX`, `S_LONG_LONG_INDEX`, etc.). Locate the index directory via
`superblock.indices`; if it is a zero run, no fields are indexed.

A volume may be created without indices; in that case `superblock.indices` is a
zero run.

---

## 14. The Journal (Log)

BFS journals **metadata** through a circular log to keep the volume consistent
across crashes. The log stores full copies of the blocks that a transaction
modifies; on replay, those blocks are written back to their home locations.

### Log location and pointers

- `superblock.log_blocks` — a `block_run` giving the first block and the length
  (in blocks) of the log area.
- `superblock.log_start` / `superblock.log_end` — head and tail offsets, measured
  in blocks **relative to the start of the log area**, and interpreted modulo the
  log length (the log is circular).
- When `log_start == log_end`, the log is empty and the volume is consistent.
- `superblock.flags` is `'DIRT'` while a mount is active and `'CLEN'` after a
  clean unmount.

Free space in the (circular) log:

```c
freeLogBlocks = (log_start <= log_end)
    ? logSize - log_end + log_start
    : log_start - log_end;
```

### Log entry format: `run_array`

Each log entry begins with a header block, the `run_array`, that lists the home
locations of the data blocks that follow it in the log:

```c
struct run_array {
    int32     count;      // number of valid runs
    int32     max_runs;   // capacity (see note below)
    block_run runs[0];    // 'count' runs, kept sorted by (group, start)
};
```

- The `run_array` occupies exactly one log block; `runs[]` fills the rest of the
  block.
- `max_runs` capacity for a block:

  ```c
  maxCount = (block_size - sizeof(run_array)) / sizeof(block_run);
  MaxRuns  = (maxCount < 128) ? maxCount : 127;   // capped at 127
  ```

  Note: the stored `max_runs` value is one greater than the usable count — an
  off-by-one that a validator must tolerate. Consistency check: the effective
  usable maximum is `MaxRuns(block_size) - 1`, and `count` must be in
  `[1, usableMax]`.
- The runs are sorted by allocation group then start block, and each must be a
  valid `block_run` within the volume.

### On-disk log entry layout

A log entry is a `run_array` block immediately followed by the data blocks it
describes, all placed consecutively in the circular log:

```
[ run_array block ][ data block ][ data block ] ... [ data block ]
        |               \___________ total = sum of runs[i].length ___________/
        |
        +-- runs[0..count-1] name the home block_runs for the data blocks,
            in the same order the data blocks appear in the log
```

The number of data blocks in an entry equals the sum of `runs[i].length` over
all runs. The next entry's `run_array` follows immediately (wrapping around the
end of the log area).

### Replaying the log

On mount, if `log_start != log_end` the log must be replayed (regardless of the
`'CLEN'`/`'DIRT'` flag, though a mismatch is worth noting):

1. Set `start = log_start`.
2. While `start != log_end`:
   a. Read the `run_array` at log block `start % logSize`; validate it.
   b. The data blocks follow starting at `(start + 1) % logSize`, contiguously in
      the log (wrapping).
   c. **First pass** — sanity-check the data blocks. If an entry would overwrite
      block 0 (the superblock region), verify that the replacement is itself a
      valid superblock.
   d. **Second pass** — for each run, write its data blocks from the log to their
      home byte offset (`ToOffset(run)`), block by block, advancing the log
      position (mod `logSize`).
   e. Advance `start` by `1 + (total data blocks in this entry)`.
3. After the whole log is replayed, set `log_start = log_end`, set the flag to
   `'CLEN'`, and write the superblock back.

Because the log holds complete block images keyed by home location, replay is
idempotent and simply restores the intended post-transaction state.

Writers append entries at `log_end` and advance it; a background flusher later
writes the cached home blocks and advances `log_start`, freeing log space. A
read-only or recovery tool only needs the replay logic above.

---

## 15. Timestamp Encoding

BFS packs a timestamp (seconds plus sub-second resolution) into a single 64-bit
value. The whole-seconds part occupies the high bits; the low bits carry a
sub-second component and a small disambiguator.

```c
#define INODE_TIME_SHIFT  16
#define INODE_TIME_MASK   0xfff0
```

### Encoding

```c
// from (seconds, nanoseconds):
encoded = ((int64)seconds << INODE_TIME_SHIFT) + unique_from_nsec(nanoseconds);
```

The low 16 bits hold the sub-second information produced by `unique_from_nsec`:

- For a **non-zero** nanosecond value, bits `[15:4]` (masked by
  `INODE_TIME_MASK = 0xfff0`) encode the nanoseconds scaled into a 12-bit field
  (`((nsec + 16383) >> 14)`), and the low 4 bits carry a small monotonically
  increasing counter to spread otherwise-identical timestamps across index
  buckets.
- For a **zero** nanosecond value, the low 12 bits hold a counter and the pattern
  `0xf000` is OR-ed in, i.e. bits `[15:12] == 0xF`. This range (`0xF000`–`0xFFFF`)
  is reserved to mark "no real sub-second timestamp".

### Decoding

```c
seconds = encoded >> INODE_TIME_SHIFT;

// nanoseconds:
if ((encoded & 0xF000) == 0xF000)
    nanoseconds = 0;                                  // reserved "no sub-second" range
else
    nanoseconds = (encoded & INODE_TIME_MASK) << 14;  // reconstruct ~10^9-scale value
```

The extra low-bit entropy exists specifically so that time-based index keys are
well distributed even when many files share the same whole-second timestamp. For
plain timestamp reporting, only the seconds (and optionally the reconstructed
nanoseconds) matter.

---

## 16. Mounting, Validation, and Recovery

A robust mount / open sequence:

1. **Read the superblock** from byte offset 512 (fall back to offset 0 if the
   magic is not found there). Validate magic and the structural invariants in
   [Section 4](#4-the-superblock). Derive `block_size`, `block_shift`, and
   `ag_shift` from it.
2. **Check the device size** is at least `num_blocks << block_shift` bytes.
3. **Replay the log** if `log_start != log_end`
   ([Section 14](#14-the-journal-log)). This makes the volume consistent. For a
   strictly read-only tool that cannot write, replay can be performed into an
   in-memory overlay so that subsequent reads observe the post-replay state.
4. **Initialize the block allocator view** by reading the bitmap (only needed if
   allocating or validating free space).
5. **Load the root directory** from `superblock.root_dir`; validate the inode.
6. **Load the index directory** from `superblock.indices` if it is non-zero.

### Consistency checking / recovery notes

- **Inode discovery**: a valid inode block has `magic1 == 0x3bbe0ad9`,
  `INODE_IN_USE` set, `inode_num.length == 1`, and `inode_size` equal to the
  superblock value. Candidate inode blocks lie above the log and below
  `num_blocks`. This lets a recovery tool scan for inodes even if directory trees
  are damaged.
- **Reconstructing hierarchy**: each inode records its `parent` `block_run`, so
  parent/child relationships can be rebuilt independent of directory B+trees.
- **Names**: even if a directory tree is corrupt, each inode's own name is in its
  `small_data` region (the record tagged `0x13`), enabling name recovery.
- **Free vs. used**: cross-check the bitmap against the set of blocks actually
  referenced by inode data streams (direct, indirect, double-indirect runs), the
  bitmap blocks, and the log. `used_blocks` should match the count of set bits.
- **B+tree integrity**: each node's `all_key_count` and `all_key_length` must fit
  within `node_size`; each key length must be `<= BPLUSTREE_MAX_KEY_LENGTH`; each
  leaf value must not be `-1`; and all node links must be `BPLUSTREE_NULL` or a
  valid multiple of `node_size` within `maximum_size`.

---

## 17. Creating a Fresh Volume

For completeness, the initialization procedure that a formatter follows:

1. Choose a `block_size` (1024, 2048, 4096, or 8192) and compute `block_shift`.
   `inode_size` is set equal to `block_size`.
2. `num_blocks = deviceSize / block_size`.
3. Choose the allocation-group geometry. Starting from a minimum group shift
   determined by the block size, increase `ag_shift` (doubling the bitmap blocks
   per group) until the number of groups is reasonable (a small number of groups
   for small disks; groups cap at 65536 blocks each, i.e. `ag_shift` up to 16).
   Record `blocks_per_ag`, `ag_shift`, and `num_ags = ceil(num_blocks /
   (1 << ag_shift))`.
4. Choose a log size based on the volume size (e.g. 512 blocks for tiny volumes,
   2048 blocks normally, 4096 blocks for volumes over 1 GiB). Place the log
   immediately after the bitmap: `log_blocks` starts at block
   `bitmapBlocks + 1` with the chosen length. Set `log_start = log_end =
   ToBlock(log_blocks)`.
5. Write and clear the bitmap; mark the boot block, the bitmap, and the log as
   used; set `used_blocks` accordingly.
6. Create the root directory inode (mode `S_IFDIR | S_STR_INDEX` plus permission
   bits) with a directory B+tree in its data stream, and record it in
   `superblock.root_dir`. The tree must contain the `.` and `..` entries (both
   pointing at the root itself for the root directory), sorted in with any other
   entries ([Section 11](#11-directories)).
7. Optionally create the index directory (mode `S_INDEX_DIR | S_IFDIR |
   S_STR_INDEX`) and the standard indices (`name`, `size`, `last_modified`,
   `BEOS:APP_SIG`), recording the index directory in `superblock.indices`. Give
   each container the key-type bit matching its tree ([Section 6](#6-inodes)).
8. Erase the legacy boot sector (offset 0) and any foreign superblock at offset
   1024 to avoid misidentification.
9. Set `flags = 'CLEN'`, write the superblock at offset 512, and flush.

---

## Appendix: Constant Summary

```c
// Superblock
#define SUPER_BLOCK_MAGIC1      'BFS1'
#define SUPER_BLOCK_MAGIC2      0xdd121031
#define SUPER_BLOCK_MAGIC3      0x15b6830e
#define SUPER_BLOCK_FS_LENDIAN  'BIGE'
#define SUPER_BLOCK_DISK_CLEAN  'CLEN'
#define SUPER_BLOCK_DISK_DIRTY  'DIRT'
#define BFS_DISK_NAME_LENGTH    32

// block_run
#define MAX_BLOCK_RUN_LENGTH    65535

// Inode
#define INODE_MAGIC1                0x3bbe0ad9
#define INODE_FILE_NAME_LENGTH      256
#define SHORT_SYMLINK_NAME_LENGTH   144
#define INODE_TIME_SHIFT            16
#define INODE_TIME_MASK             0xfff0

// Data stream
#define NUM_DIRECT_BLOCKS           12
#define NUM_ARRAY_BLOCKS            4
#define DOUBLE_INDIRECT_ARRAY_SIZE  4096

// small_data
#define FILE_NAME_TYPE          'CSTR'
#define FILE_NAME_NAME          0x13
#define FILE_NAME_NAME_LENGTH   1
#define MAX_INDEX_KEY_LENGTH    255

// B+tree
#define BPLUSTREE_MAGIC             0x69f6c2e8
#define BPLUSTREE_NODE_SIZE         1024
#define BPLUSTREE_MAX_KEY_LENGTH    256
#define BPLUSTREE_MIN_KEY_LENGTH    1
#define BPLUSTREE_NULL              -1
#define BPLUSTREE_FREE              -2
#define BPLUSTREE_DUPLICATE_NODE     2
#define BPLUSTREE_DUPLICATE_FRAGMENT 3
#define NUM_FRAGMENT_VALUES          7
#define NUM_DUPLICATE_VALUES         125
```
