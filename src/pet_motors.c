/*
 * src/pet_motors.c -- Digital pet motor abstraction implementation.
 *
 * Maps the high-level pet vocabulary (direction / amplitude / speed) onto the
 * motor_pwm driver API:
 *
 *   - head / body / tail : stepper motors via the "pwm_RoHS" driver,
 *     position-controlled in degrees around a 90deg center.
 *   - fan                : DC motor via the "pwm_gpio" driver, velocity
 *     controlled 0..100 (duty cycle), running on its own driver thread.
 *
 * When built without PET_HAVE_MOTORS (e.g. on a host without libgpiod) the
 * hardware calls are replaced by log statements so the MQTT logic can still be
 * built and exercised.
 */

#include "pet_motors.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef PET_HAVE_MOTORS
#include "motor.h"
#endif

/* --- Geometry / tuning ----------------------------------------------------- */

#define PET_CENTER_ANGLE 90 /* neutral position, degrees */

/* Off-center offset (degrees) per amplitude. Kept within +-30 so the target
 * stays inside the stepper's [60,120] travel range (constant_range = 60). */
static const int PET_AMP_OFFSET[3] = {
    [PET_AMP_SMALL] = 10,
    [PET_AMP_MEDIUM] = 20,
    [PET_AMP_LARGE] = 30,
};

/* Stepper velocity (driver clamps to 1..10). */
static const int PET_STEP_SPEED[3] = {
    [PET_SPEED_SLOW] = 1,
    [PET_SPEED_MEDIUM] = 2,
    [PET_SPEED_FAST] = 3,
};

/* Fan duty (0..100) per speed tier. */
static const int PET_FAN_DUTY[4] = {
    [PET_FAN_OFF] = 0,
    [PET_FAN_SLOW] = 33,
    [PET_FAN_MEDIUM] = 66,
    [PET_FAN_FAST] = 100,
};

#ifdef PET_HAVE_MOTORS

/*
 * Stepper configuration, mirroring the pwm_RoHS driver's private info layout.
 * GPIO assignments follow motor_pwm/tests/test_motor_pwm.c:
 *   motor 1 (head): step=46 dir=43 enable=42 stop=83
 *   motor 2 (body): step=34 dir=35 enable=36 stop=82
 *   motor 3 (tail): step=37 dir=38 enable=39 stop=61
 */
struct pet_RoHS_info {
  uint8_t motor_index;
  uint8_t step_gpio;
  uint8_t dir_gpio;
  uint8_t enable_gpio;
  uint8_t stop_gpio;

  int current_position;
  int constant_range;
  int gpio_max_steps;

  bool enable_gpio_level;
  bool dir_gpio_left_level;
  bool stop_gpio_active_level;

  int range_steps;
};

struct pet_pwm_gpio_info {
  uint32_t period;     /* us */
  uint32_t duty_cycle; /* us */
};

#define PET_FAN_GPIO 72
#define PET_FAN_PERIOD_US 100000

static struct pet_RoHS_info g_stepper_info[3] = {
    [PET_PART_HEAD] = {.motor_index = 1, .step_gpio = 46, .dir_gpio = 43,
                       .enable_gpio = 42, .stop_gpio = 83, .current_position = -1,
                       .constant_range = 60, .gpio_max_steps = -1},
    [PET_PART_BODY] = {.motor_index = 2, .step_gpio = 34, .dir_gpio = 35,
                       .enable_gpio = 36, .stop_gpio = 82, .current_position = -1,
                       .constant_range = 60, .gpio_max_steps = -1},
    [PET_PART_TAIL] = {.motor_index = 3, .step_gpio = 37, .dir_gpio = 38,
                       .enable_gpio = 39, .stop_gpio = 61, .current_position = -1,
                       .constant_range = 60, .gpio_max_steps = -1},
};

static struct motor_dev *g_steppers[3] = {NULL, NULL, NULL};
static struct motor_dev *g_fan = NULL;

/* Drive a stepper to an absolute angle (the driver clamps to its range). */
static void stepper_goto(pet_part_t part, int angle, pet_speed_t speed) {
  if (part > PET_PART_TAIL || !g_steppers[part]) return;
  struct motor_cmd cmd = {
      .mode = MOTOR_MODE_POS,
      .pos_des = (float)angle,
      .vel_des = (float)PET_STEP_SPEED[speed],
  };
  motor_set_cmd_one(g_steppers[part], &cmd);
}

