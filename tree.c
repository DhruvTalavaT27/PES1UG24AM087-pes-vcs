/*
 * tree.c — Tree object serialization and construction.
 *
 * A tree is a directory snapshot: a flat list of entries, each mapping a
 * name to a blob hash (file) or another tree hash (subdirectory). Entries
 * are serialized as "<mode-octal> <name>\0<32-byte-hash>", concatenated.
 *
 * Two properties matter in a content-addressed store:
 *   - serialization is deterministic — entries are sorted by name, so two
 *     directories with the same contents hash identically;
 *   - construction is recursive — tree_from_index turns the flat staging
 *     area into a nested tree, which is exactly what a commit snapshots.
 */

#include "strata.h"
#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Directory entries carry this mode; files keep the 0100644/0100755
 * modes recorded in the index. */
#define MODE_DIR 0040000

/* Parse raw tree bytes into a Tree struct. Returns 0 or -1. */
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        /* Mode: digits up to the first space. */
        const uint8_t *space = memchr(ptr, ' ', (size_t)(end - ptr));
        if (!space) return -1;

        char mode_str[16];
        size_t mode_len = (size_t)(space - ptr);
        if (mode_len == 0 || mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        mode_str[mode_len] = '\0';
        entry->mode = (uint32_t)strtoul(mode_str, NULL, 8);

        ptr = space + 1;

        /* Name: up to the NUL separator. */
        const uint8_t *nul = memchr(ptr, '\0', (size_t)(end - ptr));
        if (!nul) return -1;

        size_t name_len = (size_t)(nul - ptr);
        if (name_len == 0 || name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = nul + 1;

        /* Hash: the next 32 raw bytes. */
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

/* qsort comparator: entries are ordered by name. */
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

/* Serialize a Tree into binary form, sorting entries by name first.
 * Caller must free(*data_out). Returns 0 or -1. */
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = (size_t)tree->count * 296;   /* worst case per entry */
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted = *tree;
    qsort(sorted.entries, sorted.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted.count; i++) {
        const TreeEntry *entry = &sorted.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += (size_t)written + 1;   /* skip the NUL sprintf wrote */
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out  = offset;
    return 0;
}

/*
 * Flatten a tree into a list of file entries with full paths. This is how
 * `status` gets a comparable view of the last commit: read the commit,
 * take its root tree, flatten it, then diff it against the index.
 */
typedef struct {
    FlatEntry *entries;
    int count;
    int cap;
} FlattenBuf;

static int flatten_level(const ObjectID *tree_id, const char *prefix, FlattenBuf *buf) {
    ObjectType type;
    void *raw;
    size_t raw_len;
    if (object_read(tree_id, &type, &raw, &raw_len) != 0) return -1;
    if (type != OBJ_TREE) { free(raw); return -1; }

    Tree tree;
    if (tree_parse(raw, raw_len, &tree) != 0) { free(raw); return -1; }
    free(raw);

    for (int i = 0; i < tree.count; i++) {
        TreeEntry *e = &tree.entries[i];

        char path[512];
        if (prefix[0] == '\0')
            snprintf(path, sizeof(path), "%s", e->name);
        else
            snprintf(path, sizeof(path), "%s/%s", prefix, e->name);

        if (e->mode == MODE_DIR) {
            if (flatten_level(&e->hash, path, buf) != 0) return -1;
        } else {
            if (buf->count >= buf->cap) {
                buf->cap = buf->cap ? buf->cap * 2 : 64;
                FlatEntry *grown = realloc(buf->entries, (size_t)buf->cap * sizeof(FlatEntry));
                if (!grown) return -1;
                buf->entries = grown;
            }
            FlatEntry *fe = &buf->entries[buf->count++];
            snprintf(fe->path, sizeof(fe->path), "%s", path);
            fe->mode = e->mode;
            fe->hash = e->hash;
        }
    }
    return 0;
}

int tree_flatten(const ObjectID *root, FlatEntry **entries, int *count) {
    FlattenBuf buf = { NULL, 0, 0 };
    if (flatten_level(root, "", &buf) != 0) {
        free(buf.entries);
        return -1;
    }
    *entries = buf.entries;
    *count   = buf.count;
    return 0;
}

/*
 * Recursively build the tree for one directory level.
 *
 * entries[0..count) are the staged files whose paths all live under
 * `prefix` (e.g. "src/"). Each entry either belongs to a subdirectory —
 * in which case the matching run is handed to a recursive call — or is a
 * file at this level. The finished Tree is serialized and written to the
 * object store; *id_out receives the tree's hash.
 *
 * The index is kept sorted by path (see index_save), so entries sharing a
 * subdirectory prefix are always contiguous — the grouping below relies
 * on that.
 */
static int write_tree_level(IndexEntry *entries, int count, const char *prefix, ObjectID *id_out) {
    Tree tree = { 0 };

    int i = 0;
    while (i < count) {
        const char *path = entries[i].path;
        const char *rel  = path + strlen(prefix);    /* strip the prefix */
        const char *slash = strchr(rel, '/');

        if (slash) {
            /* Subdirectory: collect every sibling under the same dir. */
            size_t dlen = (size_t)(slash - rel);
            if (dlen == 0 || dlen >= sizeof(tree.entries[0].name)) return -1;
            char dirname[sizeof(tree.entries[0].name)];
            memcpy(dirname, rel, dlen);
            dirname[dlen] = '\0';

            char subprefix[512];
            snprintf(subprefix, sizeof(subprefix), "%s%s/", prefix, dirname);
            int start = i;
            while (i < count && strncmp(entries[i].path, subprefix, strlen(subprefix)) == 0)
                i++;

            ObjectID subtree_id;
            if (write_tree_level(entries + start, i - start, subprefix, &subtree_id) != 0)
                return -1;

            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = MODE_DIR;
            memcpy(e->name, dirname, dlen + 1);
            e->hash = subtree_id;
        } else {
            /* A file at this level. */
            size_t nlen = strlen(rel);
            if (nlen == 0 || nlen >= sizeof(tree.entries[0].name)) return -1;

            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = entries[i].mode;
            memcpy(e->name, rel, nlen + 1);
            e->hash = entries[i].hash;
            i++;
        }
    }

    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;
    int rc = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return rc;
}

/* Build the full snapshot tree from the staging area. */
int tree_from_index(ObjectID *id_out) {
    Index index = { 0 };
    if (index_load(&index) != 0) return -1;
    if (index.count == 0) return -1;
    return write_tree_level(index.entries, index.count, "", id_out);
}
