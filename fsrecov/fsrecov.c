#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include "fat32.h"

_Static_assert(sizeof(struct fat32hdr) == 512, "FAT BPB layout");
_Static_assert(sizeof(struct fat32dent) == 32, "FAT directory layout");

typedef struct {
    const unsigned char *bytes;
    size_t mapped, volume, data, cluster_size, fat;
    uint32_t clusters;
} Disk;
typedef struct { char name[256]; uint32_t first, size; } Image;

static uint16_t le16(const unsigned char *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int power_of_two(uint32_t n) { return n && !(n & (n - 1)); }

static int map_disk(const char *name, Disk *disk) {
    int fd = open(name, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0) { perror(name); return -1; }
    if (fstat(fd, &st) || st.st_size < 512 || (uint64_t)st.st_size > SIZE_MAX) {
        close(fd); fprintf(stderr, "Invalid image length\n"); return -1;
    }
    disk->mapped = (size_t)st.st_size;
    void *mapping = mmap(NULL, disk->mapped, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) { perror("mmap"); return -1; }
    disk->bytes = mapping;
    const unsigned char *p = mapping;
    uint32_t sector = le16(p + 11), spc = p[13], reserved = le16(p + 14);
    uint32_t fats = p[16], fat_sectors = le32(p + 36), sectors = le32(p + 32);
    uint64_t volume = (uint64_t)sector * sectors;
    uint64_t first_data = ((uint64_t)reserved + (uint64_t)fats * fat_sectors) * sector;
    if (le16(p + 510) != 0xaa55 || sector < 512 || sector > 4096 || !power_of_two(sector) ||
        !power_of_two(spc) || spc > 128 || !reserved || !fats || fats > 2 ||
        !fat_sectors || !sectors || le16(p + 17) || le16(p + 22) ||
        volume > disk->mapped || first_data >= volume) goto invalid;
    disk->volume = (size_t)volume;
    disk->data = (size_t)first_data;
    disk->cluster_size = (size_t)sector * spc;
    uint64_t clusters = (volume - first_data) / disk->cluster_size;
    if (!clusters || clusters > 0x0fffffed ||
        (uint64_t)fat_sectors * sector / 4 < clusters + 2) goto invalid;
    disk->clusters = (uint32_t)clusters;
    disk->fat = (size_t)reserved * sector;
    return 0;
invalid:
    fprintf(stderr, "Not a supported FAT32 volume\n");
    munmap(mapping, disk->mapped);
    return -1;
}

static int cluster_valid(const Disk *disk, uint32_t cluster) {
    return cluster >= 2 && cluster - 2 < disk->clusters;
}
static const unsigned char *cluster_bytes(const Disk *disk, uint32_t cluster) {
    return disk->bytes + disk->data + (size_t)(cluster - 2) * disk->cluster_size;
}
static int allowed_stem(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}
static int image_name(const char *name) {
    size_t n = strlen(name);
    if (n < 5 || n > 255 || strcasecmp(name + n - 4, ".bmp")) return 0;
    for (size_t i = 0; i < n - 4; i++) if (!allowed_stem((unsigned char)name[i])) return 0;
    return 1;
}
static unsigned char short_checksum(const unsigned char *entry) {
    unsigned char checksum = 0;
    for (int i = 0; i < 11; i++) checksum = (unsigned char)(((checksum & 1) << 7) + (checksum >> 1) + entry[i]);
    return checksum;
}

static int long_name(const Disk *disk, size_t offset, const unsigned char *directories, char name[256]) {
    static const unsigned char positions[] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    const unsigned char *entry = disk->bytes + offset;
    unsigned char checksum = short_checksum(entry);
    uint16_t characters[260];
    for (size_t i = 0; i < 260; i++) characters[i] = 0xffff;
    int complete = 0;
    size_t units = 0;
    for (unsigned int order = 1; order <= 20; order++) {
        int boundary = (offset - disk->data) % disk->cluster_size == 0;
        const unsigned char *part = offset >= disk->data + 32 ? disk->bytes + offset - 32 : NULL;
        int valid = part && (part[0] == order || part[0] == (order | 0x40)) &&
                    part[11] == 0x0f && !part[12] && part[13] == checksum && !le16(part + 26);
        if (!valid && boundary) {
            // A directory's next cluster need not be physically adjacent.
            // A matching ordinal/checksum at a recognized directory tail is
            // usable only when unique; ambiguous fragments are not guessed.
            part = NULL;
            for (uint32_t c = 0; c < disk->clusters; c++) {
                if (!directories[c]) continue;
                const unsigned char *candidate = cluster_bytes(disk, c + 2) + disk->cluster_size - 32;
                if ((candidate[0] == order || candidate[0] == (order | 0x40)) &&
                    candidate[11] == 0x0f && !candidate[12] && candidate[13] == checksum && !le16(candidate + 26)) {
                    if (part) return 0;
                    part = candidate;
                }
            }
            valid = part != NULL;
        }
        if (!valid) break;
        offset = (size_t)(part - disk->bytes);
        for (size_t i = 0; i < 13; i++) characters[(order - 1) * 13 + i] = le16(part + positions[i]);
        if (part[0] & 0x40) { complete = 1; units = order * 13; break; }
    }
    if (!complete) return 0;
    size_t n = 0;
    for (; n < units && characters[n] != 0; n++) {
        if (n >= 255 || characters[n] < 33 || characters[n] > 126) return 0;
        name[n] = (char)characters[n];
    }
    if (n > 255) return 0;
    name[n] = 0;
    return image_name(name);
}

static int short_name(const unsigned char *entry, char name[256]) {
    size_t n = 0;
    for (int i = 0; i < 8 && entry[i] != ' '; i++) {
        unsigned char c = entry[i];
        if (!allowed_stem(c)) return 0;
        name[n++] = (char)((entry[12] & 8) ? tolower(c) : c);
    }
    if (!n) return 0;
    name[n++] = '.';
    for (int i = 8; i < 11; i++) {
        unsigned char c = entry[i];
        if (!isalpha(c)) return 0;
        name[n++] = (char)((entry[12] & 16) ? tolower(c) : c);
    }
    name[n] = 0;
    return image_name(name);
}

static int bitmap_header(const Disk *disk, uint32_t first, uint32_t size) {
    if (!cluster_valid(disk, first) || size < 54 || size > disk->volume) return 0;
    const unsigned char *p = cluster_bytes(disk, first);
    if (p[0] != 'B' || p[1] != 'M' || le32(p + 2) != size ||
        le32(p + 14) != 40 || le16(p + 26) != 1 || le16(p + 28) != 24 || le32(p + 30)) return 0;
    int64_t width = (int32_t)le32(p + 18), height = (int32_t)le32(p + 22);
    uint32_t start = le32(p + 10);
    if (width <= 0 || !height || start < 54 || start > size) return 0;
    if (height < 0) height = -height;
    uint64_t row = ((uint64_t)width * 3 + 3) & ~UINT64_C(3);
    return row <= size && (uint64_t)height <= (size - start) / row;
}

static int write_all(int fd, const void *bytes, size_t count) {
    const unsigned char *p = bytes;
    while (count) {
        ssize_t n = write(fd, p, count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; count -= (size_t)n;
    }
    return 0;
}

static int checksum_file(int fd, char digest[41]) {
    int pipefd[2];
    if (lseek(fd, 0, SEEK_SET) < 0 || pipe2(pipefd, O_CLOEXEC)) return -1;
    pid_t child = fork();
    if (child == 0) {
        if (dup2(fd, STDIN_FILENO) < 0 || dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipefd[0]); close(pipefd[1]); close(fd);
        execlp("sha1sum", "sha1sum", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    if (child < 0) { close(pipefd[0]); return -1; }
    char result[128];
    size_t used = 0;
    while (used < sizeof(result)) {
        ssize_t n = read(pipefd[0], result + used, sizeof(result) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(pipefd[0]);
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) || used < 41 || result[40] != ' ') return -1;
    for (int i = 0; i < 40; i++) if (!isxdigit((unsigned char)result[i])) return -1;
    memcpy(digest, result, 40); digest[40] = 0;
    return 0;
}

static int recover_image(const Disk *disk, const Image *image, const uint32_t *owners, uint32_t *chain, uint32_t *visited, uint32_t stamp) {
    size_t count = ((size_t)image->size + disk->cluster_size - 1) / disk->cluster_size;
    if (!count || count > disk->clusters) return 0;
    uint32_t cluster = image->first;
    int fat_chain = 1;
    for (size_t i = 0; i < count; i++) {
        if (!cluster_valid(disk, cluster) || visited[cluster - 2] == stamp) { fat_chain = 0; break; }
        visited[cluster - 2] = stamp;
        chain[i] = cluster;
        uint32_t next = le32(disk->bytes + disk->fat + (size_t)cluster * 4) & 0x0fffffff;
        if (i + 1 == count) {
            if (next < 0x0ffffff8) fat_chain = 0;
        } else cluster = next;
    }
    if (!fat_chain) {
        size_t found = 0;
        cluster = image->first;
        // FAT allocation normally scans forward and can wrap at the end of
        // the volume. Skip conservative prefixes of other known BMP files
        // and directory clusters instead of copying them into this image.
        for (uint32_t seen = 0; seen < disk->clusters && found < count; seen++) {
            uint32_t owner = owners[cluster - 2];
            if (!owner || owner == stamp) chain[found++] = cluster;
            cluster = cluster == disk->clusters + 1 ? 2 : cluster + 1;
        }
        if (found != count) return 0;
    }
    char name[] = "/tmp/fsrecov-XXXXXX";
    int output = mkostemp(name, O_CLOEXEC);
    if (output < 0) return -1;
    unlink(name);
    size_t remaining = image->size;
    int result = 0;
    for (size_t i = 0; i < count; i++) {
        size_t amount = remaining < disk->cluster_size ? remaining : disk->cluster_size;
        if (write_all(output, cluster_bytes(disk, chain[i]), amount)) { result = -1; break; }
        remaining -= amount;
    }
    char digest[41];
    if (!result && checksum_file(output, digest)) result = -1;
    close(output);
    if (result) return -1;
    printf("%s  %s\n", digest, image->name);
    fflush(stdout);
    return fat_chain ? 2 : 1;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "Usage: fsrecov FILE\n"); return 1; }
    Disk disk = {0};
    if (map_disk(argv[1], &disk)) return 1;
    uint32_t *chain = malloc((size_t)disk.clusters * sizeof(*chain));
    uint32_t *visited = calloc(disk.clusters, sizeof(*visited));
    uint32_t *owners = calloc(disk.clusters, sizeof(*owners));
    uint32_t *starts = calloc(disk.clusters, sizeof(*starts));
    unsigned char *directories = calloc(disk.clusters, 1);
    Image *images = NULL;
    size_t length = 0, capacity = 0, fat_count = 0, contiguous_count = 0;
    int result = 0;
    if (!chain || !visited || !owners || !starts || !directories) { result = 1; goto done; }
    uint32_t root = le32(disk.bytes + 44) & 0x0fffffff;
    if (cluster_valid(&disk, root)) directories[root - 2] = 1;
    // Classify directory clusters before resolving any long-name boundary.
    for (size_t offset = disk.data; offset + 32 <= disk.volume; offset += 32) {
        const unsigned char *entry = disk.bytes + offset;
        if (!entry[0] || entry[0] == 0xe5 || (entry[11] & 0xd8) ||
            strncasecmp((const char *)entry + 8, "BMP", 3)) continue;
        uint32_t first = ((uint32_t)le16(entry + 20) << 16) | le16(entry + 26);
        if (bitmap_header(&disk, first, le32(entry + 28))) directories[(offset - disk.data) / disk.cluster_size] = 1;
    }
    for (size_t offset = disk.data; offset + 32 <= disk.volume; offset += 32) {
        const unsigned char *entry = disk.bytes + offset;
        if (!entry[0] || entry[0] == 0xe5 || (entry[11] & 0xd8) ||
            (entry[11] & 0x0f) == 0x0f || strncasecmp((const char *)entry + 8, "BMP", 3)) continue;
        Image image = {.first = ((uint32_t)le16(entry + 20) << 16) | le16(entry + 26), .size = le32(entry + 28)};
        if (!bitmap_header(&disk, image.first, image.size)) continue;
        if (!long_name(&disk, offset, directories, image.name) && !short_name(entry, image.name)) continue;
        size_t i;
        for (i = 0; i < length; i++) if (!strcmp(images[i].name, image.name) && images[i].first == image.first && images[i].size == image.size) break;
        if (i != length) continue;
        if (length == capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            if (next < capacity || next > SIZE_MAX / sizeof(*images)) { result = 1; break; }
            Image *grown = realloc(images, next * sizeof(*images));
            if (!grown) { result = 1; break; }
            images = grown; capacity = next;
        }
        images[length++] = image;
        if (length > UINT32_MAX) { result = 1; break; }
    }
    for (size_t i = 0; i < length; i++) starts[images[i].first - 2] = (uint32_t)i + 1;
    for (uint32_t c = 0; c < disk.clusters; c++) if (directories[c]) owners[c] = UINT32_MAX;
    for (size_t i = 0; i < length; i++) {
        uint32_t c = images[i].first, owner = (uint32_t)i + 1;
        size_t count = ((size_t)images[i].size + disk.cluster_size - 1) / disk.cluster_size;
        for (size_t j = 0; j < count; j++) {
            uint32_t at = c - 2;
            if (directories[at] || (starts[at] && starts[at] != owner) ||
                (owners[at] && owners[at] != owner)) break;
            owners[at] = owner;
            c = c == disk.clusters + 1 ? 2 : c + 1;
        }
    }
    for (size_t i = 0; i < length && !result; i++) {
        int recovered = recover_image(&disk, &images[i], owners, chain, visited, (uint32_t)i + 1);
        if (recovered < 0) { perror("Recover/checksum"); result = 1; break; }
        fat_count += recovered == 2;
        contiguous_count += recovered == 1;
    }
    fprintf(stderr, "Candidates=%zu FAT=%zu inferred=%zu\n", length, fat_count, contiguous_count);
done:
    free(chain); free(visited); free(owners); free(starts); free(directories); free(images);
    munmap((void *)disk.bytes, disk.mapped);
    return result;
}
