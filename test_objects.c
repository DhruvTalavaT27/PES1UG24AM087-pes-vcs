/*
 * test_objects.c — Unit tests for the object store.
 *
 * Build: make test_objects
 * Run:   ./test_objects
 */

#include "strata.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

static void test_blob_storage(void) {
    const char *content = "Hello, strata!\n";
    ObjectID id;

    assert(object_write(OBJ_BLOB, content, strlen(content), &id) == 0);

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);
    printf("stored blob  %s\n", hex);

    char path[512];
    object_path(&id, path, sizeof(path));
    printf("    at      %s\n", path);

    ObjectType type;
    void *data;
    size_t len;
    assert(object_read(&id, &type, &data, &len) == 0);
    assert(type == OBJ_BLOB);
    assert(len == strlen(content));
    assert(memcmp(data, content, len) == 0);
    free(data);

    printf("PASS  blob roundtrip\n");
}

static void test_deduplication(void) {
    const char *content = "duplicate content\n";
    ObjectID id1, id2;

    assert(object_write(OBJ_BLOB, content, strlen(content), &id1) == 0);
    assert(object_write(OBJ_BLOB, content, strlen(content), &id2) == 0);

    assert(memcmp(&id1, &id2, sizeof(ObjectID)) == 0);
    assert(object_exists(&id1));

    /* Writing the same content twice must not create a second file. */
    char path[512];
    object_path(&id1, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    fclose(f);

    printf("PASS  deduplication (identical content, one object)\n");
}

static void test_integrity_check(void) {
    const char *content = "integrity is not optional\n";
    ObjectID id;
    assert(object_write(OBJ_BLOB, content, strlen(content), &id) == 0);

    /* Flip one byte in the stored file; the read must now fail. */
    char path[512];
    object_path(&id, path, sizeof(path));
    FILE *f = fopen(path, "r+b");
    assert(f != NULL);
    fseek(f, 20, SEEK_SET);
    fputc('X', f);
    fclose(f);

    ObjectType type;
    void *data;
    size_t len;
    assert(object_read(&id, &type, &data, &len) == -1);

    printf("PASS  corruption detected on read\n");
}

static void test_empty_blob(void) {
    ObjectID id;
    assert(object_write(OBJ_BLOB, "", 0, &id) == 0);

    ObjectType type;
    void *data;
    size_t len;
    assert(object_read(&id, &type, &data, &len) == 0);
    assert(type == OBJ_BLOB);
    assert(len == 0);
    free(data);   /* free(NULL) is fine */

    printf("PASS  empty blob roundtrip\n");
}

static void test_missing_object(void) {
    ObjectID id;
    memset(&id, 0xAB, sizeof(id));

    ObjectType type;
    void *data;
    size_t len;
    assert(object_read(&id, &type, &data, &len) == -1);

    printf("PASS  missing object reported as error\n");
}

int main(void) {
    int rc __attribute__((unused));
    rc = system("rm -rf .strata");
    rc = system("mkdir -p .strata/objects .strata/refs/heads");

    test_blob_storage();
    test_deduplication();
    test_integrity_check();
    test_empty_blob();
    test_missing_object();

    printf("\nAll object store tests passed.\n");
    return 0;
}
