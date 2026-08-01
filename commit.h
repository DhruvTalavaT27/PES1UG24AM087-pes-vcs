/*
 * commit.h — Commit object interface.
 *
 * A commit ties together a tree snapshot (the state of the staging area),
 * a pointer to its parent, the author identity, and a message.
 */

#ifndef COMMIT_H
#define COMMIT_H

#include "strata.h"

typedef struct {
    ObjectID tree;          /* root tree of the snapshot */
    ObjectID parent;        /* parent commit, if any */
    int has_parent;         /* 0 for the initial commit */
    char author[256];       /* identity from strata_author() */
    uint64_t timestamp;     /* Unix time of the commit */
    char message[4096];     /* commit message */
} Commit;

/* Create a commit from the staging area and advance the current branch. */
int commit_create(const char *message, ObjectID *commit_id_out);

/* Parse raw commit bytes into a Commit struct. Returns 0 or -1. */
int commit_parse(const void *data, size_t len, Commit *commit_out);

/* Serialize a Commit into raw bytes. Caller must free(*data_out). */
int commit_serialize(const Commit *commit, void **data_out, size_t *len_out);

/* Walk history from HEAD to the root, calling callback for each commit. */
typedef void (*commit_walk_fn)(const ObjectID *id, const Commit *commit, void *ctx);
int commit_walk(commit_walk_fn callback, void *ctx);

/* ── HEAD helpers ──────────────────────────────────────────────────────────── */

/* Hash that HEAD resolves to; -1 when the repository has no commits yet. */
int head_read(ObjectID *id_out);

/* Point HEAD (or the branch it references) at a new commit, atomically. */
int head_update(const ObjectID *new_commit);

/* Human name of the current branch (or the raw hash when detached). */
void head_branch(char *out, size_t out_size);

#endif /* COMMIT_H */
