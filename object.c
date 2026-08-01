/*
 * object.c — Content-addressable object store.
 *
 * Every piece of data — file contents, directory listings, commits — is
 * stored as an "object" named after the SHA-256 of its serialized form.
 * Identical content therefore deduplicates to a single object, and any
 * corruption is caught on read by re-hashing and comparing against the
 * name the object was looked up by.
 *
 * On disk an object is "<type> <size>\0" followed by raw data, stored at
 * .strata/objects/XX/YYY... where XX is the first two hex characters of
 * the hash. Sharding like this keeps individual directories small as the
 * store grows.
 *
 * Every write goes through a temp file + fsync + rename, so a crash can
 * never leave a partial object at its final path.
 */

#include "strata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

/* Format a hash as lowercase hex. */
void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++)
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    hex_out[HASH_HEX_SIZE] = '\0';
}

/* Parse 64 hex characters back into a hash. */
int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

/* SHA-256 of an arbitrary buffer. */
static void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

/* Filesystem path for an object: .strata/objects/XX/YYYY... */
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

/* True if an object with this id already exists on disk. */
int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

/*
 * Write an object to the store. The header and data are hashed together,
 * so the resulting id uniquely identifies the content. Returns 0 on
 * success, -1 on error; *id_out receives the computed hash either way.
 */
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    static const char *type_names[] = { "blob", "tree", "commit" };
    if (type < OBJ_BLOB || type > OBJ_COMMIT) return -1;

    /* Assemble "<type> <size>\0<data>". */
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_names[type], len) + 1;
    if (header_len <= 1 || header_len > (int)sizeof(header)) return -1;

    size_t full_len = (size_t)header_len + len;
    uint8_t *full = malloc(full_len);
    if (!full) return -1;
    memcpy(full, header, (size_t)header_len);
    memcpy(full + header_len, data, len);

    compute_hash(full, full_len, id_out);

    /* Already stored? Identical content maps to the same hash. */
    if (object_exists(id_out)) {
        free(full);
        return 0;
    }

    char dir_path[512], obj_path[512], tmp_path[512];
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);
    snprintf(dir_path, sizeof(dir_path), "%s/%.2s", OBJECTS_DIR, hex);
    snprintf(obj_path, sizeof(obj_path), "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%.2s/tmp_XXXXXX", OBJECTS_DIR, hex);

    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
        free(full);
        return -1;
    }

    int fd = mkstemp(tmp_path);
    if (fd < 0) { free(full); return -1; }

    /* write() may short-circuit; loop until everything is on the page cache. */
    size_t written = 0;
    while (written < full_len) {
        ssize_t n = write(fd, full + written, full_len - written);
        if (n < 0) { close(fd); unlink(tmp_path); free(full); return -1; }
        written += (size_t)n;
    }
    free(full);

    if (fsync(fd) != 0) { close(fd); unlink(tmp_path); return -1; }
    if (close(fd) != 0) { unlink(tmp_path); return -1; }

    if (rename(tmp_path, obj_path) != 0) { unlink(tmp_path); return -1; }

    /* Persist the rename itself by syncing the shard directory. */
    int dir_fd = open(dir_path, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }
    return 0;
}

/*
 * Read an object back. The contents are re-hashed on every read and
 * compared to the id, so a corrupted file is reported as an error rather
 * than silently returning garbage; the declared size in the header is
 * also cross-checked against the actual payload.
 *
 * On success *data_out is freshly allocated (caller must free it).
 * Returns 0 on success, -1 if the object is missing or corrupt.
 */
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long file_len = ftell(f);
    if (file_len < 0) { fclose(f); return -1; }
    rewind(f);

    uint8_t *buf = malloc((size_t)file_len);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)file_len, f) != (size_t)file_len) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    /* Integrity: the object's name is its hash — verify it. */
    ObjectID computed;
    compute_hash(buf, (size_t)file_len, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) { free(buf); return -1; }

    /* The header is "<type> <size>\0"; find the separator. */
    uint8_t *sep = memchr(buf, '\0', (size_t)file_len);
    if (!sep) { free(buf); return -1; }

    ObjectType type;
    size_t type_len;
    if      (strncmp((char *)buf, "blob",   4) == 0) { type = OBJ_BLOB;   type_len = 4; }
    else if (strncmp((char *)buf, "tree",   4) == 0) { type = OBJ_TREE;   type_len = 4; }
    else if (strncmp((char *)buf, "commit", 6) == 0) { type = OBJ_COMMIT; type_len = 6; }
    else { free(buf); return -1; }

    /* Cross-check the declared size against what is actually stored. */
    unsigned long declared;
    if (sscanf((char *)buf + type_len + 1, "%lu", &declared) != 1) { free(buf); return -1; }
    size_t data_len = (size_t)file_len - (size_t)(sep + 1 - buf);
    if (declared != data_len) { free(buf); return -1; }

    uint8_t *data = NULL;
    if (data_len > 0) {
        data = malloc(data_len);
        if (!data) { free(buf); return -1; }
        memcpy(data, sep + 1, data_len);
    }
    free(buf);

    *type_out = type;
    *data_out = data;
    *len_out  = data_len;
    return 0;
}
