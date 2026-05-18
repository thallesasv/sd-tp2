#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define P2P_MAGIC 0x50325031u
#define PROTO_VERSION 1u
#define MAX_LINE 1024
#define MAX_PEERS 64
#define MAX_INFLIGHT_PER_CONN 4
#define RECV_CAP 65536u
#define SEND_CAP 65536u

typedef enum {
    MSG_HELLO = 1,
    MSG_META = 2,
    MSG_BLOCK_REQUEST = 3,
    MSG_BLOCK_DATA = 4,
    MSG_DONE = 5
} MessageType;

typedef struct {
    char filename[PATH_MAX];
    uint64_t file_size;
    uint32_t block_size;
    uint32_t block_count;
    char sha256_hex[65];
} MetaInfo;

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    size_t datalen;
} Sha256Ctx;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t reserved;
    uint32_t block_index;
    uint32_t payload_len;
} WireHeader;

typedef struct Connection {
    int fd;
    bool outgoing;
    bool connected;
    bool meta_sent;
    unsigned char *recv_buf;
    size_t recv_len;
    size_t recv_cap;
    unsigned char *send_buf;
    size_t send_len;
    size_t send_off;
    size_t send_cap;
    uint32_t inflight[MAX_INFLIGHT_PER_CONN];
    size_t inflight_count;
    char remote_name[64];
} Connection;

typedef struct {
    MetaInfo meta;
    bool meta_loaded;
    bool meta_loaded_from_file;
    bool seed_mode;
    bool stop_on_complete;
    bool complete_reported;
    char peer_name[64];
    char listen_host[64];
    uint16_t listen_port;
    char input_path[PATH_MAX];
    char output_path[PATH_MAX];
    char meta_path[PATH_MAX];
    int input_fd;
    int output_fd;
    size_t received_blocks;
    size_t next_request_cursor;
    unsigned char *have_block;
    unsigned char *requested_block;
    Connection *conns;
    size_t conn_count;
    size_t conn_cap;
    int listen_fd;
    struct sockaddr_in listen_addr;
    struct sockaddr_in peer_targets[MAX_PEERS];
    size_t peer_target_count;
} PeerContext;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void fail_msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        die("malloc");
    }
    return ptr;
}

static void *xcalloc(size_t count, size_t size) {
    void *ptr = calloc(count, size);
    if (!ptr) {
        die("calloc");
    }
    return ptr;
}

static void *xrealloc(void *ptr, size_t size) {
    void *result = realloc(ptr, size);
    if (!result) {
        die("realloc");
    }
    return result;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

static void trim_newline(char *text) {
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[--len] = '\0';
    }
}

static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) {
        slash = backslash;
    }
#endif
    return slash ? slash + 1 : path;
}

static uint64_t parse_u64(const char *text) {
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        fail_msg("invalid numeric value in metadata");
    }
    return (uint64_t)value;
}

static uint32_t parse_u32(const char *text) {
    uint64_t value = parse_u64(text);
    if (value > UINT32_MAX) {
        fail_msg("numeric value too large in metadata");
    }
    return (uint32_t)value;
}

