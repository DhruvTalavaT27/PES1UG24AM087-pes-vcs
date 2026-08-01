# strata

A minimal version control system written in C, modeled on the internals of git.

`strata` snapshots your project into a content-addressable object store,
supports staging, commits, and history — and fits in about 1,300 lines of C.
Its only external dependency is OpenSSL, for SHA-256.

Why "strata"? Each commit is a layer of history — like sedimentary rock.

## Highlights

- **Content-addressed storage.** Every file, directory, and commit is named
  by the SHA-256 of its serialized form. Identical content is stored exactly
  once, and any corruption is detected the moment an object is read back.
- **Atomic everywhere.** Every on-disk mutation — objects, the index, branch
  refs — goes through a temp file, `fsync`, and `rename`. A crash mid-write
  can never leave a half-written file at its final path.
- **A real `status`.** `strata status` diffs the index against the HEAD tree
  and walks the working directory recursively, so it reports exactly what
  would be committed, what is modified but unstaged, and what is untracked.
- **Plain-text index.** The staging area is a readable text file — no binary
  format to reverse-engineer, trivially debuggable.
- **Zero configuration.** Commit identity comes from `$STRATA_AUTHOR`, or
  falls back to `user <user@localhost>` derived from `$USER`, the same
  fallback git uses.

## Quick start

```console
$ strata init
Initialized empty strata repository in /tmp/demo/.strata/

$ echo "version 1" > file.txt
$ echo "hello world" > hello.txt
$ strata add file.txt hello.txt

$ strata status
On branch main

Changes to be committed:
  new file:   file.txt
  new file:   hello.txt

$ strata commit -m "Initial commit"
[main a189f6067ce5] Initial commit

$ strata log
commit a189f6067ce5 (HEAD -> main)
Author: you <you@localhost>
Date:   Sat Aug  1 06:21:50 2026 +0000

    Initial commit

$ echo "version 2" >> file.txt
$ strata status
On branch main

Changes not staged for commit:
  modified:   file.txt

$ strata add file.txt
$ strata commit -m "Bump to version 2"
[main 338dd3f762c9] Bump to version 2
```

(Output captured from a real session; hashes and dates vary with content and time.)

## How it works

### The object store

Everything — file contents, directory listings, commits — is an *object*.
An object is `"<type> <size>\0"` followed by raw bytes, and it lives at a
path derived from its own hash:

```
.strata/objects/
├── 2f/
│   └── 8a3b5c7d9e...        # first two hex chars of the hash = shard dir
├── a1/
│   ├── 9c4e6f8a0b...
│   └── b2d4f6a8c0...
└── ff/
    └── 1234567890...
```

Hashing the *full* object (header + data) is what makes the store
content-addressable:

- **Deduplication** — two identical files hash identically and share one blob.
- **Integrity** — every `object_read` re-hashes the file and compares it to
  the hash it was looked up by, so corruption fails loudly instead of
  surfacing as garbage later.
- **Sharding** — objects are split across `XX/` subdirectories so no single
  directory grows unbounded.

### Trees

A *tree* is a directory snapshot: entries mapping a name to a blob (file) or
another tree (subdirectory), each with a mode. Entries are sorted by name
before serialization, so two directories with the same contents always hash
identically — unchanged directories are reused across commits for free.

```
100644 blob a1b2c3d4... README.md
100755 blob e5f6a7b8... build.sh
040000 tree 9c0d1e2f... src/
```

`tree_from_index` turns the flat staging area into this nested hierarchy —
`src/main.c` in the index becomes a `src` subtree containing `main.c` — and
`tree_flatten` does the reverse, which is how `status` compares the index
against the last commit.

### Commits and refs

A commit points at a root tree (the snapshot), its parent, the author, and a
message. The parent pointer chains commits into history; a branch ref under
`.strata/refs/heads/` always names the newest commit; `HEAD` selects the
branch:

```
HEAD ──► main ──► C3 ──► C2 ──► C1 (root)
                    │      │      │
                  Tree3  Tree2  Tree1
```

Commits share trees and blobs with their ancestors — a one-line change
produces exactly one new blob, one new tree, and one new commit.

### The index

The index is the staging area: the set of files that will go into the next
commit. It's a plain text file, one entry per line:

```
100644 a1b2c3d4e5f6a7b8c9d0... 1699900000 42 README.md
100644 f7e8d9c0b1a2b3c4d5e6... 1699900100 128 src/main.c
```

Because it is rewritten on almost every command, it gets the same
temp-file + `fsync` + `rename` treatment as everything else.

## Building and testing

Requires a C compiler, `make`, and OpenSSL headers (on Debian/Ubuntu:
`sudo apt install build-essential libssl-dev`).

```console
$ make all                    # builds strata + the unit test binaries
$ make test-unit              # object store and tree unit tests
$ make test-integration       # end-to-end script (init → commit → log)
$ make test                   # everything
```

A GitHub Actions workflow builds and tests on every push — including a
second job that rebuilds everything under AddressSanitizer and
UndefinedBehaviorSanitizer and re-runs the full suite.

## Repository layout

| File              | Responsibility                                    |
| ----------------- | ------------------------------------------------- |
| `strata.h`        | Core types, repository layout, object API         |
| `object.c`        | Content-addressable object store                  |
| `tree.c`          | Tree serialization, snapshot construction, flattening |
| `index.c`         | Staging area (text format, atomic writes)         |
| `commit.c`        | Commit objects, history walking, refs             |
| `status.c`        | Working tree status (index vs HEAD vs worktree)   |
| `strata.c`        | CLI dispatch                                      |
| `test_objects.c`  | Unit tests for the object store                   |
| `test_tree.c`     | Unit tests for tree serialization/flattening      |
| `test_sequence.sh`| End-to-end integration test                       |

## Design decisions

- **Text index format.** A binary index would be faster to parse, but the
  plain-text format is readable, diffable, and trivially debuggable — and for
  a single-user tool the parse cost is irrelevant.
- **Metadata-based change detection.** `status` compares mtime + size against
  the index instead of re-hashing file contents on every run. It's fast, at
  the cost of occasionally being conservative (a change within the same
  second, to a file of the same size, can be missed until the next `add`).
- **`fsync` on directories.** Renaming a temp file into place is atomic, but
  persisting the rename itself requires syncing the containing directory —
  easy to forget, and it's the difference between "atomic in theory" and
  "atomic after a power loss".
- **Colors only on a TTY.** `status` colorizes output only when stdout is a
  terminal, so piping to `grep` or a file stays clean.
- **Self-describing objects.** Type and size live in the object header, so
  the store needs no external bookkeeping — a `find` on `.strata/objects`
  tells you everything that exists.

## Limitations & roadmap

Honest about what this is: a teaching-grade VCS, not a git replacement.

- **No branching or checkout.** `HEAD` always points at `main`; the ref
  machinery is in place, but switching branches and materializing a tree
  into the working directory is not.
- **No diffs.** `status` tells you *what* changed, not *how*.
- **No merges, rebases, or remotes.** History is a single chain.
- **Index paths can't contain spaces** — the text format parses whitespace-
  separated fields.
- **Untracked scanning** skips build artifacts heuristically (`.o`, `.exe`,
  the binaries themselves) rather than reading a `.gitignore`.

