/* profile_loader.h: runtime YAML profile loader and FNV-1a hash for bfr_vcu.
 *
 * Parses the YAML subset exported by MK12 Profile Studio and vendor/mk12-vcu/profiles/,
 * merges overrides onto base.yaml defaults, validates schema bounds and pedal-table
 * monotonicity, and computes the 32-bit FNV-1a hash shown in Profile Studio and Lua Debug.
 */
#ifndef SIL_PROFILE_LOADER_H
#define SIL_PROFILE_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIM_PROFILE_POINTS 11
#define SIM_PROFILE_NAME_MAX 32

typedef struct {
  char     name[SIM_PROFILE_NAME_MAX];
  float    max_nm;                          /* MAX_TORQUE [20, 150], integer */
  float    pedal_table[SIM_PROFILE_POINTS]; /* torqueMap_table [0, 1], monotonic, 0..1 */
  float    coast_end;                       /* REGEN_END_PERCENT [0.005, 0.1] */
  float    drive_start;                     /* APPS_INFLECTION_PERCENT [0.02, 0.2] */
  float    liftoff_nm;                      /* REGEN_LIFTOFF_TORQUE [-30, 0] */
  float    fade_lo_kph;                     /* REGEN_FADE_LO_KPH [0, 40] */
  float    fade_hi_kph;                     /* REGEN_FADE_HI_KPH [5, 60] */
  float    ramp_nm_s;                       /* REGEN_RAMP_NM_S [5, 200] */
  float    tau_s;                           /* RC_TIME_CONSTANT [0.005, 0.2] */
  float    slew_nm_s;                       /* SLEW_RATE_LIMIT [200, 10000] */
  uint32_t hash;                            /* FNV-1a 32-bit hash of resolved tunables */
} SimProfile;

/* Populate p with vendor/mk12-vcu/profiles/base.yaml defaults and compute p->hash. */
void simProfile_initBase(SimProfile *p);

/* Compute the FNV-1a 32-bit hash of p's resolved tunables (excluding name). */
uint32_t simProfile_hash(const SimProfile *p);

/* Validate all schema bounds, cross-field rules, and pedal_table monotonicity. */
bool simProfile_validate(const SimProfile *p, char *err, size_t err_cap);

/* Parse a profile YAML string on top of base defaults, validate, and compute p->hash.
 * Returns true on success; on failure writes a diagnostic into err and returns false. */
bool simProfile_loadString(SimProfile *p, const char *text, const char *label,
                           char *err, size_t err_cap);

/* Read and parse a profile YAML file from disk. */
bool simProfile_loadFile(SimProfile *p, const char *path, char *err, size_t err_cap);

#endif /* SIL_PROFILE_LOADER_H */
