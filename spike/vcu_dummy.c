/* vcu_dummy.c: stand-in for vcu_sim.exe in the spike. Opens the shared block
 * the car script opens, answers every new tick with 50 Nm while the pedal is
 * past 10% (0 Nm otherwise, and with the brake on), and prints a status line
 * once a second.
 *
 *   vcu_dummy.exe [--nm 50] [--sleep] [--seconds N]           Windows
 *   vcu_dummy --file PATH [--nm 50] [--sleep] [--seconds N]   Mac/Linux (tests)
 *
 * --sleep waits 1 ms between polls instead of spinning on a core.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bridge_layout.h"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

#define BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)

static double now_s(void) {
#ifdef _WIN32
  static LARGE_INTEGER f;
  LARGE_INTEGER c;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)f.QuadPart;
#else
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
#endif
}

static void idle(int sleep_ms) {
#ifdef _WIN32
  if (sleep_ms) Sleep(1); else SwitchToThread();
#else
  if (sleep_ms) usleep(1000); else sched_yield();
#endif
}

static volatile bridge_block *open_block(const char *file) {
#ifdef _WIN32
  (void)file;
  /* CSP names its files Local\<name>; creating or opening gives the same block
   * whichever side starts first. */
  HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                sizeof(bridge_block), "Local\\" BRIDGE_NAME);
  if (!h) { fprintf(stderr, "CreateFileMapping failed: %lu\n", GetLastError()); return NULL; }
  void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge_block));
  if (!p) { fprintf(stderr, "MapViewOfFile failed: %lu\n", GetLastError()); return NULL; }
  return (volatile bridge_block *)p;
#else
  if (!file) { fprintf(stderr, "--file PATH is required off Windows\n"); return NULL; }
  int fd = open(file, O_RDWR | O_CREAT, 0600);
  if (fd < 0 || ftruncate(fd, sizeof(bridge_block)) != 0) { perror(file); return NULL; }
  void *p = mmap(NULL, sizeof(bridge_block), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) { perror("mmap"); return NULL; }
  return (volatile bridge_block *)p;
#endif
}

int main(int argc, char **argv) {
  float cmd_nm = 50.0f;
  int sleep_ms = 0;
  double run_for = 0;
  const char *file = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--nm") && i + 1 < argc) cmd_nm = (float)atof(argv[++i]);
    else if (!strcmp(argv[i], "--sleep")) sleep_ms = 1;
    else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) run_for = atof(argv[++i]);
    else if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
    else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
  }
#ifdef _WIN32
  if (sleep_ms) timeBeginPeriod(1);
#endif

  volatile bridge_block *b = open_block(file);
  if (!b) return 1;
  printf("vcu_dummy: %s, %.0f Nm past 10%% pedal, %s\n", BRIDGE_NAME, cmd_nm,
         sleep_ms ? "1 ms sleep" : "spinning");
  printf("Start AC with the spike car (or it's already running). Ctrl+C to stop.\n");
  fflush(stdout);

  uint32_t last = b->seq, ticks = 0, answers = 0;
  float gas = 0, brake = 0, rpm = 0, out = 0;
  int gear = 0, warned = 0;
  double start = now_s(), report = start, last_answer = 0;

  for (;;) {
    uint32_t s1 = b->seq;
    BARRIER();
    float g = b->gas, br = b->brake, r = b->rpm;
    int gr = b->gear;
    uint32_t magic = b->magic, version = b->version;
    BARRIER();
    uint32_t s2 = b->seq;

    if (s1 == s2 && s1 != last && magic == BRIDGE_MAGIC) {
      if (version != BRIDGE_VERSION && !warned) {
        printf("warning: car script layout v%u, this program v%u\n", version, BRIDGE_VERSION);
        warned = 1;
      }
      gas = g; brake = br; rpm = r; gear = gr;
      out = (gas > 0.10f && brake <= 0.10f) ? cmd_nm : 0.0f;
      b->torque_nm = out;
      b->vcu_state = 2; /* drive */
      b->fault_bits = 0;
      b->vcu_ticks = ++ticks;
      BARRIER();
      b->seq_echo = s1;
      last = s1;
      answers++;
      last_answer = now_s();
    } else {
      idle(sleep_ms);
    }

    double t = now_s();
    if (t - report >= 1.0) {
      const char *state = (last_answer > 0 && t - last_answer < 0.05) ? "LINKED " : "waiting";
      printf("%s | answers/s %4.0f | gas %.2f brake %.2f rpm %5.0f gear %d | sending %.0f Nm\n",
             state, answers / (t - report), gas, brake, rpm, gear, out);
      fflush(stdout);
      answers = 0;
      report = t;
    }
    if (run_for > 0 && t - start >= run_for) return 0;
  }
}
