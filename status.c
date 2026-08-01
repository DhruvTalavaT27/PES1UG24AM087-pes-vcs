/*
 * status.c — Working tree status.
 *
 * The closest thing strata does to a diff. Three comparisons are made:
 *
 *   1. index vs HEAD tree    -> what the next commit would change
 *   2. working tree vs index -> edits not yet staged (fast path: mtime + size)
 *   3. working tree vs index -> untracked files, found by walking the tree
 *
 * Output is colorized only when stdout is a terminal, so piping stays clean.
 */

#include "strata.h"
#include "index.h"
#include "commit.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* ANSI colors — empty strings unless stdout is a TTY. */
static const char *C_GREEN, *C_RED, *C_CYAN, *C_BOLD, *C_RESET;

static void init_colors(void) {
    if (isatty(fileno(stdout))) {
        C_GREEN = "\033[32m";
        C_RED   = "\033[31m";
        C_CYAN  = "\033[36m";
        C_BOLD  = "\033[1m";
        C_RESET = "\033[0m";
    } else {
        C_GREEN = C_RED = C_CYAN = C_BOLD = C_RESET = "";
    }
}

/* Tracks how many sections have been opened, for blank-line spacing. */
static int sections_shown;

/* Print a section header the first time something lands in it. */
static void section_header(int *printed, const char *title) {
    if (!*printed) {
        if (sections_shown > 0) printf("\n");
        printf("%s:\n", title);
        sections_shown++;
        *printed = 1;
    }
}

/* Linear lookup in a flattened HEAD tree. */
static FlatEntry *find_flat(FlatEntry *entries, int count, const char *path) {
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i].path, path) == 0)
            return &entries[i];
    return NULL;
}

/* Flatten the tree that HEAD points at; empty when there are no commits. */
static int load_head_flat(FlatEntry **entries, int *count) {
    ObjectID head;
    if (head_read(&head) != 0) {
        *entries = NULL;
        *count = 0;
        return 0;
    }

    ObjectType type;
    void *raw;
    size_t raw_len;
    if (object_read(&head, &type, &raw, &raw_len) != 0) return -1;

    Commit c;
    int rc = commit_parse(raw, raw_len, &c);
    free(raw);
    if (rc != 0) return -1;

    return tree_flatten(&c.tree, entries, count);
}

/* Build products and editor droppings that should never read as untracked. */
static int is_ignored(const char *name) {
    size_t len = strlen(name);
    if (len > 2 && strcmp(name + len - 2, ".o") == 0) return 1;
    if (len > 4 && strcmp(name + len - 4, ".exe") == 0) return 1;
    static const char *binaries[] = { "strata", "test_objects", "test_tree", NULL };
    for (int i = 0; binaries[i]; i++)
        if (strcmp(name, binaries[i]) == 0) return 1;
    return 0;
}

/* Recursively find working-tree files that are not staged. */
static void scan_untracked(const char *dirpath, Index *index, int *printed, int *count) {
    DIR *dir = opendir(dirpath[0] ? dirpath : ".");
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strcmp(ent->d_name, REPO_DIR) == 0) continue;

        char path[1024];
        if (dirpath[0])
            snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name);
        else
            snprintf(path, sizeof(path), "%s", ent->d_name);

        struct stat st;
        if (lstat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_untracked(path, index, printed, count);
        } else if (S_ISREG(st.st_mode) && !is_ignored(ent->d_name)) {
            if (!index_find(index, path)) {
                section_header(printed, "Untracked files");
                printf("%s  %s%s\n", C_CYAN, path, C_RESET);
                (*count)++;
            }
        }
    }
    closedir(dir);
}

int status_run(void) {
    init_colors();
    sections_shown = 0;

    Index index;
    if (index_load(&index) != 0) {
        fprintf(stderr, "error: failed to load the index\n");
        return -1;
    }

    FlatEntry *head = NULL;
    int head_count = 0;
    if (load_head_flat(&head, &head_count) != 0) {
        fprintf(stderr, "error: failed to read the HEAD tree\n");
        return -1;
    }

    char branch[64];
    head_branch(branch, sizeof(branch));
    printf("%sOn branch %s%s\n\n", C_BOLD, branch, C_RESET);

    int staged = 0, unstaged = 0, untracked = 0;

    /* 1. Staged — index entries that differ from HEAD. */
    int shown = 0;
    for (int i = 0; i < index.count; i++) {
        IndexEntry *ie = &index.entries[i];
        FlatEntry *he = find_flat(head, head_count, ie->path);
        if (!he) {
            section_header(&shown, "Changes to be committed");
            printf("%s  new file:   %s%s\n", C_GREEN, ie->path, C_RESET);
            staged++;
        } else if (he->mode != ie->mode ||
                   memcmp(he->hash.hash, ie->hash.hash, HASH_SIZE) != 0) {
            section_header(&shown, "Changes to be committed");
            printf("%s  modified:   %s%s\n", C_GREEN, ie->path, C_RESET);
            staged++;
        }
    }
    /* Tracked at HEAD but no longer staged: staged deletions. */
    for (int i = 0; i < head_count; i++) {
        if (!index_find(&index, head[i].path)) {
            section_header(&shown, "Changes to be committed");
            printf("%s  deleted:    %s%s\n", C_GREEN, head[i].path, C_RESET);
            staged++;
        }
    }

    /* 2. Unstaged — the working tree diverged from the index. */
    shown = 0;
    for (int i = 0; i < index.count; i++) {
        struct stat st;
        if (stat(index.entries[i].path, &st) != 0) {
            section_header(&shown, "Changes not staged for commit");
            printf("%s  deleted:    %s%s\n", C_RED, index.entries[i].path, C_RESET);
            unstaged++;
        } else if (st.st_mtime != (time_t)index.entries[i].mtime_sec ||
                   st.st_size != (off_t)index.entries[i].size) {
            section_header(&shown, "Changes not staged for commit");
            printf("%s  modified:   %s%s\n", C_RED, index.entries[i].path, C_RESET);
            unstaged++;
        }
    }

    /* 3. Untracked — present in the working tree, missing from the index. */
    shown = 0;
    scan_untracked("", &index, &shown, &untracked);

    if (sections_shown == 0)
        printf("nothing to commit, working tree clean\n");

    free(head);
    return 0;
}
