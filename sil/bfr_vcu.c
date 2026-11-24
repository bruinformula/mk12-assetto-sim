/* bfr_vcu.c: Software-in-the-Loop (SIL) MK12 VCU executable for Assetto Corsa.
 *
 * Compiles the real HAL-free firmware files from vendor/mk12-vcu/Core/Src/
 * unmodified (torque_map.c, power_limit.c, traction_control.c, and Embedded
 * Coder Block_Diagram_1*.c), redirecting profile tunables at runtime to a
 * SimProfile loaded from YAML (or base.yaml defaults).
 *
 * Usage:
 *   bfr_vcu.exe [profile.yaml] [--profile PATH] [--tc-active | --tc-shadow | --no-tc]
 *               [--log PATH | --no-log] [--sleep]
 *   bfr_vcu --file PATH [--profile PATH] [--tc-active | --tc-shadow | --no-tc]
 *           [--log PATH | --no-log] [--sleep] [--seconds N]
 *   bfr_vcu --verify-reference CSV [--profile PATH]
 *
 * On Windows, dragging a .yaml profile file onto bfr_vcu.exe passes it as the
 * single positional argument and runs with that profile. By default, TC runs
 * in shadow mode (telemetry logged without cutting torque) and 100 Hz CSV
 * telemetry is saved to logs\ next to bfr_vcu.exe.
 */

/* Standard headers BEFORE #define const so libc headers are untouched. */
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define NOMINMAX
#include <io.h>
#include <windows.h>
#include <mmsystem.h>
#else
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "bridge_layout.h"
#include "profile_loader.h"
#include "traction_control.h"

/* Same #undef + #include pattern as vendor/mk12-vcu/tests/reference/reference.c:
 * redirect the firmware's compile-time tunables to g_profile at runtime without
 * modifying a single line of vendor/mk12-vcu/Core/. */
#define const
#include "torque_map.h"
#include "power_limit.h"
#undef const

static SimProfile g_profile;

#undef MAX_TORQUE
#undef REGEN_END_PERCENT
#undef APPS_INFLECTION_PERCENT
#undef REGEN_LIFTOFF_TORQUE
#undef REGEN_FADE_LO_KPH
#undef REGEN_FADE_HI_KPH
#undef REGEN_RAMP_NM_S
#undef RC_TIME_CONSTANT
#undef SLEW_RATE_LIMIT

#define MAX_TORQUE              (g_profile.max_nm)
#define REGEN_END_PERCENT       (g_profile.coast_end)
#define APPS_INFLECTION_PERCENT (g_profile.drive_start)
#define REGEN_LIFTOFF_TORQUE    (g_profile.liftoff_nm)
#define REGEN_FADE_LO_KPH       (g_profile.fade_lo_kph)
#define REGEN_FADE_HI_KPH       (g_profile.fade_hi_kph)
#define REGEN_RAMP_NM_S         (g_profile.ramp_nm_s)
#define RC_TIME_CONSTANT        (g_profile.tau_s)
#define SLEW_RATE_LIMIT         (g_profile.slew_nm_s)

#define const
#include "torque_map.c"
#include "power_limit.c"
#undef const

#define BARRIER()          __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define VCU_DT_S           0.001
#define TORQUE_TX_DIVIDER  10
#define MAX_TICK_DT_S      0.1f

#ifndef SIL_VERSION
#define SIL_VERSION        "dev"
#endif
#ifndef VCU_FW_REV
#define VCU_FW_REV         "unknown"
#endif

typedef enum {
  TC_MODE_SHADOW = 0, /* default: run Block_Diagram_1, log tc_nm/slip, do not cut torque */
  TC_MODE_ACTIVE = 1, /* --tc-active: apply Simulink TC torque cuts */
  TC_MODE_OFF    = 2, /* --no-tc: skip Block_Diagram_1 completely */
} TcMode;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

#ifdef _WIN32
static volatile LONG g_exited = 0;