static void sha256_transform(Sha256Ctx *ctx, const uint8_t data[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90beffbau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];

    for (size_t i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) | ((uint32_t)data[j + 2] << 8) | (uint32_t)data[j + 3];
    }
    for (size_t i = 16; i < 64; ++i) {
        uint32_t s0 = (m[i - 15] >> 7 | m[i - 15] << 25) ^ (m[i - 15] >> 18 | m[i - 15] << 14) ^ (m[i - 15] >> 3);
        uint32_t s1 = (m[i - 2] >> 17 | m[i - 2] << 15) ^ (m[i - 2] >> 19 | m[i - 2] << 13) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (size_t i = 0; i < 64; ++i) {
        uint32_t S1 = (e >> 6 | e << 26) ^ (e >> 11 | e << 21) ^ (e >> 25 | e << 7);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + k[i] + m[i];
        uint32_t S0 = (a >> 2 | a << 30) ^ (a >> 13 | a << 19) ^ (a >> 22 | a << 10);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(Sha256Ctx *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(Sha256Ctx *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == sizeof(ctx->data)) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(Sha256Ctx *ctx, uint8_t hash[32]) {
    size_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80u;
        while (i < 56) {
            ctx->data[i++] = 0x00u;
        }
    } else {
        ctx->data[i++] = 0x80u;
        while (i < 64) {
            ctx->data[i++] = 0x00u;
        }
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8u;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        for (size_t j = 0; j < 8; ++j) {
            hash[i + (j * 4)] = (uint8_t)(ctx->state[j] >> (24 - i * 8));
        }
    }
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *hex_out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        hex_out[i * 2] = digits[bytes[i] >> 4];
        hex_out[i * 2 + 1] = digits[bytes[i] & 0x0fu];
    }
    hex_out[len * 2] = '\0';
}

static void compute_sha256_file(const char *path, char out_hex[65]) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        die(path);
    }

    Sha256Ctx ctx;
    sha256_init(&ctx);

    unsigned char buffer[8192];
    size_t read_bytes;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        sha256_update(&ctx, buffer, read_bytes);
    }

    if (ferror(fp)) {
        fclose(fp);
        die("fread");
    }

    fclose(fp);

    uint8_t hash[32];
    sha256_final(&ctx, hash);
    bytes_to_hex(hash, sizeof(hash), out_hex);
}

static bool load_metadata_file(const char *path, MetaInfo *meta) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }

    memset(meta, 0, sizeof(*meta));
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        trim_newline(line);
        if (strncmp(line, "filename=", 9) == 0) {
            snprintf(meta->filename, sizeof(meta->filename), "%s", line + 9);
        } else if (strncmp(line, "filesize=", 9) == 0) {
            meta->file_size = parse_u64(line + 9);
        } else if (strncmp(line, "block_size=", 11) == 0) {
            meta->block_size = parse_u32(line + 11);
        } else if (strncmp(line, "block_count=", 12) == 0) {
            meta->block_count = parse_u32(line + 12);
        } else if (strncmp(line, "sha256=", 7) == 0) {
            snprintf(meta->sha256_hex, sizeof(meta->sha256_hex), "%s", line + 7);
        }
    }

    fclose(fp);
    return meta->filename[0] != '\0' && meta->file_size > 0 && meta->block_size > 0 && meta->block_count > 0 && meta->sha256_hex[0] != '\0';
}

static void save_metadata_file(const char *path, const MetaInfo *meta) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        die(path);
    }

    fprintf(fp, "filename=%s\n", meta->filename);
    fprintf(fp, "filesize=%llu\n", (unsigned long long)meta->file_size);
    fprintf(fp, "block_size=%u\n", meta->block_size);
    fprintf(fp, "block_count=%u\n", meta->block_count);
    fprintf(fp, "sha256=%s\n", meta->sha256_hex);

    fclose(fp);
}

static void init_metadata_from_input(PeerContext *ctx, const char *input_path, uint32_t block_size) {
    struct stat st;
    if (stat(input_path, &st) != 0) {
        die(input_path);
    }

    if (!S_ISREG(st.st_mode)) {
        fail_msg("input file must be a regular file");
    }

    memset(&ctx->meta, 0, sizeof(ctx->meta));
    snprintf(ctx->meta.filename, sizeof(ctx->meta.filename), "%s", path_basename(input_path));
    ctx->meta.file_size = (uint64_t)st.st_size;
    ctx->meta.block_size = block_size;
    ctx->meta.block_count = (uint32_t)((ctx->meta.file_size + block_size - 1u) / block_size);
    compute_sha256_file(input_path, ctx->meta.sha256_hex);

    if (ctx->meta_path[0] == '\0') {
        snprintf(ctx->meta_path, sizeof(ctx->meta_path), "%s.meta", input_path);
    }
    save_metadata_file(ctx->meta_path, &ctx->meta);
    ctx->meta_loaded = true;
    ctx->meta_loaded_from_file = true;
}

static void open_input_file(PeerContext *ctx) {
    ctx->input_fd = open(ctx->input_path, O_RDONLY);
    if (ctx->input_fd < 0) {
        die(ctx->input_path);
    }
}

