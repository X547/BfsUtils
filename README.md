# BfsUtils — `makebfs`, `bfsextract`, `bfscheck`

**BfsUtils** is a set of filesystem-aware tools for **BFS** (the Be File System):

- **`makebfs`** generates a fresh, mountable BFS image from a directory tree —
  every file, directory, and symlink under a source directory, plus the standard
  BFS indices and, on Haiku, each node's attributes.
- **`bfsextract`** does the inverse: it reads a BFS image and reconstructs its
  tree into a destination directory, restoring contents, symlinks, permissions,
  timestamps, and, on Haiku, attributes.
- **`bfscheck`** is a read-only integrity checker (a check-only `checkfs`): it
  validates the superblock, bitmap, inodes, data streams, and B+trees, reporting
  all findings without stopping on the first.
- **`bfsresize`** grows or shrinks a BFS image in place, crash-safely via a
  sidecar redo-journal.
- **`bfscheck`** verifies the integrity of a BFS image (read-only), reporting
  every problem it finds without stopping and without modifying the image.

Both work with the on-disk layout described in
[`docs/BFS_On-Disk_Format.md`](local/BFS_On-Disk_Format.md). No Haiku driver
source is consulted. On non-Haiku POSIX platforms, attributes are ignored in
both directions; on Haiku they are preserved.

## Building