static BOOL WINAPI on_win_ctrl(DWORD ctrl_type) {
  if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
      ctrl_type == CTRL_CLOSE_EVENT) {
    g_stop = 1;
    /* Give main() a moment to flush and close the CSV log on window close. */
    for (int i = 0; i < 50 && !g_exited; i++) {
      Sleep(10);
    }
    return TRUE;
  }
  return FALSE;
}
#endif

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

/* Match torqueDiag_deciNm() in vendor/mk12-vcu/Core/Src/torque_diag.c:
 * Nm -> 0.1 Nm counts, truncated toward zero, saturated to int16. */
static int16_t deci_nm(float nm) {
  float deci = nm * 10.0f;
  if (deci >= 32767.0f) return INT16_MAX;
  if (deci <= -32768.0f) return INT16_MIN;
  if (deci != deci) return 0;
  return (int16_t)deci;
}

static int16_t clamp_rpm_i16(float rpm) {
  if (!(rpm == rpm)) return 0;
  if (rpm >= 32767.0f) return 32767;
  if (rpm <= -32768.0f) return -32768;
  return (int16_t)rpm;
}

static float compute_slip_ratio(const float wheel_omega[4], float speed_mps) {
  float tire_radius_m = (TIRE_DIAMETER_IN * 0.0254f) * 0.5f;
  float v_rear = 0.5f * (wheel_omega[2] + wheel_omega[3]) * tire_radius_m;
  float v_ref = speed_mps;
  if (fabsf(v_rear) < 0.1f && fabsf(v_ref) < 0.1f) return 0.0f;
  float denom = fabsf(v_ref);
  if (denom < 1.0f) denom = 1.0f;
  float slip = (v_rear - v_ref) / denom;
  if (slip < -1.0f) slip = -1.0f;
  if (slip > 5.0f) slip = 5.0f;
  return slip;
}

static volatile bridge_block *open_block(const char *file) {
#ifdef _WIN32
  (void)file;
  /* CSP names its files Local\<name>; creating or opening gives the same block
   * whichever side starts first. */
  HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                sizeof(bridge_block), "Local\\" BRIDGE_NAME);
  if (!h) {
    fprintf(stderr, "CreateFileMapping failed: %lu\n", GetLastError());
    return NULL;
  }
  void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge_block));
  if (!p) {
    fprintf(stderr, "MapViewOfFile failed: %lu\n", GetLastError());
    CloseHandle(h);
    return NULL;
  }
  return (volatile bridge_block *)p;
#else
  if (!file) {
    fprintf(stderr, "--file PATH is required off Windows\n");
    return NULL;
  }
  int fd = open(file, O_RDWR | O_CREAT, 0600);
  if (fd < 0 || ftruncate(fd, (off_t)sizeof(bridge_block)) != 0) {
    perror(file);
    if (fd >= 0) close(fd);
    return NULL;
  }
  void *p = mmap(NULL, sizeof(bridge_block), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    perror("mmap");
    return NULL;
  }
  return (volatile bridge_block *)p;
#endif
}

/* Parse a CSV field (empty string allowed). Returns pointer past delimiter or end. */
static char *next_csv_field(char *p, char **out) {
  *out = p;
  if (!p) return NULL;
  char *c = strchr(p, ',');
  if (c) {
    *c = '\0';
    return c + 1;
  }
  return NULL;
}

/* Verify bfr_vcu's runtime torque_map + power_limit pipeline against a reference
 * CSV produced by vendor/mk12-vcu/tests/reference/reference.c (within 0.001 Nm). */