static void open_output_file(PeerContext *ctx) {
    if (ctx->output_path[0] == '\0') {
        snprintf(ctx->output_path, sizeof(ctx->output_path), "downloaded_%s", ctx->meta.filename[0] ? ctx->meta.filename : "peer_file.bin");
    }

    ctx->output_fd = open(ctx->output_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (ctx->output_fd < 0) {
        die(ctx->output_path);
    }
    if (ftruncate(ctx->output_fd, (off_t)ctx->meta.file_size) != 0) {
        die("ftruncate");
    }
}

static void ensure_storage_ready(PeerContext *ctx) {
    if (!ctx->meta_loaded) {
        return;
    }
    if (!ctx->have_block) {
        ctx->have_block = xcalloc(ctx->meta.block_count, sizeof(unsigned char));
        ctx->requested_block = xcalloc(ctx->meta.block_count, sizeof(unsigned char));
        ctx->received_blocks = 0;
        ctx->next_request_cursor = 0;
    }
    if (!ctx->seed_mode && ctx->output_fd < 0) {
        open_output_file(ctx);
    }
    if (ctx->seed_mode && ctx->input_fd < 0) {
        open_input_file(ctx);
    }
    if (ctx->seed_mode) {
        for (uint32_t i = 0; i < ctx->meta.block_count; ++i) {
            ctx->have_block[i] = 1;
        }
        ctx->received_blocks = ctx->meta.block_count;
    }
}

static void log_line(const PeerContext *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stdout, "[%s:%u] ", ctx->listen_host, ctx->listen_port);
    vfprintf(stdout, fmt, args);
    fprintf(stdout, "\n");
    fflush(stdout);
    va_end(args);
}

static int parse_host_port(const char *text, char *host_out, size_t host_cap, uint16_t *port_out) {
    const char *colon = strrchr(text, ':');
    if (!colon || colon == text || *(colon + 1) == '\0') {
        return -1;
    }

    size_t host_len = (size_t)(colon - text);
    if (host_len >= host_cap) {
        return -1;
    }

    memcpy(host_out, text, host_len);
    host_out[host_len] = '\0';

    long port = strtol(colon + 1, NULL, 10);
    if (port <= 0 || port > 65535) {
        return -1;
    }
    *port_out = (uint16_t)port;
    return 0;
}

static int create_listen_socket(const char *host, uint16_t port, struct sockaddr_in *addr_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket");
    }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        die("setsockopt");
    }

    memset(addr_out, 0, sizeof(*addr_out));
    addr_out->sin_family = AF_INET;
    addr_out->sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr_out->sin_addr) != 1) {
        fail_msg("invalid listen address");
    }

    if (bind(fd, (struct sockaddr *)addr_out, sizeof(*addr_out)) != 0) {
        die("bind");
    }
    if (listen(fd, 16) != 0) {
        die("listen");
    }
    if (set_nonblocking(fd) != 0) {
        die("fcntl");
    }
    return fd;
}

static int connect_nonblocking(const char *host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket");
    }
    if (set_nonblocking(fd) != 0) {
        die("fcntl");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        fail_msg("invalid peer address");
    }

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        return fd;
    }
    if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
        return fd;
    }

    close(fd);
    die("connect");
    return -1;
}

static Connection *add_connection(PeerContext *ctx, int fd, bool outgoing, const char *remote_name) {
    if (ctx->conn_count == ctx->conn_cap) {
        size_t new_cap = ctx->conn_cap ? ctx->conn_cap * 2u : 8u;
        ctx->conns = xrealloc(ctx->conns, new_cap * sizeof(Connection));
        memset(ctx->conns + ctx->conn_cap, 0, (new_cap - ctx->conn_cap) * sizeof(Connection));
        ctx->conn_cap = new_cap;
    }

    Connection *conn = &ctx->conns[ctx->conn_count++];
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;
    conn->outgoing = outgoing;
    conn->connected = !outgoing;
    conn->recv_cap = RECV_CAP;
    conn->send_cap = SEND_CAP;
    conn->recv_buf = xmalloc(conn->recv_cap);
    conn->send_buf = xmalloc(conn->send_cap);
    if (remote_name) {
        snprintf(conn->remote_name, sizeof(conn->remote_name), "%s", remote_name);
    }
    return conn;
}

