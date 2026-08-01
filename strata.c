/*
 * strata.c — Command-line interface.
 *
 *     strata init                  create a repository
 *     strata add <file>...         stage files
 *     strata rm <file>...          unstage files
 *     strata status                show staged / unstaged / untracked
 *     strata commit -m <message>   snapshot the staging area
 *     strata log                   walk the commit history
 *     strata --version             print version and exit
 *
 * Parsing is deliberately plain: one command per invocation, and options
 * kept to the minimum the tool actually needs.
 */

#include "strata.h"
#include "index.h"
#include "commit.h"
#include "status.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

/* Resolve the identity stamped on commits. */
const char *strata_author(void) {
    const char *env = getenv("STRATA_AUTHOR");
    if (env && env[0]) return env;

    const char *user = getenv("USER");
    if (!user || !user[0]) user = "anonymous";

    static char buf[512];
    snprintf(buf, sizeof(buf), "%s <%s@localhost>", user, user);
    return buf;
}

/* Every command except init needs an existing repository. */
static int require_repo(void) {
    if (access(REPO_DIR, F_OK) != 0) {
        fprintf(stderr, "error: not a strata repository (run 'strata init' first)\n");
        return -1;
    }
    return 0;
}

/* strata init */
static void cmd_init(void) {
    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), ".");

    if (mkdir(REPO_DIR, 0755) != 0 && access(REPO_DIR, F_OK) != 0) {
        fprintf(stderr, "error: failed to create %s\n", REPO_DIR);
        return;
    }
    mkdir(OBJECTS_DIR, 0755);
    mkdir(".strata/refs", 0755);
    mkdir(REFS_DIR, 0755);

    int reinit = 0;
    if (access(HEAD_FILE, F_OK) != 0) {
        FILE *f = fopen(HEAD_FILE, "w");
        if (f) {
            fprintf(f, "ref: refs/heads/main\n");
            fclose(f);
        }
    } else {
        reinit = 1;
    }

    printf(reinit
               ? "Reinitialized existing strata repository in %s/%s/\n"
               : "Initialized empty strata repository in %s/%s/\n",
           cwd, REPO_DIR);
}

/* strata add <file>... */
static void cmd_add(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: strata add <file>...\n");
        return;
    }

    Index index;
    if (index_load(&index) != 0) {
        fprintf(stderr, "error: failed to load the index\n");
        return;
    }

    for (int i = 2; i < argc; i++) {
        if (index_add(&index, argv[i]) != 0)
            fprintf(stderr, "error: failed to stage '%s'\n", argv[i]);
    }
}

/* strata rm <file>... */
static void cmd_rm(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: strata rm <file>...\n");
        return;
    }

    Index index;
    if (index_load(&index) != 0) {
        fprintf(stderr, "error: failed to load the index\n");
        return;
    }

    for (int i = 2; i < argc; i++)
        index_remove(&index, argv[i]);
}

/* strata status */
static void cmd_status(void) {
    (void)status_run();
}

/* strata commit -m <message> */
static void cmd_commit(int argc, char *argv[]) {
    if (argc < 4 || strcmp(argv[2], "-m") != 0) {
        fprintf(stderr, "error: commit requires a message (strata commit -m \"message\")\n");
        return;
    }

    ObjectID commit_id;
    if (commit_create(argv[3], &commit_id) != 0)
        return;

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&commit_id, hex);

    char branch[64];
    head_branch(branch, sizeof(branch));
    printf("[%s %.*s] %s\n", branch, 12, hex, argv[3]);
}

/* Format a Unix timestamp as "Mon Aug  1 12:34:56 2026 +0000". */
static const char *format_timestamp(uint64_t ts) {
    static char out[64];
    time_t t = (time_t)ts;

    struct tm local_tm, utc_tm;
    localtime_r(&t, &local_tm);
    gmtime_r(&t, &utc_tm);

    /* Reconstruct the UTC offset from the two decompositions. */
    long off = (local_tm.tm_hour - utc_tm.tm_hour) * 3600L
             + (local_tm.tm_min - utc_tm.tm_min) * 60L;
    if (off > 12 * 3600L) off -= 24 * 3600L;
    if (off < -12 * 3600L) off += 24 * 3600L;

    char base[48];
    strftime(base, sizeof(base), "%a %b %e %H:%M:%S %Y", &local_tm);
    long abs_off = off < 0 ? -off : off;
    snprintf(out, sizeof(out), "%s %c%02ld%02ld", base,
             off < 0 ? '-' : '+',
             abs_off / 3600, (abs_off % 3600) / 60);
    return out;
}

/* Callback used by cmd_log: print one commit. */
static void print_commit(const ObjectID *id, const Commit *commit, void *ctx) {
    int *pos = (int *)ctx;
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);

    printf("commit %.*s", 12, hex);
    if (*pos == 0) {
        char branch[64];
        head_branch(branch, sizeof(branch));
        printf(" (HEAD -> %s)", branch);
    }
    printf("\n");
    printf("Author: %s\n", commit->author);
    printf("Date:   %s\n", format_timestamp(commit->timestamp));
    printf("\n    %s\n\n", commit->message);
    (*pos)++;
}

/* strata log */
static void cmd_log(void) {
    int pos = 0;
    if (commit_walk(print_commit, &pos) != 0)
        fprintf(stderr, "No commits yet.\n");
}

static void usage(void) {
    fprintf(stderr,
        "Usage: strata <command> [args]\n"
        "\n"
        "A minimal version control system, inspired by git's internals.\n"
        "\n"
        "Commands:\n"
        "  init                  Create a new repository\n"
        "  add <file>...         Stage files for the next commit\n"
        "  rm <file>...          Unstage files\n"
        "  status                Show staged, unstaged, and untracked changes\n"
        "  commit -m <message>   Create a commit from the staging area\n"
        "  log                   Show commit history\n"
        "\n"
        "Run 'strata --version' to print the version.\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0) {
        printf("strata version %s\n", STRATA_VERSION);
        return 0;
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "init") != 0 && require_repo() != 0)
        return 1;

    if      (strcmp(cmd, "init") == 0)   cmd_init();
    else if (strcmp(cmd, "add") == 0)    cmd_add(argc, argv);
    else if (strcmp(cmd, "rm") == 0)     cmd_rm(argc, argv);
    else if (strcmp(cmd, "status") == 0) cmd_status();
    else if (strcmp(cmd, "commit") == 0) cmd_commit(argc, argv);
    else if (strcmp(cmd, "log") == 0)    cmd_log();
    else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        fprintf(stderr, "Run 'strata' with no arguments for usage.\n");
        return 1;
    }
    return 0;
}
