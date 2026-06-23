/*
 * src/pet_motors.h -- Digital pet motor abstraction.
 *
 * Hides the motor_pwm driver details behind a small, intent-oriented API
 * expressed in the same vocabulary as device-spec.json (direction /
 * amplitude / speed). The head, body and tail are stepper motors
 * (pwm_RoHS), the fan is a DC motor driven through a PWM GPIO (pwm_gpio).
 *
 * All motion calls are synchronous: stepper moves block until the motor
 * reaches its target; the fan runs on its own driver thread and returns
 * immediately.
 */

#ifndef PET_MOTORS_H
#define PET_MOTORS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The three movable stepper-driven parts. */
typedef enum {
  PET_PART_HEAD = 0,
  PET_PART_BODY = 1,
  PET_PART_TAIL = 2,
} pet_part_t;

typedef enum {
  PET_DIR_LEFT = 0,
  PET_DIR_RIGHT,
} pet_direction_t;

typedef enum {
  PET_AMP_SMALL = 0,
  PET_AMP_MEDIUM,
  PET_AMP_LARGE,
} pet_amplitude_t;

typedef enum {
  PET_SPEED_SLOW = 0,
  PET_SPEED_MEDIUM,
  PET_SPEED_FAST,
} pet_speed_t;

typedef enum {
  PET_FAN_OFF = 0,
  PET_FAN_SLOW,
  PET_FAN_MEDIUM,
  PET_FAN_FAST,
} pet_fan_speed_t;

/**
 * Allocate and initialize every motor (3 steppers + fan).
 * Safe to call once at startup. Returns 0 on success, -1 on failure.
 * On failure no motor is left allocated.
 */
int pet_motors_init(void);

/** Release every motor. Stops the fan and disables the steppers. */
void pet_motors_deinit(void);

/**
 * Move a part to a held tilt/turn/swing pose, off-center in @dir by an
 * offset selected by @amp, travelling at @speed. The part stays there
 * until the next move or pet_part_reset().
 */
void pet_part_move(pet_part_t part, pet_direction_t dir, pet_amplitude_t amp,
                   pet_speed_t speed);

/**
 * Oscillate a part @count times (shake / wag): swing to one side, then the
 * other, @count round-trips, finishing centered. @count of 0 simply
 * centers the part. @amp/@speed control reach and pace.
 */
void pet_part_oscillate(pet_part_t part, pet_amplitude_t amp, pet_speed_t speed,
                        int count);

/** Return a part to its centered (neutral) position. */
void pet_part_reset(pet_part_t part);

/** Set the fan speed; PET_FAN_OFF stops it. */
void pet_fan_set(pet_fan_speed_t speed);

#ifdef __cplusplus
}
#endif

#endif /* PET_MOTORS_H */