static void disconnect_peer(PeerContext *ctx, size_t index, const char *reason) {
    Connection *conn = &ctx->conns[index];
    if (reason) {
        log_line(ctx, "disconnecting %s (%s)", conn->remote_name[0] ? conn->remote_name : "peer", reason);
    }
    for (size_t i = 0; i < conn->inflight_count; ++i) {
        uint32_t block = conn->inflight[i];
        if (block < ctx->meta.block_count) {
            ctx->requested_block[block] = 0;
        }
    }
    close(conn->fd);
    free(conn->recv_buf);
    free(conn->send_buf);

    if (index + 1 != ctx->conn_count) {
        ctx->conns[index] = ctx->conns[ctx->conn_count - 1];
    }
    memset(&ctx->conns[ctx->conn_count - 1], 0, sizeof(Connection));
    ctx->conn_count--;
}

static bool queue_bytes(Connection *conn, const void *data, size_t len) {
    if (conn->send_len + len > conn->send_cap) {
        size_t new_cap = conn->send_cap;
        while (new_cap < conn->send_len + len) {
            new_cap *= 2u;
        }
        conn->send_buf = xrealloc(conn->send_buf, new_cap);
        conn->send_cap = new_cap;
    }
    memcpy(conn->send_buf + conn->send_len, data, len);
    conn->send_len += len;
    return true;
}

static bool queue_message(Connection *conn, uint8_t type, uint32_t block_index, const void *payload, uint32_t payload_len) {
    WireHeader header;
    header.magic = htonl(P2P_MAGIC);
    header.version = PROTO_VERSION;
    header.type = type;
    header.reserved = 0;
    header.block_index = htonl(block_index);
    header.payload_len = htonl(payload_len);
    if (!queue_bytes(conn, &header, sizeof(header))) {
        return false;
    }
    if (payload_len > 0 && payload) {
        if (!queue_bytes(conn, payload, payload_len)) {
            return false;
        }
    }
    return true;
}

static bool queue_meta(Connection *conn, const MetaInfo *meta) {
    return queue_message(conn, MSG_META, 0, meta, (uint32_t)sizeof(*meta));
}

static bool queue_block_request(Connection *conn, uint32_t block_index) {
    return queue_message(conn, MSG_BLOCK_REQUEST, block_index, NULL, 0);
}

static bool queue_block_data(PeerContext *ctx, Connection *conn, uint32_t block_index) {
    if (!ctx->seed_mode && block_index >= ctx->meta.block_count) {
        return false;
    }

    uint64_t offset_in_file = (uint64_t)block_index * ctx->meta.block_size;
    size_t bytes_left = (size_t)(ctx->meta.file_size - offset_in_file);
    size_t payload_len = bytes_left < ctx->meta.block_size ? bytes_left : ctx->meta.block_size;
    unsigned char *buffer = xmalloc(payload_len);

    ssize_t got;
    if (ctx->seed_mode) {
        got = pread(ctx->input_fd, buffer, payload_len, (off_t)offset_in_file);
    } else {
        got = pread(ctx->output_fd, buffer, payload_len, (off_t)offset_in_file);
    }
    if (got < 0 || (size_t)got != payload_len) {
        free(buffer);
        return false;
    }

    bool ok = queue_message(conn, MSG_BLOCK_DATA, block_index, buffer, (uint32_t)payload_len);
    free(buffer);
    return ok;
}

static bool all_blocks_present(const PeerContext *ctx) {
    return ctx->meta_loaded && ctx->received_blocks >= ctx->meta.block_count;
}

