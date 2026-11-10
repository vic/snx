// snx runtime: a Unix socket client for `snx wire` (nx/wire.srs), bound by `extern def` in nx/rt.srs.
// A string is srs_str (bytes + length, no NUL); length -1 = failure.
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

typedef struct { const uint8_t* p; int64_t n; } srs_str;

static const srs_str net_fail = {(const uint8_t*)"", -1};

static uint8_t* net_alloc(int64_t n) {
  uint8_t* p = malloc((size_t)(n > 0 ? n : 1));
  if (!p) { fputs("snx: out of memory\n", stderr); exit(70); }
  return p;
}

// a stream socket connected to the Unix socket at path, or -1
int64_t Nx__netOpen(srs_str path) {
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  if (path.n <= 0 || (size_t)path.n >= sizeof a.sun_path) return -1;
  memcpy(a.sun_path, path.p, (size_t)path.n);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr*)&a, sizeof a) != 0) { close(fd); return -1; }
  return fd;
}

// all of s written; false when the peer is gone (MSG_NOSIGNAL: no SIGPIPE)
bool Nx__netSend(int64_t fd, srs_str s) {
  int64_t o = 0;
  while (o < s.n) {
    ssize_t k = send((int)fd, s.p + o, (size_t)(s.n - o), MSG_NOSIGNAL);
    if (k <= 0) return false;
    o += k;
  }
  return true;
}

// up to n bytes, fewer only at the end of the stream
srs_str Nx__netRead(int64_t fd, int64_t n) {
  uint8_t* p = net_alloc(n);
  int64_t o = 0;
  while (o < n) {
    ssize_t k = read((int)fd, p + o, (size_t)(n - o));
    if (k < 0) { free(p); return net_fail; }
    if (k == 0) break;
    o += k;
  }
  return (srs_str){p, o};
}

// every byte up to the end of the stream
srs_str Nx__netDrain(int64_t fd) {
  int64_t cap = 65536, o = 0;
  uint8_t* p = net_alloc(cap);
  for (;;) {
    if (o == cap) {
      uint8_t* q = realloc(p, (size_t)(cap *= 2));
      if (!q) { fputs("snx: out of memory\n", stderr); exit(70); }
      p = q;
    }
    ssize_t k = read((int)fd, p + o, (size_t)(cap - o));
    if (k < 0) { free(p); return net_fail; }
    if (k == 0) return (srs_str){p, o};
    o += k;
  }
}

// v as an unsigned 64-bit decimal (std::to_string(uint64_t))
srs_str Nx__u64Str(int64_t v) {
  uint8_t* p = net_alloc(21);
  int n = snprintf((char*)p, 21, "%llu", (unsigned long long)(uint64_t)v);
  return (srs_str){p, n};
}