int pet_motors_init(void) {
  for (int i = 0; i < 3; i++) {
    g_steppers[i] = motor_alloc_pwm("pwm_RoHS", 0, &g_stepper_info[i]);
    if (!g_steppers[i] || motor_init_one(g_steppers[i]) < 0) {
      fprintf(stderr, "[pet] failed to init stepper %d\n", i);
      pet_motors_deinit();
      return -1;
    }
  }

  struct pet_pwm_gpio_info fan_info = {
      .period = PET_FAN_PERIOD_US,
      .duty_cycle = 0,
  };
  g_fan = motor_alloc_pwm("pwm_gpio", PET_FAN_GPIO, &fan_info);
  if (!g_fan || motor_init_one(g_fan) < 0) {
    fprintf(stderr, "[pet] failed to init fan\n");
    pet_motors_deinit();
    return -1;
  }

  /* Center everything to a known neutral pose. */
  for (int i = 0; i < 3; i++) stepper_goto((pet_part_t)i, PET_CENTER_ANGLE, PET_SPEED_MEDIUM);
  return 0;
}

void pet_motors_deinit(void) {
  pet_fan_set(PET_FAN_OFF);
  for (int i = 0; i < 3; i++) {
    if (g_steppers[i]) {
      motor_free(&g_steppers[i], 1);
      g_steppers[i] = NULL;
    }
  }
  if (g_fan) {
    motor_free(&g_fan, 1);
    g_fan = NULL;
  }
}

void pet_part_move(pet_part_t part, pet_direction_t dir, pet_amplitude_t amp,
                   pet_speed_t speed) {
  int offset = PET_AMP_OFFSET[amp];
  /* Lower angle = left (matches the driver's turn-left branch). */
  int angle = (dir == PET_DIR_LEFT) ? PET_CENTER_ANGLE - offset
                                    : PET_CENTER_ANGLE + offset;
  stepper_goto(part, angle, speed);
}

void pet_part_oscillate(pet_part_t part, pet_amplitude_t amp, pet_speed_t speed,
                        int count) {
  int offset = PET_AMP_OFFSET[amp];
  for (int i = 0; i < count; i++) {
    stepper_goto(part, PET_CENTER_ANGLE - offset, speed);
    stepper_goto(part, PET_CENTER_ANGLE + offset, speed);
  }
  stepper_goto(part, PET_CENTER_ANGLE, speed);
}

void pet_part_reset(pet_part_t part) {
  stepper_goto(part, PET_CENTER_ANGLE, PET_SPEED_MEDIUM);
}

void pet_fan_set(pet_fan_speed_t speed) {
  if (!g_fan) return;
  struct motor_cmd cmd = {
      .mode = (speed == PET_FAN_OFF) ? MOTOR_MODE_IDLE : MOTOR_MODE_VEL,
      .vel_des = (float)PET_FAN_DUTY[speed],
  };
  motor_set_cmd_one(g_fan, &cmd);
}

#else /* !PET_HAVE_MOTORS -- host build without libgpiod: log-only stubs. */

int pet_motors_init(void) {
  printf("[pet] motors disabled (built without PET_HAVE_MOTORS)\n");
  return 0;
}

void pet_motors_deinit(void) {}

void pet_part_move(pet_part_t part, pet_direction_t dir, pet_amplitude_t amp,
                   pet_speed_t speed) {
  int offset = PET_AMP_OFFSET[amp];
  int angle = (dir == PET_DIR_LEFT) ? PET_CENTER_ANGLE - offset
                                    : PET_CENTER_ANGLE + offset;
  printf("[pet] (stub) part %d -> %d deg @ speed %d\n", part, angle,
         PET_STEP_SPEED[speed]);
}

void pet_part_oscillate(pet_part_t part, pet_amplitude_t amp, pet_speed_t speed,
                        int count) {
  printf("[pet] (stub) part %d oscillate x%d amp=%d speed=%d\n", part, count,
         PET_AMP_OFFSET[amp], PET_STEP_SPEED[speed]);
}

void pet_part_reset(pet_part_t part) {
  printf("[pet] (stub) part %d -> center\n", part);
}

void pet_fan_set(pet_fan_speed_t speed) {
  printf("[pet] (stub) fan duty -> %d\n", PET_FAN_DUTY[speed]);
}

#endif /* PET_HAVE_MOTORS */
