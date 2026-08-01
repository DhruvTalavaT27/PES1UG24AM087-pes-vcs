/*
 * commit.c — Commit creation and history traversal.
 *
 * Commits are stored as text, one field per line:
 *
 *     tree <64-hex-hash>
 *     parent <64-hex-hash>            <- omitted for the root commit
 *     author <identity> <unix-time>
 *     committer <identity> <unix-time>
 *
 *     <message>
 *
 * The parent pointer chains commits into history; the branch ref under
 * .strata/refs/heads/ always names the newest commit, and HEAD selects
 * which branch that is.
 */

#include "strata.h"
#include "commit.h"
#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/* Parse raw commit data into a Commit struct. */
int commit_parse(const void *data, size_t len, Commit *commit_out) {
    const char *p = (const char *)data;
    const char *end = p + len;
    char hex[HASH_HEX_SIZE + 1];

    /* tree <hex>\n */
    if (sscanf(p, "tree %64s\n", hex) != 1) return -1;
    if (hex_to_hash(hex, &commit_out->tree) != 0) return -1;
    p = strchr(p, '\n');
    if (!p) return -1;
    p++;

    /* parent <hex>\n — optional, root commits have none */
    if (strncmp(p, "parent ", 7) == 0) {
        if (sscanf(p, "parent %64s\n", hex) != 1) return -1;
        if (hex_to_hash(hex, &commit_out->parent) != 0) return -1;
        commit_out->has_parent = 1;
        p = strchr(p, '\n');
        if (!p) return -1;
        p++;
    } else {
        commit_out->has_parent = 0;
    }

    /* author <identity> <timestamp>\n */
    char author_buf[256];
    if (sscanf(p, "author %255[^\n]\n", author_buf) != 1) return -1;
    char *last_space = strrchr(author_buf, ' ');
    if (!last_space) return -1;
    commit_out->timestamp = (uint64_t)strtoull(last_space + 1, NULL, 10);
    *last_space = '\0';
    snprintf(commit_out->author, sizeof(commit_out->author), "%s", author_buf);

    p = strchr(p, '\n');          /* skip author line */
    if (!p) return -1;
    p++;
    p = strchr(p, '\n');          /* skip committer line */
    if (!p) return -1;
    p++;
    p = strchr(p, '\n');          /* skip the blank line */
    if (!p) return -1;
    p++;

    /* The message runs to the end of the object — copy it boundedly. */
    size_t remaining = (size_t)(end - p);
    size_t mlen = remaining < sizeof(commit_out->message) - 1
                      ? remaining
                      : sizeof(commit_out->message) - 1;
    memcpy(commit_out->message, p, mlen);
    commit_out->message[mlen] = '\0';
    return 0;
}

/* Serialize a Commit struct to the text format. */
int commit_serialize(const Commit *commit, void **data_out, size_t *len_out) {
    char tree_hex[HASH_HEX_SIZE + 1], parent_hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&commit->tree, tree_hex);

    char buf[8192];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "tree %s\n", tree_hex);
    if (commit->has_parent) {
        hash_to_hex(&commit->parent, parent_hex);
        n += snprintf(buf + n, sizeof(buf) - n, "parent %s\n", parent_hex);
    }
    n += snprintf(buf + n, sizeof(buf) - n,
                  "author %s %" PRIu64 "\n"
                  "committer %s %" PRIu64 "\n"
                  "\n"
                  "%s",
                  commit->author, commit->timestamp,
                  commit->author, commit->timestamp,
                  commit->message);

    *data_out = malloc((size_t)n + 1);
    if (!*data_out) return -1;
    memcpy(*data_out, buf, (size_t)n + 1);
    *len_out = (size_t)n;
    return 0;
}

/* Walk commit history from HEAD to the root commit. */
int commit_walk(commit_walk_fn callback, void *ctx) {
    ObjectID id;
    if (head_read(&id) != 0) return -1;

    while (1) {
        ObjectType type;
        void *raw;
        size_t raw_len;
        if (object_read(&id, &type, &raw, &raw_len) != 0) return -1;

        Commit c;
        int rc = commit_parse(raw, raw_len, &c);
        free(raw);
        if (rc != 0) return -1;

        callback(&id, &c, ctx);

        if (!c.has_parent) break;
        id = c.parent;
    }
    return 0;
}

/* Read the commit hash the current HEAD resolves to. */
int head_read(ObjectID *id_out) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;

    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0';

    /* HEAD is either "ref: refs/heads/<branch>" or a raw hash. */
    if (strncmp(line, "ref: ", 5) == 0) {
        char ref_path[528];
        snprintf(ref_path, sizeof(ref_path), "%s/%s", REPO_DIR, line + 5);
        f = fopen(ref_path, "r");
        if (!f) return -1;              /* branch exists but has no commits yet */
        if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
        fclose(f);
        line[strcspn(line, "\r\n")] = '\0';
    }
    return hex_to_hash(line, id_out);
}

/* Point the current branch at a new commit, atomically. */
int head_update(const ObjectID *new_commit) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;

    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0';

    char target_path[520];
    if (strncmp(line, "ref: ", 5) == 0)
        snprintf(target_path, sizeof(target_path), "%s/%s", REPO_DIR, line + 5);
    else
        snprintf(target_path, sizeof(target_path), "%s", HEAD_FILE);   /* detached */

    char tmp_path[536];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", target_path);
    int fd = mkstemp(tmp_path);
    if (fd < 0) return -1;

    FILE *out = fdopen(fd, "w");
    if (!out) { close(fd); unlink(tmp_path); return -1; }

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(new_commit, hex);
    fprintf(out, "%s\n", hex);

    int ok = (fflush(out) == 0) && (fsync(fileno(out)) == 0);
    if (fclose(out) != 0) ok = 0;
    if (!ok) { unlink(tmp_path); return -1; }

    if (rename(tmp_path, target_path) != 0) { unlink(tmp_path); return -1; }
    return 0;
}

/* Human-readable name of the current branch. */
void head_branch(char *out, size_t out_size) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) { snprintf(out, out_size, "main"); return; }

    char line[512];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        snprintf(out, out_size, "main");
        return;
    }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0';

    if (strncmp(line, "ref: refs/heads/", 16) == 0)
        snprintf(out, out_size, "%s", line + 16);
    else
        snprintf(out, out_size, "%s", line);    /* detached HEAD — the hash */
}

/* Create a commit from the staging area. */
int commit_create(const char *message, ObjectID *commit_id_out) {
    ObjectID tree_id;
    if (tree_from_index(&tree_id) != 0) {
        fprintf(stderr, "error: nothing staged to commit\n");
        return -1;
    }

    Commit c = { 0 };
    c.has_parent = (head_read(&c.parent) == 0);
    c.tree = tree_id;
    c.timestamp = (uint64_t)time(NULL);
    snprintf(c.author, sizeof(c.author), "%s", strata_author());
    snprintf(c.message, sizeof(c.message), "%s", message);

    void *data;
    size_t len;
    if (commit_serialize(&c, &data, &len) != 0) return -1;
    int rc = object_write(OBJ_COMMIT, data, len, commit_id_out);
    free(data);
    if (rc != 0) return -1;

    return head_update(commit_id_out);
}
