# BfsUtils — `makebfs`, `bfsextract`, `bfscheck`, `bfsresize`, `bfsmap`, `bfsdump`

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
- **`bfsmap`** renders a BFS image's block usage as a color-coded PNG
  (read-only), so you can see at a glance how the volume is laid out and how
  fragmented it is.
- **`bfsdump`** writes a BFS image's on-disk structures as JSON (read-only) —
  the superblock, individual inodes, data streams, directories, indices, and
  B+trees, either as their logical content or node by node.

Both work with the on-disk layout described in
[`docs/BFS_On-Disk_Format.md`](docs/BFS_On-Disk_Format.md). No Haiku driver
source is consulted. On non-Haiku POSIX platforms, attributes are ignored in
both directions; on Haiku they are preserved.

## Images and devices

Everywhere these tools take an image file they also take a **block device**, on
Linux and on Haiku. The differences worth knowing:

- The volume's size comes from the device, not from `stat`: `BLKGETSIZE64` on
  Linux, `B_GET_GEOMETRY` on Haiku. On Haiku a whole disk (`.../raw`) is a
  *character* device and each partition on it a *block* device; both work.
- `makebfs` fills the whole device unless `--size` says otherwise, and refuses a
  size that does not fit.
- A device holds the previous volume's bytes where a fresh image file would read
  as zeroes, so `makebfs` explicitly clears block 0 (old boot sector, and the
  ext2 superblock at offset 1024) and the log area. Free space keeps its old
  contents unless `--zero-free` is given.
- `bfsresize --max` resizes the volume to fill the device (or, for an image
  file, its current length).
- **Writing to a mounted device is refused.** Read-only tools (`bfscheck`,
  `bfsdump`, `bfsextract`, `bfsmap`) work on a mounted device, though what they
  read is a moving target if it is being written to.

To try this without real hardware, Haiku can register an image as a device with
`diskimage register <file>` (and `diskimage unregister <file>`); on Linux the
equivalent is `losetup --find --show <file>`.

## Building