static void report_completion(PeerContext *ctx) {
    if (ctx->seed_mode || ctx->complete_reported || !all_blocks_present(ctx)) {
        return;
    }

    ctx->complete_reported = true;
    if (ctx->output_fd >= 0) {
        fsync(ctx->output_fd);
    }

    if (ctx->meta.sha256_hex[0] != '\0') {
        char local_hash[65];
        compute_sha256_file(ctx->output_path, local_hash);
        if (strcmp(local_hash, ctx->meta.sha256_hex) == 0) {
            log_line(ctx, "download complete: %s verified with SHA-256 %s", ctx->output_path, local_hash);
        } else {
            log_line(ctx, "download complete but hash mismatch: expected %s got %s", ctx->meta.sha256_hex, local_hash);
        }
    } else {
        log_line(ctx, "download complete: %s", ctx->output_path);
    }

    if (ctx->stop_on_complete) {
        log_line(ctx, "stop-on-complete enabled; shutting down");
        exit(EXIT_SUCCESS);
    }
}

static ssize_t find_next_missing_block(PeerContext *ctx) {
    if (!ctx->meta_loaded || all_blocks_present(ctx)) {
        return -1;
    }

    uint32_t total = ctx->meta.block_count;
    for (uint32_t offset = 0; offset < total; ++offset) {
        uint32_t index = (ctx->next_request_cursor + offset) % total;
        if (!ctx->have_block[index] && !ctx->requested_block[index]) {
            ctx->next_request_cursor = (index + 1u) % total;
            return (ssize_t)index;
        }
    }
    return -1;
}

static void request_more_blocks(PeerContext *ctx) {
    if (!ctx->meta_loaded || all_blocks_present(ctx)) {
        return;
    }

    for (size_t i = 0; i < ctx->conn_count; ++i) {
        Connection *conn = &ctx->conns[i];
        if (!conn->connected) {
            continue;
        }
        while (conn->inflight_count < MAX_INFLIGHT_PER_CONN) {
            ssize_t next = find_next_missing_block(ctx);
            if (next < 0) {
                return;
            }
            uint32_t block = (uint32_t)next;
            if (ctx->requested_block[block]) {
                break;
            }
            if (!queue_block_request(conn, block)) {
                return;
            }
            ctx->requested_block[block] = 1;
            conn->inflight[conn->inflight_count++] = block;
            log_line(ctx, "requesting block %u from %s", block, conn->remote_name[0] ? conn->remote_name : "peer");
        }
    }
}

static void finalize_received_block(PeerContext *ctx, uint32_t block_index) {
    if (block_index >= ctx->meta.block_count || ctx->have_block[block_index]) {
        return;
    }

    ctx->have_block[block_index] = 1;
    ctx->requested_block[block_index] = 0;
    ctx->received_blocks++;
    log_line(ctx, "received block %u/%u", block_index + 1u, ctx->meta.block_count);
    report_completion(ctx);
}

static void handle_meta_message(PeerContext *ctx, const MetaInfo *meta) {
    if (!ctx->meta_loaded) {
        memcpy(&ctx->meta, meta, sizeof(*meta));
        ctx->meta_loaded = true;
        ensure_storage_ready(ctx);
        if (ctx->meta_path[0] != '\0' && !ctx->meta_loaded_from_file) {
            save_metadata_file(ctx->meta_path, &ctx->meta);
        }
        log_line(ctx, "metadata loaded: file=%s size=%llu block_size=%u blocks=%u", ctx->meta.filename, (unsigned long long)ctx->meta.file_size, ctx->meta.block_size, ctx->meta.block_count);
        return;
    }

    if (ctx->meta.file_size != meta->file_size || ctx->meta.block_size != meta->block_size || ctx->meta.block_count != meta->block_count || strcmp(ctx->meta.sha256_hex, meta->sha256_hex) != 0) {
        log_line(ctx, "metadata mismatch detected from peer; keeping local metadata");
    }
}

static void handle_block_request(PeerContext *ctx, Connection *conn, uint32_t block_index) {
    if (!ctx->meta_loaded || block_index >= ctx->meta.block_count) {
        return;
    }

    if (!ctx->have_block[block_index]) {
        return;
    }

    if (!queue_block_data(ctx, conn, block_index)) {
        log_line(ctx, "failed to queue block %u to %s", block_index, conn->remote_name[0] ? conn->remote_name : "peer");
    }
}

