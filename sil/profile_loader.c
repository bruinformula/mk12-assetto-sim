/* profile_loader.c: minimal strict YAML parser & validator for MK12 VCU profiles.
 *
 * Matches the YAML subset and schema rules of vendor/mk12-vcu/profiles/schema.json
 * and tools/profile_editor/index.html. Zero external dependencies so it cross-compiles
 * statically with mingw-w64.
 */
#include "profile_loader.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t cap, const char *fmt, ...) {
  if (!err || cap == 0) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, cap, fmt, ap);
  va_end(ap);
}

/* Format a float identically to Profile Studio's num(v):
 *   Number.isInteger(v) ? String(v) : String(+v.toFixed(4))
 * Uses "%.4f", trims trailing fractional zeros and decimal point, normalizes "-0" -> "0". */
static void fmt_num(char *buf, size_t cap, float v) {
  double d = (double)v;
  if (fabs(d) < 5e-5) d = 0.0;
  snprintf(buf, cap, "%.4f", d);
  char *dot = strchr(buf, '.');
  if (dot) {
    char *end = buf + strlen(buf) - 1;
    while (end > dot && *end == '0') *end-- = '\0';
    if (end == dot) *end = '\0';
  }
  if (strcmp(buf, "-0") == 0) {
    buf[0] = '0';
    buf[1] = '\0';
  }
}

uint32_t simProfile_hash(const SimProfile *p) {
  char n_max[32], n_pt[SIM_PROFILE_POINTS][32];
  char n_ce[32], n_ds[32];
  char n_lo[32], n_flo[32], n_fhi[32], n_ramp[32];
  char n_tau[32], n_slew[32];

  fmt_num(n_max, sizeof n_max, p->max_nm);
  for (int i = 0; i < SIM_PROFILE_POINTS; i++) {
    fmt_num(n_pt[i], sizeof n_pt[i], p->pedal_table[i]);
  }
  fmt_num(n_ce, sizeof n_ce, p->coast_end);
  fmt_num(n_ds, sizeof n_ds, p->drive_start);
  fmt_num(n_lo, sizeof n_lo, p->liftoff_nm);
  fmt_num(n_flo, sizeof n_flo, p->fade_lo_kph);
  fmt_num(n_fhi, sizeof n_fhi, p->fade_hi_kph);
  fmt_num(n_ramp, sizeof n_ramp, p->ramp_nm_s);
  fmt_num(n_tau, sizeof n_tau, p->tau_s);
  fmt_num(n_slew, sizeof n_slew, p->slew_nm_s);

  char canon[1024];
  int len = snprintf(
      canon, sizeof canon,
      "{\"torque\":{\"max_nm\":%s,\"pedal_table\":[%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s]},"
      "\"pedal\":{\"coast_end\":%s,\"drive_start\":%s},"
      "\"regen\":{\"liftoff_nm\":%s,\"fade_lo_kph\":%s,\"fade_hi_kph\":%s,\"ramp_nm_s\":%s},"
      "\"filter\":{\"tau_s\":%s,\"slew_nm_s\":%s}}",
      n_max,
      n_pt[0], n_pt[1], n_pt[2], n_pt[3], n_pt[4], n_pt[5],
      n_pt[6], n_pt[7], n_pt[8], n_pt[9], n_pt[10],
      n_ce, n_ds,
      n_lo, n_flo, n_fhi, n_ramp,
      n_tau, n_slew);

  if (len < 0) return 0;
  size_t n = (size_t)len < sizeof(canon) ? (size_t)len : sizeof(canon) - 1;
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < n; i++) {
    h ^= (uint8_t)canon[i];
    h *= 0x01000193u;
  }
  return h;
}

void simProfile_initBase(SimProfile *p) {
  memset(p, 0, sizeof *p);
  snprintf(p->name, sizeof p->name, "%s", "base");
  p->max_nm = 80.0f;
  for (int i = 0; i < SIM_PROFILE_POINTS; i++) {
    p->pedal_table[i] = (float)i * 0.1f;
  }
  p->pedal_table[0] = 0.0f;
  p->pedal_table[SIM_PROFILE_POINTS - 1] = 1.0f;
  p->coast_end = 0.03f;
  p->drive_start = 0.08f;
  p->liftoff_nm = -5.0f;
  p->fade_lo_kph = 8.0f;
  p->fade_hi_kph = 20.0f;
  p->ramp_nm_s = 40.0f;
  p->tau_s = 0.05f;
  p->slew_nm_s = 2000.0f;
  p->hash = simProfile_hash(p);
}