The project uses [Meson](https://mesonbuild.com/):

```sh
meson setup build
ninja -C build
```

The result is `build/makebfs`. It builds on any POSIX-compatible system and on
Haiku. Attribute archiving is compiled in only under `__HAIKU__`.

`bfsmap` links against **libpng** and `bfsdump` against **RapidJSON** (both found
via `pkg-config`); the other tools have no external dependencies. Install the
development headers first — for example `libpng-dev` and `rapidjson-dev` on
Debian/Ubuntu, `libpng16_devel` and `rapidjson` on Haiku. RapidJSON is
header-only, so it costs nothing at run time. Both are **optional**: if either is
missing, that one tool is skipped and the rest still build.

On the remote Haiku machine (see `local/ENVIRONMENT.md`):

```sh
# from the project root, uploaded to ~/remote/BfsUtils
meson setup build && ninja -C build
```

## Usage

```
makebfs [options] <source-directory> <output-image|device>

  -b, --block-size N   block size: 1024, 2048, 4096, or 8192 (default 2048)
  -s, --size BYTES     total volume size; accepts K/M/G suffixes
                       (default: smallest image that fits the content,
                       or, on a device, the whole device)
      --zero-free      zero the device first, so no trace of the previous
                       contents survives in free space (slow)
  -n, --name NAME      volume name (default: source directory name)
      --endian ORDER   on-disk byte order: little (default) or big
      --no-index       do not generate the standard BFS indices
      --no-attributes  do not archive BFS attributes (Haiku only)
  -v, --verbose        print each scanned and written path
      --no-progress    do not show the progress indicator
  -h, --help
```

While it works, `makebfs` reports its phases on stderr — the scan, the plan, and
then the write, which counts every block it emits:

```
scanning      12483 entries
planning      12483 / 12483 nodes
indexing
laying out
writing        71%  1.02 GiB / 1.44 GiB   38.4 MiB/s
```

The indicator appears only when stderr is a terminal, so redirected output is
unaffected; `--no-progress` suppresses it there too. `-v` replaces the single
updating line with one line per path, and remains useful in a pipe.

Examples:

```sh
# Smallest possible image of ./payload
makebfs ./payload payload.bfs

# Fixed 64 MiB volume named "Data"
makebfs -s 64M -n Data ./payload data.bfs

# Big-endian volume (as used by BeOS/PowerPC); little-endian is the default
makebfs --endian big ./payload payload-be.bfs
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
double-indirect). Both little-endian and big-endian volumes are read (the byte
order is detected from the superblock); this applies equally to `bfscheck` and
`bfsmap`. A volume with a non-empty journal is rejected unless `--replay-log` is
given, in which case the log is replayed into an in-memory overlay before
extraction.

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
bfsresize [options] <image|device> <new-size>
bfsresize [options] --max <image|device>

  -m, --max            resize to fill the device, or the image file's
                       current length; use instead of <new-size>
      --journal PATH   keep the sidecar journal here
  -n, --dry-run        report the plan without modifying the volume
  -v, --verbose        verbose output
  -h, --help
```

`bfsresize` grows or shrinks a BFS volume in place (`<new-size>` in bytes, with
K/M/G suffixes, rounded down to a block multiple; `ag_shift` is held fixed). It
is **crash-safe**: every on-disk change is applied through a sidecar redo-journal
(`<image>.bfsresize-journal`) with fsync barriers, and the file is only truncated
after the superblock commit is durable — so an interruption always leaves a
mountable volume (the old size until the final commit), and the transaction is
replayed automatically on the next run. A dirty (uncleanly unmounted) volume is
refused.

A device cannot host a sidecar file, so when resizing one the journal goes to the
current directory under a name derived from the device path (or wherever
`--journal` says). The name is deterministic on purpose: recovery finds the
journal by recomputing it, so a temporary name would defeat the crash-safety.
Shrinking a device leaves its tail addressed by nothing rather than truncating,
and growing past its end is refused.

Supported today:

- **Grow** to any size, including across bitmap-block boundaries. Each new bitmap
  block is added one boundary at a time: the committed size is advanced first,
  then the single block the enlarged reserved prefix claims is relocated out of
  the way, then the bitmap block is appended and the empty log shifted forward —
  so an interruption always leaves a mountable volume at the last committed size.
- **Shrink** to any size: when the freed tail is unused it is simply truncated;
  when it holds live data, that data (stream runs and inodes) is relocated below
  the new size with all references fixed up. Across a bitmap-block boundary the
  empty log is relocated to keep the reserved region a contiguous prefix (as
  Haiku's allocator requires).

Limitations (refused with a clear message, image untouched):

- **`ag_shift` is held fixed**, so the reserved region (boot block + bitmap +
  log) must still fit inside the first allocation group. This caps a grow at
  roughly `group_size * block_size * 8` blocks; growing beyond it would require
  enlarging `ag_shift`, which re-encodes every `block_run` in the volume.
- Relocation needs contiguous free space in a single allocation group for each
  moved run; a badly fragmented volume can refuse a resize that the raw free-block
  count would otherwise allow (there is no compaction). Use `--dry-run` first.
- **Big-endian volumes are read-only** across the tools, so `bfsresize` refuses
  them (it writes structures back little-endian). `makebfs --endian big` can
  create them and `bfsextract`/`bfscheck`/`bfsmap` read them.

```sh
bfsresize data.bfs 64M          # grow to 64 MiB
bfsresize --dry-run data.bfs 32M
```

### `bfsmap`

```sh
bfsmap [options] <image> [output.png]
```

| Option | Meaning |
| --- | --- |
| `--width N` | Blocks per row in the grid (default: auto, ~2:1 landscape). |
| `--scale N` | Pixels per block cell (default: auto). |
| `--replay-log` | Map the post-replay state of an unclean volume. |

`bfsmap` walks the volume read-only and paints every block as one cell of a grid
(row-major by block number), colored by what it holds: free, reserved (boot +
superblock), bitmap, journal, inode, metadata (directory / attribute B+trees),
index, indirect (block_run arrays), file data, fragmented file data, attribute
data, or leaked (allocated but unreachable from the root). A legend below the
grid lists every type present with its block count and share of the volume. A
regular file is counted as *fragmented* only when its data lands in physically
separated pieces — runs that abut on disk count as one contiguous fragment.

If no output path is given, `<image>.png` is used. Rendering uses **libpng**.

```sh
bfsmap payload.bfs                    # writes payload.bfs.png
bfsmap --width 512 --scale 2 data.bfs data.png
```

### `bfsdump`

```sh
bfsdump [options] <image>
```

| Section (repeatable) | Meaning |
| --- | --- |
| `--superblock` | Volume header, geometry, log state, and derived counts (the default). |
| `--inode SPEC` | One inode's fields, decoded. |
| `--data-stream SPEC` | That inode's `data_stream`, tier by tier, with every run. |
| `--directory SPEC` | Directory entries. |
| `--index` | List the volume's indices with their key types. |
| `--index=NAME` | Dump one index as a sorted key → inodes map. |
| `--btree SPEC` | B+tree as a sorted key → value(s) map. |
| `--btree-nodes SPEC` | B+tree node by node, with links, fill, and stats. |

| Option | Meaning |
| --- | --- |
| `--depth N` | Recursion depth for `--directory` (default 1). |
| `--attributes` | Include attributes in `--inode` output. |
| `--max-entries N` | Cap entries per directory or tree. |
| `--max-nodes N` | Cap nodes per `--btree-nodes` dump. |
| `--max-data N` | Cap dumped blob bytes (default 256). |
| `--resolve-values` | Name the inodes a B+tree's values point at (type, and path where one reaches them). |
| `--replay-log` | Dump the post-replay state of an unclean volume. |
| `--compact` | Single-line JSON (default: indented). |
| `-o, --output FILE` | Write to `FILE` instead of stdout. |

A **SPEC** selects an inode and is one of: an absolute path inside the volume
(`/system/packages`), a block number (`2073` or `0x819`), a `block_run`
(`group:start:length`), or the names `root` and `indices`. Every option also
accepts the `--option=value` form; `--index` needs it, since a bare `--index`
means "list them all".

The two B+tree formats answer different questions. `--btree` gives the tree's
**content**: leaf entries in key order, keys decoded per the index's key type,
and duplicate containers expanded so every entry carries a plain `values` array.
`--btree-nodes` gives its **structure**: every node breadth-first with its
level, sibling links, key table, fill percentage, the free-node chain, and any
node-sized slot that nothing accounts for. The node view reads all nodes
directly, so it still works on a tree whose leaf chain is broken. The two agree
by construction — a map's `entry_count` equals the node view's
`stats.leaf_keys`, the remaining keys being internal separators.

Every leaf value in either view is an inode number, and `--resolve-values` turns
each one from a bare number into an object naming what it points at:

```json
{ "key": "application/x-Be.vnd.mimeset",
  "values": [ { "inode": 131260, "type": "file", "path": "/beos/bin/mimeset" } ] }
```

The path is found by walking each inode's `parent` up to the root and naming
every step in the directory above it, so it is the volume's own two records
agreeing rather than a lookup: an inode a parent does not list back gets `type`
but no `path`. So do the inodes nothing names — an index, which hangs off the
superblock rather than a directory, and an attribute, which its owner references
through the `attributes` run.

Output is JSON on stdout, so it composes with `jq` and friends. A structure that
cannot be read becomes an `"error"` or `"status"` member on its own section
rather than aborting the run, so a damaged volume still yields a complete,
parseable document.

```sh
bfsdump payload.bfs                             # superblock
bfsdump --index payload.bfs                     # what is indexed
bfsdump --index=name --max-entries 20 data.bfs  # the name index, first 20 keys
bfsdump --btree-nodes=/system data.bfs          # a directory tree's structure
bfsdump --index=name --resolve-values data.bfs  # the name index, as paths
bfsdump --inode /system/packages --attributes --data-stream /system/packages data.bfs
bfsdump --compact --directory root data.bfs | jq '.directories[0].entries[].name'
```

## What gets written

- **Superblock** at byte offset 512, in the selected byte order (see `--endian`),
  `flags = 'CLEN'`, empty log (`log_start == log_end`).
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
2. **Byte order.** Integers are serialized in the volume's byte order —
   little-endian by default, big-endian with `--endian big` — independent of the
   host's byte order. The `fs_byte_order` marker holds the integer `'BIGE'`,
   which serializes to the bytes `BIGE` on a big-endian volume and `EGIB` on a
   little-endian one.
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
| `BfsFormat.h` / `Endian.h` | On-disk constants, field offsets, endian-aware serialization |
| `BPlusTreeReader` | Read-only B+tree parsing: header, nodes, keys, duplicates |
| `JsonWriter` | Streaming JSON output over RapidJSON's SAX writer |
| `Dumper` | Render a volume's structures as JSON (`bfsdump`) |
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
