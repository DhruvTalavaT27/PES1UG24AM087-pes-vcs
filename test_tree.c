/*
 * test_tree.c — Unit tests for tree serialization and flattening.
 *
 * Build: make test_tree
 * Run:   ./test_tree
 */

#include "strata.h"
#include "tree.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

static void test_roundtrip(void) {
    Tree original;
    original.count = 3;

    original.entries[0].mode = 0100644;
    memset(original.entries[0].hash.hash, 0xAA, HASH_SIZE);
    strcpy(original.entries[0].name, "README.md");

    original.entries[1].mode = 0040000;
    memset(original.entries[1].hash.hash, 0xBB, HASH_SIZE);
    strcpy(original.entries[1].name, "src");

    original.entries[2].mode = 0100755;
    memset(original.entries[2].hash.hash, 0xCC, HASH_SIZE);
    strcpy(original.entries[2].name, "build.sh");

    void *data;
    size_t len;
    assert(tree_serialize(&original, &data, &len) == 0);
    printf("serialized tree: %zu bytes\n", len);

    Tree parsed;
    assert(tree_parse(data, len, &parsed) == 0);
    assert(parsed.count == 3);

    /* Serialization sorts by name: README.md < build.sh < src. */
    assert(strcmp(parsed.entries[0].name, "README.md") == 0);
    assert(strcmp(parsed.entries[1].name, "build.sh") == 0);
    assert(strcmp(parsed.entries[2].name, "src") == 0);

    assert(parsed.entries[0].mode == 0100644);
    assert(parsed.entries[1].mode == 0100755);
    assert(parsed.entries[2].mode == 0040000);

    assert(memcmp(parsed.entries[0].hash.hash, original.entries[0].hash.hash, HASH_SIZE) == 0);

    free(data);
    printf("PASS  serialize/parse roundtrip (modes, hashes, sort order)\n");
}

static void test_determinism(void) {
    Tree a, b;
    a.count = 2;
    b.count = 2;

    a.entries[0].mode = 0100644;
    memset(a.entries[0].hash.hash, 0x11, HASH_SIZE);
    strcpy(a.entries[0].name, "z_file.txt");
    a.entries[1].mode = 0100644;
    memset(a.entries[1].hash.hash, 0x22, HASH_SIZE);
    strcpy(a.entries[1].name, "a_file.txt");

    b.entries[0].mode = 0100644;
    memset(b.entries[0].hash.hash, 0x22, HASH_SIZE);
    strcpy(b.entries[0].name, "a_file.txt");
    b.entries[1].mode = 0100644;
    memset(b.entries[1].hash.hash, 0x11, HASH_SIZE);
    strcpy(b.entries[1].name, "z_file.txt");

    void *da, *db;
    size_t la, lb;
    assert(tree_serialize(&a, &da, &la) == 0);
    assert(tree_serialize(&b, &db, &lb) == 0);

    assert(la == lb);
    assert(memcmp(da, db, la) == 0);

    free(da);
    free(db);
    printf("PASS  deterministic serialization (entry order does not matter)\n");
}

static void test_flatten(void) {
    /* Build src/main.c and README.md as real objects, then flatten the root. */
    const char *readme = "# demo\n";
    const char *main_c = "int main(void) { return 0; }\n";

    ObjectID readme_blob, main_blob;
    assert(object_write(OBJ_BLOB, readme, strlen(readme), &readme_blob) == 0);
    assert(object_write(OBJ_BLOB, main_c, strlen(main_c), &main_blob) == 0);

    /* src tree: one entry, main.c */
    Tree src_tree;
    src_tree.count = 1;
    src_tree.entries[0].mode = 0100644;
    src_tree.entries[0].hash = main_blob;
    strcpy(src_tree.entries[0].name, "main.c");

    void *src_data;
    size_t src_len;
    assert(tree_serialize(&src_tree, &src_data, &src_len) == 0);
    ObjectID src_id;
    assert(object_write(OBJ_TREE, src_data, src_len, &src_id) == 0);
    free(src_data);

    /* root tree: README.md blob + src subtree */
    Tree root;
    root.count = 2;
    root.entries[0].mode = 0100644;
    root.entries[0].hash = readme_blob;
    strcpy(root.entries[0].name, "README.md");
    root.entries[1].mode = 0040000;
    root.entries[1].hash = src_id;
    strcpy(root.entries[1].name, "src");

    void *root_data;
    size_t root_len;
    assert(tree_serialize(&root, &root_data, &root_len) == 0);
    ObjectID root_id;
    assert(object_write(OBJ_TREE, root_data, root_len, &root_id) == 0);
    free(root_data);

    FlatEntry *flat;
    int count;
    assert(tree_flatten(&root_id, &flat, &count) == 0);
    assert(count == 2);

    /* Entries come out sorted by path. */
    assert(strcmp(flat[0].path, "README.md") == 0);
    assert(strcmp(flat[1].path, "src/main.c") == 0);
    assert(memcmp(flat[1].hash.hash, main_blob.hash, HASH_SIZE) == 0);

    free(flat);
    printf("PASS  tree_flatten walks nested subtrees\n");
}

int main(void) {
    int rc __attribute__((unused));
    rc = system("rm -rf .strata");
    rc = system("mkdir -p .strata/objects .strata/refs/heads");

    test_roundtrip();
    test_determinism();
    test_flatten();

    printf("\nAll tree tests passed.\n");
    return 0;
}
