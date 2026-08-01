/*
 * strata.h — Core types and constants shared by every module.
 *
 * strata is a small, git-inspired version control system written in C.
 * This header defines the repository layout, the object model, and the
 * hashing helpers. Nothing here depends on any other module.
 */

#ifndef STRATA_H
#define STRATA_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* SHA-256 produces 32 bytes, or 64 hex characters. */
#define HASH_SIZE     32
#define HASH_HEX_SIZE 64

/* Version reported by `strata --version`. */
#define STRATA_VERSION "0.1.0"

/*
 * Repository layout. Everything lives under .strata/, mirroring the
 * structure git uses: objects are content-addressed, refs point at
 * commits, HEAD selects the current branch, and the index records the
 * staging area.
 */
#define REPO_DIR     ".strata"
#define OBJECTS_DIR  ".strata/objects"
#define REFS_DIR     ".strata/refs/heads"
#define INDEX_FILE   ".strata/index"
#define HEAD_FILE    ".strata/HEAD"

/* The three kinds of objects in the store. */
typedef enum {
    OBJ_BLOB,    /* raw file contents */
    OBJ_TREE,    /* a directory listing */
    OBJ_COMMIT   /* a snapshot with metadata */
} ObjectType;

/* A content address: SHA-256 over the object's serialized form. */
typedef struct {
    uint8_t hash[HASH_SIZE];
} ObjectID;

/* ── Hashing helpers ──────────────────────────────────────────────────────── */

/* Format a hash as 64 lowercase hex digits (+ NUL). hex_out needs 65 bytes. */
void hash_to_hex(const ObjectID *id, char *hex_out);

/* Parse 64 hex digits into a binary hash. Returns 0 on success, -1 on error. */
int hex_to_hash(const char *hex, ObjectID *id_out);

/* ── Object store (object.c) ──────────────────────────────────────────────── */

/*
 * Write an object to the store. The full serialized form — header plus
 * data — is hashed, so an object's name changes if any byte changes.
 * Identical content deduplicates to a single object. On success *id_out
 * holds the hash. Returns 0 or -1.
 */
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

/*
 * Read an object back, verifying its integrity against *id on the way.
 * *data_out is freshly allocated (caller must free it). Returns 0, or -1
 * if the object is missing or corrupt.
 */
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

/* True if an object with this id already exists on disk. */
int object_exists(const ObjectID *id);

/* Filesystem path for an object, e.g. .strata/objects/2f/8a3b... */
void object_path(const ObjectID *id, char *path_out, size_t path_size);

/* ── Author identity ──────────────────────────────────────────────────────── */

/*
 * Resolve the identity stamped on commits.
 *
 * $STRATA_AUTHOR wins when set. Otherwise we fall back to
 * "user <user@localhost>" derived from $USER — the same fallback git
 * uses. The returned string is valid until the next call.
 */
const char *strata_author(void);

#endif /* STRATA_H */
