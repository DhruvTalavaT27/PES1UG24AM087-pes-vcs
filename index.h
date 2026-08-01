/*
 * index.h — Staging area (index) interface.
 *
 * The index is the set of files staged for the next commit. It is stored
 * as a plain text file, one entry per line:
 *
 *     <mode-octal> <64-hex-hash> <mtime-seconds> <size> <path>
 *
 * A text format is a deliberate choice: it is trivially debuggable, and
 * for a single-user tool the parse cost is irrelevant.
 */

#ifndef INDEX_H
#define INDEX_H

#include "strata.h"

#define MAX_INDEX_ENTRIES 10000

typedef struct {
    uint32_t mode;        /* 0100644 or 0100755 */
    ObjectID hash;        /* blob hash of the staged content */
    uint64_t mtime_sec;   /* file mtime when staged, for fast change detection */
    uint32_t size;        /* file size when staged */
    char path[512];       /* repo-relative path, e.g. "src/main.c" */
} IndexEntry;

typedef struct {
    IndexEntry entries[MAX_INDEX_ENTRIES];
    int count;
} Index;

/* Load the index. A missing file just means nothing is staged (not an error). */
int index_load(Index *index);

/* Persist the index atomically (temp file + fsync + rename). */
int index_save(const Index *index);

/* Stage a file: store its contents as a blob and record an entry. */
int index_add(Index *index, const char *path);

/* Unstage a file. Returns 0, or -1 if it wasn't staged. */
int index_remove(Index *index, const char *path);

/* Find a staged entry by path, or NULL. */
IndexEntry *index_find(Index *index, const char *path);

#endif /* INDEX_H */