static int verify_reference_csv(const char *csv_path) {
  FILE *fp = fopen(csv_path, "r");
  if (!fp) {
    perror(csv_path);
    return 1;
  }

  TorqueFilter f;
  torqueMap_filterReset(&f);

  char line[512];
  unsigned long checked = 0, failures = 0, lineno = 0;
  double max_err = 0.0;
  const double TOL = 0.001;

  while (fgets(line, sizeof line, fp)) {
    lineno++;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    if (len == 0 || line[0] == '#') continue;
    if (strncmp(line, "kind,", 5) == 0) continue;

    char *cols[12] = {0};
    char *p = line;
    for (int i = 0; i < 12; i++) p = next_csv_field(p, &cols[i]);
    if (!cols[0] || !cols[11]) {
      fprintf(stderr, "%s:%lu: malformed row\n", csv_path, lineno);
      fclose(fp);
      return 1;
    }

    const char *kind = cols[0];
    if (strcmp(kind, "map") == 0) {
      float apps = strtof(cols[4], NULL);
      float bse = strtof(cols[5], NULL);
      float speed = strtof(cols[6], NULL);
      float exp_target = strtof(cols[9], NULL);
      float got = torqueMap_target(apps, bse, speed);
      double err = fabs((double)got - (double)exp_target);
      if (err > max_err) max_err = err;
      if (err > TOL) {
        if (failures < 10)
          fprintf(stderr, "FAIL %s:%lu map apps=%.4g bse=%.4g speed=%.4g: got %.6f exp %.6f\n",
                  csv_path, lineno, apps, bse, speed, got, exp_target);
        failures++;
      }
      checked++;
    } else if (strcmp(kind, "speed") == 0) {
      float rpm = strtof(cols[7], NULL);
      float exp_kph = strtof(cols[6], NULL);
      float got = torqueMap_carspeedKph((int16_t)rpm);
      double err = fabs((double)got - (double)exp_kph);
      if (err > max_err) max_err = err;
      if (err > TOL) {
        if (failures < 10)
          fprintf(stderr, "FAIL %s:%lu speed rpm=%.4g: got %.6f exp %.6f\n",
                  csv_path, lineno, rpm, got, exp_kph);
        failures++;
      }
      checked++;
    } else if (strcmp(kind, "cap") == 0) {
      float rpm = strtof(cols[7], NULL);
      float exp_cap = strtof(cols[10], NULL);
      float got = powerLimit_capNm(rpm);
      double err = fabs((double)got - (double)exp_cap);
      if (err > max_err) max_err = err;
      if (err > TOL) {
        if (failures < 10)
          fprintf(stderr, "FAIL %s:%lu cap rpm=%.4g: got %.6f exp %.6f\n",
                  csv_path, lineno, rpm, got, exp_cap);
        failures++;
      }
      checked++;
    } else if (strcmp(kind, "trace") == 0) {
      int tick = atoi(cols[2]);
      float dt = strtof(cols[3], NULL);
      float apps = strtof(cols[4], NULL);
      float bse = strtof(cols[5], NULL);
      float speed = strtof(cols[6], NULL);
      float rpm = strtof(cols[7], NULL);
      int cut = atoi(cols[8]);
      float exp_target = strtof(cols[9], NULL);
      float exp_cap = strtof(cols[10], NULL);
      float exp_out = strtof(cols[11], NULL);

      if (tick == 0) torqueMap_filterReset(&f);
      float cap = powerLimit_capNm(rpm);
      float out = torqueMap_requestLimited(&f, cut != 0, apps, bse, speed, cap, dt);

      double e_t = fabs((double)f.target - (double)exp_target);
      double e_c = fabs((double)cap - (double)exp_cap);
      double e_o = fabs((double)out - (double)exp_out);
      double err = fmax(e_t, fmax(e_c, e_o));
      if (err > max_err) max_err = err;
      if (err > TOL) {
        if (failures < 10)
          fprintf(stderr,
                  "FAIL %s:%lu trace %s tick=%d: out=%.6f (exp %.6f) tgt=%.6f (exp %.6f) cap=%.6f (exp %.6f)\n",
                  csv_path, lineno, cols[1], tick, out, exp_out, f.target, exp_target, cap, exp_cap);
        failures++;
      }
      checked++;
    }
  }
  fclose(fp);

  if (checked == 0) {
    fprintf(stderr, "FAIL %s: no rows checked\n", csv_path);
    return 1;
  }
  printf("verify-reference %s: %lu rows checked, max_err=%.6g Nm, %lu failures\n",
         csv_path, checked, max_err, failures);
  return failures == 0 ? 0 : 1;
}

