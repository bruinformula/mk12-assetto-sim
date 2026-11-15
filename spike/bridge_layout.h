/* bridge_layout.h: the shared-memory block between the car script and the VCU
 * program. One block, opened by both sides; each side writes only its own half.
 *
 * Keep this in the same order as LAYOUT in car/spike.lua. Every field is 4 bytes,
 * so there is no padding and both sides agree without packing pragmas.
 *
 * Ordering: each writer fills its fields first and its counter last
 * (seq for the sim, seq_echo for the VCU). A reader reads the counter, then the
 * fields, then the counter again, and drops the read if the two differ.
 */
#ifndef BRIDGE_LAYOUT_H
#define BRIDGE_LAYOUT_H

#include <stdint.h>

/* CSP only lets car scripts open shared memory named AcTools.CSP.Limited.* */
#define BRIDGE_NAME    "AcTools.CSP.Limited.BFR_VCU"
#define BRIDGE_MAGIC   0x56524642u /* "BFRV" */
#define BRIDGE_VERSION 1u

typedef struct {
  /* sim -> VCU */
  uint32_t magic;      /* BRIDGE_MAGIC once the sim has opened the block */
  uint32_t version;    /* BRIDGE_VERSION */
  uint32_t seq;        /* +1 every car-script tick, written last */
  float    dt;         /* s, this tick */
  float    gas;        /* 0-1, driver's pedal as the script reads it */
  float    brake;      /* 0-1 */
  float    rpm;        /* motor rpm */
  int32_t  gear;       /* AC gear, 1 = first */

  /* VCU -> sim */
  uint32_t seq_echo;   /* seq this answer is for, written last */
  float    torque_nm;  /* motor torque command */
  int32_t  vcu_state;  /* 0 idle, 1 precharged, 2 drive, 3 CAN fault */
  uint32_t fault_bits; /* 0 in the spike */
  uint32_t vcu_ticks;  /* VCU program's own loop counter */
} bridge_block;

#endif
