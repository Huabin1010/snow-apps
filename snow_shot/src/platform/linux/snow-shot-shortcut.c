/* Hands a GNOME media-key press to the running Snow Shot without loading it. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <openssl/sha.h>

static int suppressed_repeat(int argc, char **argv) {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/snow-shot-shortcut-%d.lock", (int)getuid());
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return 0;
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return 0;
    }
    char current[512];
    size_t used = 0;
    current[0] = '\0';
    for (int i = 1; i < argc && used + 1 < sizeof(current); ++i) {
        if (i > 1)
            current[used++] = ' ';
        const size_t length = strlen(argv[i]);
        const size_t room = sizeof(current) - used - 1;
        const size_t copy = length < room ? length : room;
        memcpy(current + used, argv[i], copy);
        used += copy;
        current[used] = '\0';
    }
    char previous[640];
    const ssize_t n = pread(fd, previous, sizeof(previous) - 1, 0);
    if (n >= 0)
        previous[n] = '\0';
    else
        previous[0] = '\0';
    long long previous_ms = 0;
    char previous_args[512];
    previous_args[0] = '\0';
    sscanf(previous, "%lld %511[^\n]", &previous_ms, previous_args);
    struct timeval now;
    gettimeofday(&now, NULL);
    const long long ms = (long long)now.tv_sec * 1000LL + now.tv_usec / 1000;
    const int suppress =
        previous_ms > 0 && ms >= previous_ms && ms - previous_ms < 1200 &&
        strcmp(previous_args, current) == 0;
    if (!suppress) {
        char line[640];
        const int length = snprintf(line, sizeof(line), "%lld %s\n", ms, current);
        if (length > 0 && ftruncate(fd, 0) == 0) {
            if (lseek(fd, 0, SEEK_SET) >= 0) {
                const ssize_t written = write(fd, line, (size_t)length);
                (void)written;
            }
        }
    }
    flock(fd, LOCK_UN);
    close(fd);
    return suppress;
}

static void exec_fallback(const char *binary, int argc, char **argv) {
    char **args = calloc((size_t)argc + 1, sizeof(char *));
    if (args == NULL)
        _exit(127);
    args[0] = (char *)binary;
    for (int i = 1; i < argc; ++i)
        args[i] = argv[i];
    execv(binary, args);
    _exit(127);
}

static int append_json_string(char *out, size_t cap, size_t *used, const char *text) {
    if (*used + 1 >= cap)
        return -1;
    out[(*used)++] = '"';
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        const char *extra = NULL;
        char one[2] = {0, 0};
        if (*p == '"' || *p == '\\') {
            if (*used + 2 >= cap)
                return -1;
            out[(*used)++] = '\\';
            out[(*used)++] = (char)*p;
            continue;
        }
        if (*p < 0x20) {
            extra = "\\u00";
            if (*used + 6 >= cap)
                return -1;
            memcpy(out + *used, extra, 4);
            *used += 4;
            static const char hex[] = "0123456789abcdef";
            out[(*used)++] = hex[*p >> 4];
            out[(*used)++] = hex[*p & 0x0f];
            continue;
        }
        one[0] = (char)*p;
        if (*used + 1 >= cap)
            return -1;
        out[(*used)++] = one[0];
    }
    if (*used + 1 >= cap)
        return -1;
    out[(*used)++] = '"';
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    if (suppressed_repeat(argc, argv))
        return 0;

    char self[4096];
    char binary[4096];
    ssize_t self_length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (self_length < 0)
        return 127;
    self[self_length] = '\0';
    char *slash = strrchr(self, '/');
    if (slash == NULL || (size_t)(slash - self) + strlen("/snow-shot") >= sizeof(binary))
        return 127;
    *slash = '\0';
    memcpy(binary, self, (size_t)(slash - self));
    memcpy(binary + (slash - self), "/snow-shot", strlen("/snow-shot") + 1);

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        struct passwd *password = getpwuid(getuid());
        home = password != NULL ? password->pw_dir : NULL;
    }
    if (home == NULL)
        exec_fallback(binary, argc, argv);

    char identity[8192];
    snprintf(identity, sizeof(identity), "%s|SnowShot|snow_shot", home);
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)identity, strlen(identity), digest);
    char hex[25];
    for (int i = 0; i < 12; ++i)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    hex[24] = '\0';

    const char *temp = getenv("TMPDIR");
    if (temp == NULL || temp[0] == '\0')
        temp = "/tmp";
    char socket_path[512];
    snprintf(socket_path, sizeof(socket_path), "%s/snow-shot-%.24s-v2", temp, hex);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        exec_fallback(binary, argc, argv);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        close(fd);
        exec_fallback(binary, argc, argv);
    }
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        exec_fallback(binary, argc, argv);
    }

    unsigned char random_bytes[16];
    int random_fd = open("/dev/urandom", O_RDONLY);
    if (random_fd < 0 || read(random_fd, random_bytes, sizeof(random_bytes)) != (ssize_t)sizeof(random_bytes)) {
        if (random_fd >= 0)
            close(random_fd);
        close(fd);
        exec_fallback(binary, argc, argv);
    }
    close(random_fd);
    char request_id[33];
    for (size_t i = 0; i < sizeof(random_bytes); ++i)
        snprintf(request_id + i * 2, 3, "%02x", random_bytes[i]);

    char payload[8192];
    size_t used = 0;
    const char *prefix = "{\"version\":2,\"arguments\":[";
    memcpy(payload, prefix, strlen(prefix));
    used = strlen(prefix);
    if (append_json_string(payload, sizeof(payload), &used, binary) != 0) {
        close(fd);
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        if (used + 1 >= sizeof(payload)) {
            close(fd);
            return 2;
        }
        payload[used++] = ',';
        if (append_json_string(payload, sizeof(payload), &used, argv[i]) != 0) {
            close(fd);
            return 2;
        }
    }
    char suffix[96];
    snprintf(suffix, sizeof(suffix), "],\"activate\":true,\"requestId\":\"%s\"}", request_id);
    if (used + strlen(suffix) >= sizeof(payload)) {
        close(fd);
        return 2;
    }
    memcpy(payload + used, suffix, strlen(suffix));
    used += strlen(suffix);

    uint32_t length = htonl((uint32_t)used);
    if (write(fd, &length, sizeof(length)) != (ssize_t)sizeof(length) ||
        write(fd, payload, used) != (ssize_t)used) {
        close(fd);
        exec_fallback(binary, argc, argv);
    }

    struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
    char receipt[1280];
    size_t got = 0;
    while (got < sizeof(receipt)) {
        if (poll(&poll_fd, 1, 300) <= 0)
            break;
        ssize_t n = read(fd, receipt + got, sizeof(receipt) - got);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (got >= 4) {
            uint32_t body = 0;
            memcpy(&body, receipt, 4);
            body = ntohl(body);
            if (body > 0 && body < 1024 && got >= 4 + body) {
                receipt[4 + body] = '\0';
                if (strstr(receipt + 4, "\"accepted\":true") != NULL &&
                    strstr(receipt + 4, request_id) != NULL) {
                    close(fd);
                    return 0;
                }
                break;
            }
        }
    }
    close(fd);
    return 0;
}
