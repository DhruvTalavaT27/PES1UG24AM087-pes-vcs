/*
 * tree.h — Tree object interface.
 *
 * A tree object is a directory snapshot: a list of entries mapping a name
 * to a blob (file) or another tree (subdirectory), plus the file mode.
 */

#ifndef TREE_H
#define TREE_H

#include "strata.h"

#define MAX_TREE_ENTRIES 1024

typedef struct {
    uint32_t mode;      /* 0100644 regular, 0100755 executable, 0040000 directory */
    ObjectID hash;      /* SHA-256 of the blob or subtree */
    char name[256];     /* entry name — never contains '/' */
} TreeEntry;

typedef struct {
    TreeEntry entries[MAX_TREE_ENTRIES];
    int count;
} Tree;

/* Parse raw tree bytes (as read from the object store) into a Tree.
 * Returns 0 on success, -1 on malformed data. */
int tree_parse(const void *data, size_t len, Tree *tree_out);

/*
 * Serialize a Tree into raw bytes for object_write(OBJ_TREE, ...).
 * Entries are sorted by name first, so two directories with the same
 * contents always hash identically. Caller must free(*data_out).
 * Returns 0 on success, -1 on error.
 */
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out);

/* A flattened view of a tree: one entry per file, with full paths. */
typedef struct {
    char path[512];     /* e.g. "src/main.c" */
    uint32_t mode;
    ObjectID hash;
} FlatEntry;

/*
 * Recursively flatten the tree rooted at *root into a sorted list of
 * file entries. This is how `status` compares the index against HEAD.
 * Caller must free(*entries). Returns 0 on success, -1 on error.
 */
int tree_flatten(const ObjectID *root, FlatEntry **entries, int *count);

/*
 * Build the full tree hierarchy from the current staging area, write
 * every tree object to the store, and return the root hash in *id_out.
 * This is what a commit snapshots. Returns 0 on success, -1 on error.
 */
int tree_from_index(ObjectID *id_out);

#endif /* TREE_H */