The project uses [Meson](https://mesonbuild.com/):

```sh
meson setup build
ninja -C build
```

The result is `build/makebfs`. It builds on any POSIX-compatible system and on
Haiku. Attribute archiving is compiled in only under `__HAIKU__`.

On the remote Haiku machine (see `local/ENVIRONMENT.md`):

```sh
# from the project root, uploaded to ~/remote/BfsUtils
meson setup build && ninja -C build
```

## Usage

```
makebfs [options] <source-directory> <output-image>

  -b, --block-size N   block size: 1024, 2048, 4096, or 8192 (default 2048)
  -s, --size BYTES     total image size; accepts K/M/G suffixes
                       (default: smallest image that fits the content)
  -n, --name NAME      volume name (default: source directory name)
      --no-index       do not generate the standard BFS indices
      --no-attributes  do not archive BFS attributes (Haiku only)
  -h, --help
```

Examples:

```sh
# Smallest possible image of ./payload
makebfs ./payload payload.bfs

# Fixed 64 MiB volume named "Data"
makebfs -s 64M -n Data ./payload data.bfs
```

By default the image is the smallest volume the content fits into, so the
resulting volume is essentially full (`used_blocks == num_blocks`). Use
`--size` to leave free space for a writable volume.

### `bfsextract`

```
bfsextract [options] <image> <output-directory>

      --no-attributes  do not restore BFS attributes (Haiku only)
      --no-owner       do not restore uid/gid ownership
      --replay-log     replay the journal if the volume is not clean
  -v, --verbose        print each extracted path
  -h, --help
```

`bfsextract` walks the volume's directory tree (ignoring the internal indices)
and writes each file, directory, and symlink to `output-directory`, then
restores permissions, modification time, best-effort ownership, and — on Haiku —
attributes (from both the inline `small_data` region and attribute directories).
The reader resolves all three data-stream tiers (direct, indirect,
double-indirect). A volume with a non-empty journal is rejected unless
`--replay-log` is given, in which case the log is replayed into an in-memory
overlay before extraction.

```sh
# Round-trips with makebfs
makebfs ./payload payload.bfs && bfsextract payload.bfs ./restored
```

### `bfscheck`

```
bfscheck [options] <image>

      --scan-orphans   scan all blocks for in-use inodes unreachable from the
                       root (classifies leaked allocated blocks)
      --replay-log     check the post-replay state of an unclean volume
      --strict         treat warnings as failures too
  -v, --verbose        verbose output
  -h, --help
```

`bfscheck` walks the volume and reports integrity problems **without stopping on
the first error** and **without modifying the image**. It checks: superblock
magic/geometry coherence; the block bitmap against actual usage (referenced-but-
free, allocated-but-unreferenced, `used_blocks` count); `block_run` validity and
out-of-bounds/cross-linked blocks; inode magic, `inode_num`/parent consistency,
and type-bit coherence; data-stream coverage across all three tiers; B+tree
structure, key ordering, separator bounds, and duplicate structures (directory,
index, and attribute trees); and directory `.`/`..` presence and correctness.
Findings are printed inline (capped per category) with a final summary. Index
**structure** is checked, but index-vs-data content coherence is out of scope.

Exit status: `0` clean, `1` problems found, `2` could not open/parse.

```sh
bfscheck payload.bfs
```

### `bfsresize`

```
bfsresize [options] <image> <new-size>

  -n, --dry-run   report the plan without modifying the image
  -v, --verbose   verbose output
  -h, --help
```

`bfsresize` grows or shrinks a BFS image in place (`<new-size>` in bytes, with
K/M/G suffixes, rounded down to a block multiple; `ag_shift` is held fixed). It
is **crash-safe**: every on-disk change is applied through a sidecar redo-journal
(`<image>.bfsresize-journal`) with fsync barriers, and the file is only truncated
after the superblock commit is durable — so an interruption always leaves a
mountable volume (the old size until the final commit), and the transaction is
replayed automatically on the next run. A dirty (uncleanly unmounted) volume is
refused.

Supported today:

- **Grow** within a bitmap-block band (no new bitmap blocks needed).
- **Shrink** when the freed tail is unused — including across a bitmap-block
  boundary, where the empty log is relocated to keep the reserved region a
  contiguous prefix (as Haiku's allocator requires).

Not yet implemented (refused with a clear message, image untouched): shrinking
past used blocks (relocating live data — the "moved blocks" path) and growing
across a bitmap-block boundary.

```sh
bfsresize data.bfs 64M          # grow to 64 MiB
bfsresize --dry-run data.bfs 32M
```

## What gets written

- **Superblock** at byte offset 512, little-endian, `flags = 'CLEN'`, empty log
  (`log_start == log_end`).
- **Block bitmap** starting at block 1; used blocks form the contiguous prefix
  `[0, used_blocks)`.
- **Log area** reserved immediately after the bitmap (empty on a clean volume).
- **Inodes** — one block each — for every node, plus attribute directories and
  attribute inodes (Haiku), the index directory, and each index.
- **Directory B+trees** mapping entry name → inode number.
- **Indices** under the index directory: `name` (string), `size` (int64),
  `last_modified` (int64), and `BEOS:APP_SIG` (string, when present). Each node
  is inserted into `name`/`size`/`last_modified`; duplicate keys use dedicated
  duplicate nodes.
- **small_data** — the mandatory name record plus attributes that fit inline;
  larger attributes are promoted to attribute inodes.

## Verifying on Haiku

The generated image is meant to be validated with stock Haiku tools (not driver
source):

```sh
checkfs payload.bfs                 # consistency check
mkdir /tmp/mnt && mount -t bfs -o loop payload.bfs /tmp/mnt   # or: mountvolume
ls -l /tmp/mnt
catattr -r SOME:ATTR /tmp/mnt/somefile
query -v /tmp/mnt 'name=="*.txt"'   # exercises the name index
```

## Design notes / assumptions

These follow from the spec; where the spec leaves latitude, the chosen policy is
noted. They are the first things to check if a volume fails to mount.

1. **Structure layout (packed).** All on-disk structures are packed with no
   compiler padding, per section 1 of the format doc — confirmed by mounting on
   Haiku and byte-diffing a reference volume. `bfs_inode.create_time` is at
   offset 28 and `sizeof(bplustree_node)` is 28 (`btreenode::kSize`), so B+tree
   key data starts at offset 28 and the key-length table at
   `KeyAlign(28 + all_key_length)`. All offsets are centralized in
   `src/BfsFormat.h`.
6. **`.` and `..`.** Real BFS stores `.` (self) and `..` (parent) as entries of
   every *regular* directory tree; the format doc's claim that they are omitted
   is incorrect. `makebfs` writes them, **sorted in with the real names** — `.`
   (byte `0x2e`) is not always first, since names beginning `!`, `#`, `+`, `,`,
   `-` sort before it, and feeding unsorted keys to the tree builder produces
   mis-ordered nodes that crash Haiku's lookup. Attribute directories and the
   index directory do not get `.`/`..`.
7. **`S_STR_INDEX` on string-keyed containers.** Regular directories, attribute
   directories, and the index directory all set `S_STR_INDEX` in their mode
   (the index directory is `S_INDEX_DIR | S_STR_INDEX | S_IFDIR`). Haiku derives
   a container's B+tree key type from its index-type mode bit; an index
   directory lacking one is rejected ("inode tree corrupt" → "volume doesn't
   have indices"). Individual indices carry their own type bit.
2. **Little-endian volumes.** All integers are written little-endian regardless
   of host byte order; the `fs_byte_order` marker stores the literal bytes
   `BIGE`.
3. **Allocation-group geometry.** The smallest `ag_shift` valid for the block
   size is chosen, then grown to keep the group count small and to keep the
   reserved region within the first group — any geometry satisfying the
   superblock invariants is valid.
4. **Log size.** 512 / 2048 / 4096 blocks by volume size, per the spec's
   creation procedure. The log is left empty.
5. **Timestamps.** Encoded per section 15; sub-second bits are best-effort since
   index correctness only requires a consistent encoding.

## Current limitations

- **Duplicate index keys use full duplicate nodes**, not fragment packing — a
  correct but not maximally compact choice.
- POSIX builds archive data and the standard indices only; BFS attributes are
  read on Haiku.

## Source layout

| File | Responsibility |
|------|----------------|
| `BfsFormat.h` / `Endian.h` | On-disk constants, field offsets, LE serialization |
| `Geometry.h` | block ⇄ `block_run` conversion, volume geometry |
| `SourceScanner` | Walk the source tree into an in-memory model |
| `Attributes` | Read BFS attributes (`__HAIKU__` only) |
| `SmallData` | Build the inode `small_data` region |
| `BPlusTreeBuilder` | Bulk-load directory / index B+trees |
| `DataStream` | Fill a `data_stream` from allocated runs |
| `BlockAllocator` | Offline bump allocation + bitmap emission |
| `ImageFile` | Sparse, block-addressable output file |
| `BfsBuilder` | Plan → measure → geometry → place → serialize |
| `main.cpp` | Command-line interface |