static void print_usage(void) {
  printf("bfr_vcu v%s (mk12-vcu %s)\n", SIL_VERSION, VCU_FW_REV);
  printf("Usage:\n");
  printf("  bfr_vcu [profile.yaml] [--profile PATH] [--tc-active | --tc-shadow | --no-tc]\n");
  printf("          [--log PATH | --no-log] [--sleep]\n");
  printf("  bfr_vcu --file PATH [--profile PATH] [--tc-active | --tc-shadow | --no-tc]\n");
  printf("          [--log PATH | --no-log] [--sleep] [--seconds N]\n");
  printf("  bfr_vcu --verify-reference CSV [--profile PATH]\n");
  printf("  bfr_vcu --version\n");
}

static FILE *open_csv_log(const char *explicit_path, const char *profile_name,
                          char *resolved_path, size_t resolved_cap) {
  if (explicit_path && explicit_path[0]) {
    snprintf(resolved_path, resolved_cap, "%s", explicit_path);
  } else {
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char ts[32] = "00000000_000000";
    if (tm_now) {
      strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", tm_now);
    }
#ifdef _WIN32
    char exe_dir[MAX_PATH] = ".";
    DWORD n = GetModuleFileNameA(NULL, exe_dir, (DWORD)sizeof exe_dir);
    if (n > 0 && n < sizeof exe_dir) {
      char *slash = strrchr(exe_dir, '\\');
      char *fslash = strrchr(exe_dir, '/');
      if (fslash && (!slash || fslash > slash)) slash = fslash;
      if (slash) *slash = '\0';
      else snprintf(exe_dir, sizeof exe_dir, ".");
    }
    char log_dir[MAX_PATH + 16];
    snprintf(log_dir, sizeof log_dir, "%s\\logs", exe_dir);
    CreateDirectoryA(log_dir, NULL);
    snprintf(resolved_path, resolved_cap, "%s\\vcu_%s_%s.csv", log_dir, profile_name, ts);
#else
    (void)mkdir("logs", 0755);
    snprintf(resolved_path, resolved_cap, "logs/vcu_%s_%s.csv", profile_name, ts);
#endif
  }

  FILE *fp = fopen(resolved_path, "w");
  if (!fp) {
    perror(resolved_path);
    resolved_path[0] = '\0';
    return NULL;
  }
  fprintf(fp,
          "time_s,vcu_ticks,profile,hash,tc_mode,gear,gas,brake,rpm,speed_kph,"
          "w_fl,w_fr,w_rl,w_rr,driver_nm,cap_nm,tc_nm,cmd_nm,slip_ratio,tc_active\n");
  fflush(fp);
  return fp;
}

static void log_csv_row(FILE *fp, double elapsed_s, uint32_t vcu_ticks,
                        const SimProfile *prof, const char *tc_tag, int32_t gear,
                        float gas, float brake, float rpm, float speed_mps,
                        const float w[4], float driver_nm, float cap_nm,
                        float tc_nm, float cmd_nm, float slip_ratio,
                        uint32_t tc_active) {
  if (!fp) return;
  fprintf(fp,
          "%.3f,%u,%s,%08x,%s,%d,%.4f,%.4f,%.1f,%.2f,"
          "%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.1f,%.4f,%u\n",
          elapsed_s, vcu_ticks, prof->name, prof->hash, tc_tag, gear,
          gas, brake, rpm, speed_mps * 3.6f,
          w[0], w[1], w[2], w[3], driver_nm, cap_nm, tc_nm, cmd_nm,
          slip_ratio, tc_active);
}