static void remove_inflight_from_conn(Connection *conn, uint32_t block_index) {
    size_t write_pos = 0;
    for (size_t i = 0; i < conn->inflight_count; ++i) {
        if (conn->inflight[i] != block_index) {
            conn->inflight[write_pos++] = conn->inflight[i];
        }
    }
    conn->inflight_count = write_pos;
}

static void handle_block_data(PeerContext *ctx, Connection *conn, uint32_t block_index, const unsigned char *payload, uint32_t payload_len) {
    if (!ctx->meta_loaded || block_index >= ctx->meta.block_count) {
        return;
    }

    size_t offset = (size_t)block_index * ctx->meta.block_size;
    if (payload_len > ctx->meta.block_size) {
        return;
    }

    ssize_t written = pwrite(ctx->output_fd, payload, payload_len, (off_t)offset);
    if (written < 0 || (uint32_t)written != payload_len) {
        die("pwrite");
    }

    remove_inflight_from_conn(conn, block_index);
    finalize_received_block(ctx, block_index);
}

static bool consume_message(PeerContext *ctx, Connection *conn) {
    if (conn->recv_len < sizeof(WireHeader)) {
        return false;
    }

    WireHeader header;
    memcpy(&header, conn->recv_buf, sizeof(header));
    if (ntohl(header.magic) != P2P_MAGIC || header.version != PROTO_VERSION) {
        return false;
    }

    uint32_t payload_len = ntohl(header.payload_len);
    size_t total_len = sizeof(WireHeader) + payload_len;
    if (conn->recv_len < total_len) {
        return false;
    }

    uint32_t block_index = ntohl(header.block_index);
    const unsigned char *payload = conn->recv_buf + sizeof(WireHeader);

    switch (header.type) {
        case MSG_HELLO:
            break;
        case MSG_META:
            if (payload_len == sizeof(MetaInfo)) {
                MetaInfo incoming;
                memcpy(&incoming, payload, sizeof(incoming));
                handle_meta_message(ctx, &incoming);
            }
            break;
        case MSG_BLOCK_REQUEST:
            handle_block_request(ctx, conn, block_index);
            break;
        case MSG_BLOCK_DATA:
            handle_block_data(ctx, conn, block_index, payload, payload_len);
            break;
        case MSG_DONE:
            break;
        default:
            break;
    }

    memmove(conn->recv_buf, conn->recv_buf + total_len, conn->recv_len - total_len);
    conn->recv_len -= total_len;
    return true;
}

static void read_from_connection(PeerContext *ctx, size_t index) {
    Connection *conn = &ctx->conns[index];
    unsigned char temp[8192];

    for (;;) {
        ssize_t got = recv(conn->fd, temp, sizeof(temp), 0);
        if (got > 0) {
            if (conn->recv_len + (size_t)got > conn->recv_cap) {
                size_t new_cap = conn->recv_cap;
                while (new_cap < conn->recv_len + (size_t)got) {
                    new_cap *= 2u;
                }
                conn->recv_buf = xrealloc(conn->recv_buf, new_cap);
                conn->recv_cap = new_cap;
            }
            memcpy(conn->recv_buf + conn->recv_len, temp, (size_t)got);
            conn->recv_len += (size_t)got;
            while (consume_message(ctx, conn)) {
                ;
            }
            continue;
        }

        if (got == 0) {
            disconnect_peer(ctx, index, "peer closed connection");
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        disconnect_peer(ctx, index, strerror(errno));
        return;
    }
}

static void flush_connection(PeerContext *ctx, size_t index) {
    Connection *conn = &ctx->conns[index];
    while (conn->send_off < conn->send_len) {
        ssize_t sent = send(conn->fd, conn->send_buf + conn->send_off, conn->send_len - conn->send_off, 0);
        if (sent > 0) {
            conn->send_off += (size_t)sent;
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        disconnect_peer(ctx, index, sent < 0 ? strerror(errno) : "send failure");
        return;
    }

    if (conn->send_off == conn->send_len) {
        conn->send_off = 0;
        conn->send_len = 0;
    }
}

static void finalize_connect(Connection *conn) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        errno = err;
        return;
    }
    conn->connected = true;
}