static bool valid_name(const char *s) {
  size_t n = strlen(s);
  if (n < 1 || n > 31) return false;
  if (s[0] < 'a' || s[0] > 'z') return false;
  for (size_t i = 1; i < n; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

bool simProfile_validate(const SimProfile *p, char *err, size_t err_cap) {
  if (!valid_name(p->name)) {
    set_err(err, err_cap, "invalid profile name \"%s\" (must match ^[a-z][a-z0-9_]{0,30}$)",
            p->name);
    return false;
  }
  if (!isfinite(p->max_nm) || floorf(p->max_nm) != p->max_nm ||
      p->max_nm < 20.0f || p->max_nm > 150.0f) {
    set_err(err, err_cap, "torque.max_nm = %.4g must be an integer in [20, 150]", p->max_nm);
    return false;
  }
  for (int i = 0; i < SIM_PROFILE_POINTS; i++) {
    float v = p->pedal_table[i];
    if (!isfinite(v) || v < 0.0f || v > 1.0f) {
      set_err(err, err_cap, "torque.pedal_table[%d] = %.4g outside [0, 1]", i, v);
      return false;
    }
  }
  if (p->pedal_table[0] != 0.0f || p->pedal_table[SIM_PROFILE_POINTS - 1] != 1.0f) {
    set_err(err, err_cap, "torque.pedal_table must start at 0 and end at 1");
    return false;
  }
  for (int i = 1; i < SIM_PROFILE_POINTS; i++) {
    if (p->pedal_table[i] < p->pedal_table[i - 1]) {
      set_err(err, err_cap, "torque.pedal_table decreases at index %d (%.4g < %.4g)",
              i, p->pedal_table[i], p->pedal_table[i - 1]);
      return false;
    }
  }
  if (!isfinite(p->coast_end) || p->coast_end < 0.005f || p->coast_end > 0.1f) {
    set_err(err, err_cap, "pedal.coast_end = %.4g outside [0.005, 0.1]", p->coast_end);
    return false;
  }
  if (!isfinite(p->drive_start) || p->drive_start < 0.02f || p->drive_start > 0.2f) {
    set_err(err, err_cap, "pedal.drive_start = %.4g outside [0.02, 0.2]", p->drive_start);
    return false;
  }
  if (!(p->coast_end < p->drive_start)) {
    set_err(err, err_cap, "pedal.coast_end (%.4g) must be below pedal.drive_start (%.4g)",
            p->coast_end, p->drive_start);
    return false;
  }
  if (!isfinite(p->liftoff_nm) || p->liftoff_nm < -30.0f || p->liftoff_nm > 0.0f) {
    set_err(err, err_cap, "regen.liftoff_nm = %.4g outside [-30, 0]", p->liftoff_nm);
    return false;
  }
  if (!isfinite(p->fade_lo_kph) || p->fade_lo_kph < 0.0f || p->fade_lo_kph > 40.0f) {
    set_err(err, err_cap, "regen.fade_lo_kph = %.4g outside [0, 40]", p->fade_lo_kph);
    return false;
  }
  if (!isfinite(p->fade_hi_kph) || p->fade_hi_kph < 5.0f || p->fade_hi_kph > 60.0f) {
    set_err(err, err_cap, "regen.fade_hi_kph = %.4g outside [5, 60]", p->fade_hi_kph);
    return false;
  }
  if (!(p->fade_lo_kph < p->fade_hi_kph)) {
    set_err(err, err_cap, "regen.fade_lo_kph (%.4g) must be below regen.fade_hi_kph (%.4g)",
            p->fade_lo_kph, p->fade_hi_kph);
    return false;
  }
  if (!isfinite(p->ramp_nm_s) || p->ramp_nm_s < 5.0f || p->ramp_nm_s > 200.0f) {
    set_err(err, err_cap, "regen.ramp_nm_s = %.4g outside [5, 200]", p->ramp_nm_s);
    return false;
  }
  if (!isfinite(p->tau_s) || p->tau_s < 0.005f || p->tau_s > 0.2f) {
    set_err(err, err_cap, "filter.tau_s = %.4g outside [0.005, 0.2]", p->tau_s);
    return false;
  }
  if (!isfinite(p->slew_nm_s) || p->slew_nm_s < 200.0f || p->slew_nm_s > 10000.0f) {
    set_err(err, err_cap, "filter.slew_nm_s = %.4g outside [200, 10000]", p->slew_nm_s);
    return false;
  }
  return true;
}

static char *trim_end(char *s) {
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
  return s;
}

static void strip_comment(char *line) {
  for (size_t i = 0; line[i]; i++) {
    if (line[i] == '#' && (i == 0 || isspace((unsigned char)line[i - 1]))) {
      line[i] = '\0';
      break;
    }
  }
}

static bool parse_double_strict(const char *s, double *out) {
  while (isspace((unsigned char)*s)) s++;
  if (*s == '\0') return false;
  errno = 0;
  char *end = NULL;
  double v = strtod(s, &end);
  if (end == s || errno == ERANGE || !isfinite(v)) return false;
  while (isspace((unsigned char)*end)) end++;
  if (*end != '\0') return false;
  *out = v;
  return true;
}

static bool parse_pedal_table(const char *val, float out[SIM_PROFILE_POINTS],
                              const char *label, int lineno, char *err, size_t err_cap) {
  size_t len = strlen(val);
  if (len < 2 || val[0] != '[' || val[len - 1] != ']') {
    set_err(err, err_cap, "%s:%d: pedal_table must be a [..] list", label, lineno);
    return false;
  }
  char buf[512];
  if (len - 2 >= sizeof buf) {
    set_err(err, err_cap, "%s:%d: pedal_table line too long", label, lineno);
    return false;
  }
  memcpy(buf, val + 1, len - 2);
  buf[len - 2] = '\0';

  int count = 0;
  char *p = buf;
  for (;;) {
    char *comma = strchr(p, ',');
    if (comma) *comma = '\0';
    double v = 0.0;
    if (!parse_double_strict(p, &v)) {
      set_err(err, err_cap, "%s:%d: invalid number \"%s\" in pedal_table", label, lineno, p);
      return false;
    }
    if (count >= SIM_PROFILE_POINTS) {
      set_err(err, err_cap, "%s:%d: pedal_table has more than %d points",
              label, lineno, SIM_PROFILE_POINTS);
      return false;
    }
    out[count++] = (float)v;
    if (!comma) break;
    p = comma + 1;
  }
  if (count != SIM_PROFILE_POINTS) {
    set_err(err, err_cap, "%s:%d: pedal_table needs %d points, got %d",
            label, lineno, SIM_PROFILE_POINTS, count);
    return false;
  }
  return true;
}

enum Section { SEC_NONE = 0, SEC_TORQUE, SEC_PEDAL, SEC_REGEN, SEC_FILTER };

bool simProfile_loadString(SimProfile *p, const char *text, const char *label,
                           char *err, size_t err_cap) {
  if (!label) label = "profile";
  simProfile_initBase(p);

  /* Skip UTF-8 BOM if present (Windows editors). */
  if ((unsigned char)text[0] == 0xEF &&
      (unsigned char)text[1] == 0xBB &&
      (unsigned char)text[2] == 0xBF) {
    text += 3;
  }

  bool seen_name = false, seen_extends = false;
  char extends_val[64] = {0};
  uint32_t seen_sec = 0;
  uint32_t seen_keys = 0;
  enum Section sec = SEC_NONE;

  int lineno = 0;
  const char *cur = text;
  while (*cur) {
    lineno++;
    const char *nl = strchr(cur, '\n');
    size_t raw_len = nl ? (size_t)(nl - cur) : strlen(cur);
    if (raw_len >= 512) {
      set_err(err, err_cap, "%s:%d: line too long", label, lineno);
      return false;
    }
    char line[512];
    memcpy(line, cur, raw_len);
    line[raw_len] = '\0';
    cur = nl ? nl + 1 : cur + raw_len;

    if (raw_len > 0 && line[raw_len - 1] == '\r') line[raw_len - 1] = '\0';
    strip_comment(line);
    trim_end(line);

    char *s = line;
    size_t indent = 0;
    while (*s == ' ' || *s == '\t') { indent++; s++; }
    if (*s == '\0') continue;

    char *colon = strchr(s, ':');
    if (!colon) {
      set_err(err, err_cap, "%s:%d: expected key: value", label, lineno);
      return false;
    }
    *colon = '\0';
    char *key = trim_end(s);
    char *val = colon + 1;
    while (*val == ' ' || *val == '\t') val++;
    trim_end(val);

    if (indent == 0) {
      if (*val == '\0') {
        if (strcmp(key, "torque") == 0) sec = SEC_TORQUE;
        else if (strcmp(key, "pedal") == 0) sec = SEC_PEDAL;
        else if (strcmp(key, "regen") == 0) sec = SEC_REGEN;
        else if (strcmp(key, "filter") == 0) sec = SEC_FILTER;
        else {
          set_err(err, err_cap, "%s:%d: unknown section \"%s\"", label, lineno, key);
          return false;
        }
        if (seen_sec & (1u << sec)) {
          set_err(err, err_cap, "%s:%d: duplicate section \"%s\"", label, lineno, key);
          return false;
        }
        seen_sec |= (1u << sec);
      } else {
        sec = SEC_NONE;
        if (strcmp(key, "name") == 0) {
          if (seen_name) {
            set_err(err, err_cap, "%s:%d: duplicate \"name\"", label, lineno);
            return false;
          }
          if (strlen(val) >= sizeof p->name) {
            set_err(err, err_cap, "%s:%d: profile name too long", label, lineno);
            return false;
          }
          snprintf(p->name, sizeof p->name, "%s", val);
          seen_name = true;
        } else if (strcmp(key, "extends") == 0) {
          if (seen_extends) {
            set_err(err, err_cap, "%s:%d: duplicate \"extends\"", label, lineno);
            return false;
          }
          snprintf(extends_val, sizeof extends_val, "%s", val);
          seen_extends = true;
        } else {
          set_err(err, err_cap, "%s:%d: unknown top-level key \"%s\"", label, lineno, key);
          return false;
        }
      }
      continue;
    }

    if (sec == SEC_NONE) {
      set_err(err, err_cap, "%s:%d: \"%s\" is not inside a section", label, lineno, key);
      return false;
    }

    if (sec == SEC_TORQUE && strcmp(key, "pedal_table") == 0) {
      if (seen_keys & (1u << 0)) {
        set_err(err, err_cap, "%s:%d: duplicate torque.pedal_table", label, lineno);
        return false;
      }
      seen_keys |= (1u << 0);
      if (!parse_pedal_table(val, p->pedal_table, label, lineno, err, err_cap)) return false;
      continue;
    }

    int bit = -1;
    float *target = NULL;
    bool require_int = false;
    if (sec == SEC_TORQUE && strcmp(key, "max_nm") == 0) {
      bit = 1; target = &p->max_nm; require_int = true;
    } else if (sec == SEC_PEDAL && strcmp(key, "coast_end") == 0) {
      bit = 2; target = &p->coast_end;
    } else if (sec == SEC_PEDAL && strcmp(key, "drive_start") == 0) {
      bit = 3; target = &p->drive_start;
    } else if (sec == SEC_REGEN && strcmp(key, "liftoff_nm") == 0) {
      bit = 4; target = &p->liftoff_nm;
    } else if (sec == SEC_REGEN && strcmp(key, "fade_lo_kph") == 0) {
      bit = 5; target = &p->fade_lo_kph;
    } else if (sec == SEC_REGEN && strcmp(key, "fade_hi_kph") == 0) {
      bit = 6; target = &p->fade_hi_kph;
    } else if (sec == SEC_REGEN && strcmp(key, "ramp_nm_s") == 0) {
      bit = 7; target = &p->ramp_nm_s;
    } else if (sec == SEC_FILTER && strcmp(key, "tau_s") == 0) {
      bit = 8; target = &p->tau_s;
    } else if (sec == SEC_FILTER && strcmp(key, "slew_nm_s") == 0) {
      bit = 9; target = &p->slew_nm_s;
    } else {
      set_err(err, err_cap, "%s:%d: unknown key \"%s\"", label, lineno, key);
      return false;
    }

    if (seen_keys & (1u << bit)) {
      set_err(err, err_cap, "%s:%d: duplicate key \"%s\"", label, lineno, key);
      return false;
    }
    seen_keys |= (1u << bit);

    double num = 0.0;
    if (!parse_double_strict(val, &num)) {
      set_err(err, err_cap, "%s:%d: \"%s\" is not a finite number", label, lineno, val);
      return false;
    }
    if (require_int && floor(num) != num) {
      set_err(err, err_cap, "%s:%d: %s must be a whole number", label, lineno, key);
      return false;
    }
    *target = (float)num;
  }

  if (!seen_name) {
    set_err(err, err_cap, "%s: missing required \"name:\" field", label);
    return false;
  }
  if (strcmp(p->name, "base") == 0) {
    if (seen_extends) {
      set_err(err, err_cap, "%s: base profile cannot set extends", label);
      return false;
    }
  } else if (seen_extends && strcmp(extends_val, "base") != 0) {
    set_err(err, err_cap, "%s: extends must be \"base\" (got \"%s\")", label, extends_val);
    return false;
  }

  if (!simProfile_validate(p, err, err_cap)) return false;
  p->hash = simProfile_hash(p);
  return true;
}

bool simProfile_loadFile(SimProfile *p, const char *path, char *err, size_t err_cap) {
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    set_err(err, err_cap, "cannot open %s: %s", path, strerror(errno));
    return false;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    set_err(err, err_cap, "cannot seek %s", path);
    return false;
  }
  long sz = ftell(fp);
  if (sz < 0 || sz > 65536) {
    fclose(fp);
    set_err(err, err_cap, "%s: invalid file size (%ld bytes)", path, sz);
    return false;
  }
  rewind(fp);
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(fp);
    set_err(err, err_cap, "out of memory reading %s", path);
    return false;
  }
  size_t rd = fread(buf, 1, (size_t)sz, fp);
  fclose(fp);
  buf[rd] = '\0';

  bool ok = simProfile_loadString(p, buf, path, err, err_cap);
  free(buf);
  return ok;
}