static void pause_on_windows_error(int dragged_profile) {
#ifdef _WIN32
  if (dragged_profile && _isatty(_fileno(stdin))) {
    printf("\nPress Enter to exit...");
    fflush(stdout);
    (void)getchar();
  }
#else
  (void)dragged_profile;
#endif
}

int main(int argc, char **argv) {
  const char *profile_path = NULL;
  const char *file_path = NULL;
  const char *verify_csv = NULL;
  const char *log_path = NULL;
  TcMode tc_mode = TC_MODE_SHADOW;
  int no_log = 0;
  int sleep_ms = 0;
  int dragged_profile = 0;
  double run_for = 0.0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage();
      return 0;
    } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
      printf("bfr_vcu v%s (mk12-vcu %s)\n", SIL_VERSION, VCU_FW_REV);
      return 0;
    } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
      profile_path = argv[++i];
    } else if (strcmp(argv[i], "--tc-active") == 0) {
      tc_mode = TC_MODE_ACTIVE;
    } else if (strcmp(argv[i], "--tc-shadow") == 0) {
      tc_mode = TC_MODE_SHADOW;
    } else if (strcmp(argv[i], "--no-tc") == 0) {
      tc_mode = TC_MODE_OFF;
    } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
      log_path = argv[++i];
      no_log = 0;
    } else if (strcmp(argv[i], "--no-log") == 0) {
      no_log = 1;
      log_path = NULL;
    } else if (strcmp(argv[i], "--sleep") == 0) {
      sleep_ms = 1;
    } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
      file_path = argv[++i];
    } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      run_for = atof(argv[++i]);
    } else if (strcmp(argv[i], "--verify-reference") == 0 && i + 1 < argc) {
      verify_csv = argv[++i];
    } else if (argv[i][0] != '-' && !profile_path) {
      /* Single positional argument: supports Windows drag-and-drop onto bfr_vcu.exe. */
      profile_path = argv[i];
      dragged_profile = 1;
    } else {
      fprintf(stderr, "unknown option: %s\n", argv[i]);
      print_usage();
      return 2;
    }
  }

  /* Load profile (base defaults if none specified) and install pedal table. */
  if (profile_path) {
    char err[256];
    if (!simProfile_loadFile(&g_profile, profile_path, err, sizeof err)) {
      fprintf(stderr, "profile error: %s\n", err);
      pause_on_windows_error(dragged_profile && !verify_csv && run_for <= 0.0);
      return 1;
    }
  } else {
    simProfile_initBase(&g_profile);
  }

  memcpy(torqueMap_table, g_profile.pedal_table, sizeof(torqueMap_table));
  if (!torqueMap_tableIsValid(torqueMap_table, TORQUE_MAP_POINTS)) {
    fprintf(stderr, "profile error: pedal table failed firmware torqueMap_tableIsValid\n");
    pause_on_windows_error(dragged_profile && !verify_csv && run_for <= 0.0);
    return 1;
  }

  if (verify_csv) {
    return verify_reference_csv(verify_csv);
  }

#if !TC_ENABLED
  if (tc_mode == TC_MODE_ACTIVE) {
    fprintf(stderr, "warning: built with TC_ENABLED=0; staying in shadow mode\n");
    tc_mode = TC_MODE_SHADOW;
  }
#endif

  signal(SIGINT, on_signal);
#ifdef SIGTERM
  signal(SIGTERM, on_signal);
#endif
#ifdef _WIN32
  SetConsoleCtrlHandler(on_win_ctrl, TRUE);
  if (sleep_ms) timeBeginPeriod(1);