static void accept_incoming(PeerContext *ctx) {
    for (;;) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int fd = accept(ctx->listen_fd, (struct sockaddr *)&addr, &len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            die("accept");
        }

        if (set_nonblocking(fd) != 0) {
            close(fd);
            die("fcntl");
        }

        char remote[64];
        snprintf(remote, sizeof(remote), "%s:%u", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        Connection *conn = add_connection(ctx, fd, false, remote);
        if (ctx->meta_loaded) {
            queue_meta(conn, &ctx->meta);
            conn->meta_sent = true;
        }
        log_line(ctx, "accepted connection from %s", remote);
    }
}

static void connect_targets(PeerContext *ctx) {
    for (size_t i = 0; i < ctx->peer_target_count; ++i) {
        char host[64];
        inet_ntop(AF_INET, &ctx->peer_targets[i].sin_addr, host, sizeof(host));
        uint16_t port = ntohs(ctx->peer_targets[i].sin_port);
        char remote[64];
        snprintf(remote, sizeof(remote), "%s:%u", host, port);
        int fd = connect_nonblocking(host, port);
        (void)add_connection(ctx, fd, true, remote);
        log_line(ctx, "connecting to %s", remote);
    }
}

static void try_send_metadata(PeerContext *ctx) {
    if (!ctx->meta_loaded) {
        return;
    }
    for (size_t i = 0; i < ctx->conn_count; ++i) {
        Connection *conn = &ctx->conns[i];
        if (conn->connected && !conn->meta_sent) {
            queue_meta(conn, &ctx->meta);
            conn->meta_sent = true;
        }
    }
}

static void mark_existing_seed_blocks(PeerContext *ctx) {
    if (!ctx->seed_mode || !ctx->meta_loaded) {
        return;
    }
    for (uint32_t i = 0; i < ctx->meta.block_count; ++i) {
        ctx->have_block[i] = 1;
    }
    ctx->received_blocks = ctx->meta.block_count;
}

static void init_peer_context(PeerContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->input_fd = -1;
    ctx->output_fd = -1;
    ctx->listen_fd = -1;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s --listen IP:PORT [--input FILE] [--output FILE] [--meta FILE] [--block-size N] [--peer IP:PORT ...] [--id NAME] [--stop-on-complete]\n\n"
        "Examples:\n"
        "  Seeder: %s --listen 127.0.0.1:5000 --input arquivo.bin --peer 127.0.0.1:5001 --meta arquivo.bin.meta\n"
        "  Leecher: %s --listen 127.0.0.1:5001 --meta arquivo.bin.meta --output download.bin --peer 127.0.0.1:5000\n",
        prog, prog, prog);
}

