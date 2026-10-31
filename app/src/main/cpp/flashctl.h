#ifndef FLASHCTL_H
#define FLASHCTL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Return / Error codes */
#define FL_OK                0   /* Success */
#define FL_ERR_BUSY         -1   /* Camera or stock flashlight in use (device busy / CAM_ACQUIRE_DEV failed) */
#define FL_ERR_RANGE        -2   /* Brightness level out of range (must be 1..44) */
#define FL_ERR_STATE        -3   /* Invalid state (e.g. set_level/heartbeat called when torch is off) */
#define FL_ERR_IO           -4   /* Kernel ioctl or system call failure */
#define FL_ERR_COOLDOWN     -5   /* Safety or cumulative heat cooldown active; cannot turn on yet */
#define FL_ERR_NOMEM        -6   /* Memory or buffer allocation failure */
#define FL_ERR_THREAD       -7   /* Watchdog thread creation or management failure */
#define FL_ERR_UNSUPPORTED  -8   /* Hardware unsupported (subdev15 is not cam-flash) */

/**
 * struct fl_status_info - Current status snapshot of the torch hardware
 * @is_on: 1 if torch is currently active, 0 otherwise
 * @current_level: Current brightness level (1..44), or 0 if off
 * @seconds_on: Elapsed on-time in seconds during current session, or 0 if off
 * @cooldown_remaining: Seconds of cooldown remaining before fl_on can succeed
 */
struct fl_status_info {
    int is_on;
    int current_level;
    int seconds_on;
    int cooldown_remaining;
};

/**
 * fl_on - Acquire hardware, power up, configure, start, and turn on torch at specified level.
 * @level: Brightness level between 1 and 44 (0x2C).
 *
 * Verifies hardware compatibility via sysfs, allocates all required command buffers,
 * initializes the KTD2691 chip via CCI, starts the device, sets the initial brightness,
 * and spawns the monotonic safety watchdog thread.
 *
 * Returns: 0 on success, or a negative FL_ERR_* code on failure.
 */
int fl_on(int level);

/**
 * fl_set_level - Dynamically change torch brightness level while the torch is on.
 * @level: Brightness level between 1 and 44 (0x2C).
 *
 * Only valid while the torch is already ON. Re-sends the non-realtime I2C command packet
 * with the updated 0x04 register value. Also resets the watchdog inactivity timer.
 *
 * Returns: 0 on success, or a negative FL_ERR_* code on failure.
 */
int fl_set_level(int level);

/**
 * fl_off - Turn off the torch, stop and release hardware, and free all session resources.
 *
 * Dispatches explicit 0x01=0x00 off packet, stops and joins the watchdog thread,
 * issues CAM_STOP_DEV, CAM_RELEASE_DEV, frees all memory buffers, and destroys the CRM session.
 * Idempotent: safe to call repeatedly or when the torch is already off.
 *
 * Returns: 0 on success, or a negative FL_ERR_* code on failure.
 */
int fl_off(void);

/**
 * fl_heartbeat - Reset the watchdog inactivity timer.
 *
 * The watchdog will automatically shut down the torch if no heartbeat or level update
 * is received within 5 seconds.
 *
 * Returns: 0 on success, or FL_ERR_STATE if the torch is not on.
 */
int fl_heartbeat(void);

/**
 * fl_is_on - Check if the torch is currently active.
 *
 * Returns: 1 if on, 0 if off.
 */
int fl_is_on(void);

/**
 * fl_status - Retrieve a comprehensive status snapshot of the torch hardware.
 *
 * Returns: struct fl_status_info by value containing is_on, current_level,
 * seconds_on, and cooldown_remaining.
 */
struct fl_status_info fl_status(void);

/**
 * fl_max_level - Get the maximum safe verified brightness level.
 *
 * Returns: 44 (0x2C).
 */
int fl_max_level(void);

/**
 * fl_strerror - Translate a negative FL_ERR_* code into a human-readable string.
 * @err: Negative error code returned by a flashctl function.
 *
 * Returns: Pointer to a static description string.
 */
const char *fl_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* FLASHCTL_H */
