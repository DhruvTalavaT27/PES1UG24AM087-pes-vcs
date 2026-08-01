/*
 * index.c — Staging area implementation.
 *
 * The index lives at .strata/index as plain text. Because it is the one
 * file rewritten on almost every command, it is written atomically:
 * contents go to a temp file which is fsync'd and renamed over the old
 * index, so a crash mid-write can never truncate it.
 */

#include "strata.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* Find a staged entry by path. */
IndexEntry *index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

/* Unstage a file. */
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            size_t remaining = (size_t)(index->count - i - 1);
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not staged\n", path);
    return -1;
}

/* Load the index from disk. A missing file is an empty index, not an error. */
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;

    char hex[HASH_HEX_SIZE + 1];
    unsigned long long mtime;
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        if (fscanf(f, "%o %64s %llu %u %511s",
                   &e->mode, hex, &mtime, &e->size, e->path) != 5)
            break;
        e->mtime_sec = (uint64_t)mtime;
        if (hex_to_hash(hex, &e->hash) != 0) break;
        index->count++;
    }
    fclose(f);
    return 0;
}

/* qsort comparator: keep entries ordered by path. */
static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

/* Persist the index atomically. */
int index_save(const Index *index) {
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    *sorted = *index;
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), compare_index_entries);

    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", INDEX_FILE);
    int fd = mkstemp(tmp);
    if (fd < 0) { free(sorted); return -1; }

    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); free(sorted); return -1; }

    for (int i = 0; i < sorted->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted->entries[i].hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                sorted->entries[i].mode, hex,
                (unsigned long long)sorted->entries[i].mtime_sec,
                sorted->entries[i].size,
                sorted->entries[i].path);
    }

    int ok = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    if (fclose(f) != 0) ok = 0;
    free(sorted);
    if (!ok) { unlink(tmp); return -1; }

    if (rename(tmp, INDEX_FILE) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Stage a file: hash its contents into a blob and record it. */
int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "error: cannot stage '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: '%s' is not a regular file\n", path);
        return -1;
    }

    uint8_t *buf = NULL;
    if (st.st_size > 0) {
        buf = malloc((size_t)st.st_size);
        if (!buf) return -1;
        FILE *f = fopen(path, "rb");
        if (!f) { free(buf); return -1; }
        if (fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
            free(buf); fclose(f); return -1;
        }
        fclose(f);
    }

    ObjectID id;
    int rc = object_write(OBJ_BLOB, buf, (size_t)st.st_size, &id);
    free(buf);
    if (rc != 0) return -1;

    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
    }
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->mode      = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size      = (uint32_t)st.st_size;
    e->hash      = id;

    return index_save(index);
}