static void parse_args(PeerContext *ctx, int argc, char **argv) {
    uint32_t block_size_override = 1024;
    bool listen_set = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            if (parse_host_port(argv[++i], ctx->listen_host, sizeof(ctx->listen_host), &ctx->listen_port) != 0) {
                fail_msg("invalid --listen value");
            }
            listen_set = true;
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            snprintf(ctx->input_path, sizeof(ctx->input_path), "%s", argv[++i]);
            ctx->seed_mode = true;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            snprintf(ctx->output_path, sizeof(ctx->output_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--meta") == 0 && i + 1 < argc) {
            snprintf(ctx->meta_path, sizeof(ctx->meta_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--block-size") == 0 && i + 1 < argc) {
            block_size_override = parse_u32(argv[++i]);
            if (block_size_override == 0) {
                fail_msg("block size must be greater than zero");
            }
        } else if (strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
            if (ctx->peer_target_count >= MAX_PEERS) {
                fail_msg("too many peers");
            }
            char host[64];
            uint16_t port;
            if (parse_host_port(argv[++i], host, sizeof(host), &port) != 0) {
                fail_msg("invalid --peer value");
            }
            memset(&ctx->peer_targets[ctx->peer_target_count], 0, sizeof(struct sockaddr_in));
            ctx->peer_targets[ctx->peer_target_count].sin_family = AF_INET;
            ctx->peer_targets[ctx->peer_target_count].sin_port = htons(port);
            if (inet_pton(AF_INET, host, &ctx->peer_targets[ctx->peer_target_count].sin_addr) != 1) {
                fail_msg("invalid --peer address");
            }
            ctx->peer_target_count++;
        } else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            snprintf(ctx->peer_name, sizeof(ctx->peer_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--stop-on-complete") == 0) {
            ctx->stop_on_complete = true;
        } else {
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (!listen_set) {
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (ctx->seed_mode) {
        init_metadata_from_input(ctx, ctx->input_path, block_size_override);
    } else if (ctx->meta_path[0] != '\0') {
        ctx->meta_loaded = load_metadata_file(ctx->meta_path, &ctx->meta);
        ctx->meta_loaded_from_file = ctx->meta_loaded;
    }

    if (ctx->peer_name[0] == '\0') {
        snprintf(ctx->peer_name, sizeof(ctx->peer_name), "%s:%u", ctx->listen_host, ctx->listen_port);
    }
}

int main(int argc, char **argv) {
    PeerContext ctx;
    init_peer_context(&ctx);
    parse_args(&ctx, argc, argv);

    ctx.listen_fd = create_listen_socket(ctx.listen_host, ctx.listen_port, &ctx.listen_addr);
    if (ctx.meta_loaded) {
        ensure_storage_ready(&ctx);
        mark_existing_seed_blocks(&ctx);
    }

    log_line(&ctx, "peer started (%s)", ctx.peer_name);
    if (ctx.meta_loaded) {
        log_line(&ctx, "metadata ready: file=%s size=%llu block_size=%u blocks=%u", ctx.meta.filename, (unsigned long long)ctx.meta.file_size, ctx.meta.block_size, ctx.meta.block_count);
    } else {
        log_line(&ctx, "waiting for metadata from a peer");
    }

    connect_targets(&ctx);

    for (;;) {
        fd_set readfds;
        fd_set writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        FD_SET(ctx.listen_fd, &readfds);
        int maxfd = ctx.listen_fd;

        for (size_t i = 0; i < ctx.conn_count; ++i) {
            Connection *conn = &ctx.conns[i];
            FD_SET(conn->fd, &readfds);
            if ((!conn->connected || conn->send_len > conn->send_off)) {
                FD_SET(conn->fd, &writefds);
            }
            if (conn->fd > maxfd) {
                maxfd = conn->fd;
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        int ready = select(maxfd + 1, &readfds, &writefds, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("select");
        }

        if (FD_ISSET(ctx.listen_fd, &readfds)) {
            accept_incoming(&ctx);
        }

        for (size_t i = 0; i < ctx.conn_count;) {
            Connection *conn = &ctx.conns[i];
            if (!conn->connected && FD_ISSET(conn->fd, &writefds)) {
                finalize_connect(conn);
                if (!conn->connected) {
                    disconnect_peer(&ctx, i, strerror(errno));
                    continue;
                }
                if (conn->connected && ctx.meta_loaded && !conn->meta_sent) {
                    queue_meta(conn, &ctx.meta);
                    conn->meta_sent = true;
                }
            }

            if (i >= ctx.conn_count) {
                continue;
            }

            conn = &ctx.conns[i];
            if (conn->connected && FD_ISSET(conn->fd, &readfds)) {
                read_from_connection(&ctx, i);
                if (i >= ctx.conn_count) {
                    continue;
                }
                conn = &ctx.conns[i];
            }

            if (i >= ctx.conn_count) {
                continue;
            }

            conn = &ctx.conns[i];
            if (conn->connected && FD_ISSET(conn->fd, &writefds)) {
                flush_connection(&ctx, i);
                if (i >= ctx.conn_count) {
                    continue;
                }
                conn = &ctx.conns[i];
            }

            ++i;
        }

        if (ctx.meta_loaded && !ctx.seed_mode) {
            request_more_blocks(&ctx);
        }
        try_send_metadata(&ctx);
        if (ctx.meta_loaded) {
            report_completion(&ctx);
        }
    }

    return 0;
}