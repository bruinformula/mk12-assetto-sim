/* bridge_layout.h: shared-memory bridge v2 between the Assetto Corsa CSP car
 * script (sil/car/vcu_bridge.lua) and the real MK12 VCU executable (bfr_vcu).
 *
 * One block is opened by both sides; each side writes only its own half.
 * Keep this in the exact same order as LAYOUT in sil/car/vcu_bridge.lua.
 * Every field is 4 bytes (26 x 4 = 104 bytes), so there is no padding and
 * both sides agree without packing pragmas.
 *
 * Lock-free ordering: each writer fills its payload fields first and its
 * sequence counter last (seq for the sim, seq_echo for the VCU). A reader
 * reads the counter, then the fields, then the counter again, and drops any
 * read where the two counter reads differ.
 */
#ifndef SIL_BRIDGE_LAYOUT_H
#define SIL_BRIDGE_LAYOUT_H

#include <stdint.h>

/* CSP only lets car scripts open shared memory named AcTools.CSP.Limited.* */
#define BRIDGE_NAME    "AcTools.CSP.Limited.BFR_VCU"
#define BRIDGE_MAGIC   0x56524642u /* "BFRV" */
#define BRIDGE_VERSION 2u

/* vcu_state values */
#define BRIDGE_STATE_IDLE       0
#define BRIDGE_STATE_PRECHARGED 1
#define BRIDGE_STATE_DRIVE      2
#define BRIDGE_STATE_FAULT      3

/* fault_bits / status bitmask (VCU -> sim) */
#define BRIDGE_FLAG_TC_BYPASSED (1u << 0) /* --no-tc passed on CLI */
#define BRIDGE_FLAG_TC_SHADOW   (1u << 1) /* TC running in shadow mode (telemetry only) */

typedef struct {
  /* sim -> VCU (16 x 4 = 64 bytes) */
  uint32_t magic;          /* BRIDGE_MAGIC once the sim has opened the block */
  uint32_t version;        /* BRIDGE_VERSION */
  uint32_t seq;            /* +1 every car-script tick, written last */
  float    dt;             /* s, this physics tick */
  float    gas;            /* 0-1, driver's accelerator pedal */
  float    brake;          /* 0-1, driver's brake pedal */
  float    rpm;            /* motor rpm */
  int32_t  gear;           /* AC gear: -1 = R, 0 = N, 1 = first */
  float    wheel_omega[4]; /* rad/s: [0]=FL, [1]=FR, [2]=RL, [3]=RR */
  float    speed_mps;      /* vehicle ground speed (m/s) */
  float    accel_x;        /* longitudinal accel in G (AC car.acceleration.z; reserved for SE) */
  float    accel_y;        /* lateral accel in G (AC car.acceleration.x; reserved for SE) */
  float    yaw_rate;       /* yaw rate in rad/s (AC car.localAngularVelocity.y; reserved for SE) */

  /* VCU -> sim (10 x 4 = 40 bytes) */
  uint32_t seq_echo;       /* seq this answer is for, written last */
  float    torque_nm;      /* final inverter torque command (Nm) */
  int32_t  vcu_state;      /* BRIDGE_STATE_* */
  uint32_t fault_bits;     /* BRIDGE_FLAG_* bitmask */
  uint32_t vcu_ticks;      /* VCU 1 kHz control ticks executed */
  float    driver_nm;      /* driver torque request before cap/TC (Nm) */
  float    tc_nm;          /* TC model torque output (Nm) */
  float    slip_ratio;     /* rear longitudinal slip ratio */
  uint32_t tc_active;      /* 1 if TC actively reduced torque, 0 otherwise */
  uint32_t profile_hash;   /* FNV-1a 32-bit hash of resolved tunables */
} bridge_block;

_Static_assert(sizeof(bridge_block) == 104, "bridge_block must be 104 bytes (26 x 4)");

#endif /* SIL_BRIDGE_LAYOUT_H */
