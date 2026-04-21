#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>

#define DEVICE "/dev/membuf0"
#define THREADS 4
#define WRITE_SIZE 512
#define ITERATIONS 10

#define MEMBUF_IOC_MAGIC 'm'
#define MEMBUF_IOC_RESIZE _IOW(MEMBUF_IOC_MAGIC, 1, struct membuf_resize)

struct membuf_resize {
    unsigned int minor;
    size_t new_size;
};

void test_open_close() {
    int fd = open(DEVICE, O_RDWR);
    assert(fd >= 0);
    close(fd);
    printf("test_open_close OK\n");
}

void test_write_read() {
    int fd = open(DEVICE, O_RDWR);
    assert(fd >= 0);

    const char *msg = "Hello";
    write(fd, msg, strlen(msg));

    lseek(fd, 0, SEEK_SET);

    char buf[64] = {0};
    int n = read(fd, buf, sizeof(buf));
    assert(n == strlen(msg));
    assert(strncmp(buf, msg, n) == 0);

    close(fd);
    printf("test_write_read OK\n");
}

void test_ioctl_resize() {
    int fd = open(DEVICE, O_RDWR);
    assert(fd >= 0);

    struct membuf_resize r = {0, 4096};
    int ret = ioctl(fd, MEMBUF_IOC_RESIZE, &r);
    assert(ret == 0);

    char *data = malloc(3000);
    memset(data, 'A', 3000);

    write(fd, data, 3000);

    lseek(fd, 0, SEEK_SET);

    char *buf = malloc(3000);
    int n = read(fd, buf, 3000);
    assert(n == 3000);

    free(data);
    free(buf);
    close(fd);

    printf("test_ioctl_resize OK\n");
}

void test_multiple_devices() {
    int fds[3];

    for (int i = 0; i < 3; i++) {
        char path[64];
        sprintf(path, "/dev/membuf%d", i);
        fds[i] = open(path, O_RDWR);
        assert(fds[i] >= 0);
    }

    const char *data[] = {"A", "B", "C"};

    for (int i = 0; i < 3; i++) {
        write(fds[i], data[i], 1);
    }

    for (int i = 0; i < 3; i++) {
        lseek(fds[i], 0, SEEK_SET);
        char c;
        read(fds[i], &c, 1);
        assert(c == data[i][0]);
        close(fds[i]);
    }

    printf("test_multiple_devices OK\n");
}

void test_boundary() {
    int fd = open(DEVICE, O_RDWR);
    assert(fd >= 0);

    struct membuf_resize r = {0, 1024};
    ioctl(fd, MEMBUF_IOC_RESIZE, &r);

    int n = write(fd, "abc", 0);
    assert(n == 0);

    struct membuf_resize wrong = {99, 1024};
    int ret = ioctl(fd, MEMBUF_IOC_RESIZE, &wrong);
    assert(ret == -1);

    close(fd);
    printf("test_boundary OK\n");
}

typedef struct {
    int fd;
    char symbol;
} writer_arg;

typedef struct {
    int fd;
} reader_arg;

void* writer(void* arg) {
    writer_arg* a = arg;

    char buf[WRITE_SIZE];
    memset(buf, a->symbol, WRITE_SIZE);

    for (int i = 0; i < ITERATIONS; i++) {
        lseek(a->fd, 0, SEEK_SET);
        write(a->fd, buf, WRITE_SIZE);
    }

    return NULL;
}

void* reader(void* arg) {
    reader_arg* a = arg;

    char buf[WRITE_SIZE];

    for (int i = 0; i < ITERATIONS; i++) {
        lseek(a->fd, 0, SEEK_SET);
        int n = read(a->fd, buf, WRITE_SIZE);

        if (n > 0) {
            char c = buf[0];
            for (int i = 1; i < n; i++) {
                assert(buf[i] == c);
            }
        }
    }

    return NULL;
}

void test_rw_mutex() {
    int fd = open(DEVICE, O_RDWR);
    assert(fd >= 0);

    pthread_t w[THREADS], r[THREADS];
    writer_arg wa[THREADS];
    reader_arg ra[THREADS];

    for (int i = 0; i < THREADS; i++) {
        wa[i].fd = fd;
        wa[i].symbol = 'A' + i;
        pthread_create(&w[i], NULL, writer, &wa[i]);
    }

    for (int i = 0; i < THREADS; i++) {
        ra[i].fd = fd;
        pthread_create(&r[i], NULL, reader, &ra[i]);
    }

    for (int i = 0; i < THREADS; i++) {
        pthread_join(w[i], NULL);
        pthread_join(r[i], NULL);
    }

    close(fd);
    printf("test_rw_mutex OK\n");
}

int main() {
    if (access(DEVICE, F_OK) != 0) {
        printf("device not found\n");
        return 1;
    }

    test_open_close();
    test_write_read();
    test_multiple_devices();
    test_ioctl_resize();
    test_boundary();
    test_rw_mutex();

    printf("ALL TESTS PASSED\n");
    return 0;
}