#endif

  volatile bridge_block *b = open_block(file_path);
  if (!b) {
#ifdef _WIN32
    if (sleep_ms) timeEndPeriod(1);
    g_exited = 1;
#endif
    return 1;
  }

  TorqueFilter filter;
  torqueMap_filterReset(&filter);
  tractionControl_init();

  uint32_t static_flags = 0u;
  const char *tc_mode_str = "SHADOW (telemetry only)";
  const char *tc_tag = "shadow";
  if (tc_mode == TC_MODE_OFF) {
    static_flags |= BRIDGE_FLAG_TC_BYPASSED;
    tc_mode_str = "OFF (--no-tc)";
    tc_tag = "off";
  } else if (tc_mode == TC_MODE_SHADOW) {
    static_flags |= BRIDGE_FLAG_TC_SHADOW;
  } else {
    tc_mode_str = "ACTIVE (--tc-active)";
    tc_tag = "active";
  }

  /* Auto-log 100 Hz CSV on live sessions (no --file), or whenever --log PATH is given. */
  char resolved_log[512] = "";
  FILE *log_fp = NULL;
  if (!no_log && (log_path != NULL || file_path == NULL)) {
    log_fp = open_csv_log(log_path, g_profile.name, resolved_log, sizeof resolved_log);
  }

  printf("bfr_vcu v%s (fw %s) | bridge %s v%u | %s\n",
         SIL_VERSION, VCU_FW_REV,
         BRIDGE_NAME, BRIDGE_VERSION, sleep_ms ? "1 ms sleep" : "spinning");
  printf("profile: %s (hash %08x) | max %.0f Nm | regen %s | TC %s\n",
         g_profile.name, g_profile.hash, g_profile.max_nm,
         REGEN_ENABLED ? "ON" : "OFF", tc_mode_str);
  if (log_fp) {
    printf("csv log: %s (100 Hz)\n", resolved_log);
  }
  printf("Start Assetto Corsa with the bfr_mk12 SIL script. Ctrl+C to stop.\n");
  fflush(stdout);

  uint32_t last_seq = b->seq;
  uint32_t vcu_ticks = 0, answers = 0;
  int tx_ticks = 0;
  double dt_accum = 0.0;

  float gas = 0.0f, brake = 0.0f, rpm = 0.0f, speed_mps = 0.0f;
  float wheel_omega[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  int32_t gear = 0;
  int32_t prev_gear = 0;

  float latched_nm = 0.0f;
  float driver_nm = 0.0f;
  float last_cap_nm = g_profile.max_nm;
  float tc_nm = 0.0f;
  float slip_ratio = 0.0f;
  uint32_t tc_active = 0u;
  int32_t vcu_state = BRIDGE_STATE_IDLE;
  int warned_version = 0;

  double start = now_s(), report = start, last_answer = 0.0;

  while (!g_stop) {
    uint32_t s1 = b->seq;
    BARRIER();
    uint32_t magic = b->magic;
    uint32_t version = b->version;
    float in_dt = b->dt;
    float in_gas = b->gas;
    float in_brake = b->brake;
    float in_rpm = b->rpm;
    int32_t in_gear = b->gear;
    float in_w[4] = {
      b->wheel_omega[0], b->wheel_omega[1], b->wheel_omega[2], b->wheel_omega[3]
    };
    float in_speed = b->speed_mps;
    BARRIER();
    uint32_t s2 = b->seq;

    if (s1 == s2 && s1 != last_seq && magic == BRIDGE_MAGIC) {
      if (version != BRIDGE_VERSION && !warned_version) {
        printf("warning: car script bridge v%u, bfr_vcu expects v%u\n",
               version, BRIDGE_VERSION);
        fflush(stdout);
        warned_version = 1;
      }

      gas = in_gas;
      brake = in_brake;
      rpm = in_rpm;
      gear = in_gear;
      speed_mps = in_speed;
      memcpy(wheel_omega, in_w, sizeof wheel_omega);

      slip_ratio = compute_slip_ratio(wheel_omega, speed_mps);

      if (gear != 1) {
        torqueMap_filterReset(&filter);
        dt_accum = 0.0;
        tx_ticks = 0;
        latched_nm = 0.0f;
        driver_nm = 0.0f;
        last_cap_nm = g_profile.max_nm;
        tc_nm = 0.0f;
        tc_active = 0u;
        vcu_state = BRIDGE_STATE_IDLE;
        if (prev_gear == 1) {
          log_csv_row(log_fp, now_s() - start, vcu_ticks, &g_profile, tc_tag,
                      gear, gas, brake, rpm, speed_mps, wheel_omega,
                      driver_nm, last_cap_nm, tc_nm, latched_nm, slip_ratio, tc_active);
        }
      } else {
        vcu_state = BRIDGE_STATE_DRIVE;
        float clamped_dt = (in_dt == in_dt && in_dt > 0.0f) ? in_dt : 0.0f;
        if (clamped_dt > MAX_TICK_DT_S) clamped_dt = MAX_TICK_DT_S;
        dt_accum += (double)clamped_dt;

        while (dt_accum >= VCU_DT_S) {
          dt_accum -= VCU_DT_S;
          vcu_ticks++;

          float carspeed_kph = torqueMap_carspeedKph(clamp_rpm_i16(rpm));
          float cap_nm = powerLimit_capNm(rpm);
          last_cap_nm = cap_nm;
          float req_nm = torqueMap_requestLimited(
              &filter, false, gas, brake, carspeed_kph, cap_nm, (float)VCU_DT_S);
          driver_nm = filter.target;

          if (tc_mode != TC_MODE_OFF) {
            float after_tc = tractionControl_applyWheels(
                req_nm, wheel_omega[0], wheel_omega[1], wheel_omega[2], wheel_omega[3]);
            tc_nm = tractionControl_modelTorqueNm();
            slip_ratio = tractionControl_slipRatio();
            tc_active = (after_tc < req_nm - 1e-4f) ? 1u : 0u;
            if (tc_mode == TC_MODE_ACTIVE) {
              req_nm = after_tc;
            }
          } else {
            tc_nm = req_nm;
            tc_active = 0u;
          }

          tx_ticks++;
          if (tx_ticks >= TORQUE_TX_DIVIDER) {
            tx_ticks = 0;
            latched_nm = (float)deci_nm(req_nm) * 0.1f;
            log_csv_row(log_fp, now_s() - start, vcu_ticks, &g_profile, tc_tag,
                        gear, gas, brake, rpm, speed_mps, wheel_omega,
                        driver_nm, last_cap_nm, tc_nm, latched_nm, slip_ratio, tc_active);
          }
        }
      }
      prev_gear = gear;

      b->torque_nm = latched_nm;
      b->vcu_state = vcu_state;
      b->fault_bits = static_flags;
      b->vcu_ticks = vcu_ticks;
      b->driver_nm = driver_nm;
      b->tc_nm = tc_nm;
      b->slip_ratio = slip_ratio;
      b->tc_active = tc_active;
      b->profile_hash = g_profile.hash;
      BARRIER();
      b->seq_echo = s1;

      last_seq = s1;
      answers++;
      last_answer = now_s();
    } else {
      idle(sleep_ms);
    }

    double t = now_s();
    if (t - report >= 1.0) {
      const char *link = (last_answer > 0.0 && t - last_answer < 0.05) ? "LINKED " : "waiting";
      printf("%s | %4.0f Hz | gas %.2f brk %.2f rpm %5.0f g%d | drv %5.1f tc %5.1f cmd %5.1f Nm | slip %+.2f\n",
             link, answers / (t - report), gas, brake, rpm, gear,
             driver_nm, tc_nm, latched_nm, slip_ratio);
      fflush(stdout);
      if (log_fp) fflush(log_fp);
      answers = 0;
      report = t;
    }
    if (run_for > 0.0 && t - start >= run_for) break;
  }

  if (log_fp) {
    fflush(log_fp);
    fclose(log_fp);
  }
#ifdef _WIN32
  if (sleep_ms) timeEndPeriod(1);
  g_exited = 1;
#endif
  return 0;
}
