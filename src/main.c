/*
 * Copyright (c) 2016 Intel Corporation / 2023 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>

 /* PWM*/
#include <zephyr/drivers/pwm.h>

 /* BLE */
#include <bluetooth/services/lbs.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>

// --- GPIO MACROS ---
#define LED0_NODE DT_ALIAS(ledwhite1)
#define LED1_NODE DT_ALIAS(ledwhite2)
#define LED2_NODE DT_ALIAS(ledwhite3)
#define LED3_NODE DT_ALIAS(ledwhite4)
#define SW0_NODE DT_ALIAS(sw0)
#define SW1_NODE DT_ALIAS(sw1)


// --- GPIO GLOBALS ---
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(LED2_NODE, gpios);
static const struct gpio_dt_spec led3 = GPIO_DT_SPEC_GET(LED3_NODE, gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static const struct gpio_dt_spec button1 = GPIO_DT_SPEC_GET(SW1_NODE, gpios);
static struct gpio_callback button_cb_data;
static struct gpio_callback button1_cb_data;

static struct k_work play_song_work;

// --- Melody ---
#define NOTE_GS4  415   // G#4
#define NOTE_A4   440   // A4
#define NOTE_B4   494   // B4
#define NOTE_CS5  554   // C#5
#define NOTE_DS5  622   // D#5
#define NOTE_E5   659   // E5
#define NOTE_FS5  740   // F#5
#define NOTE_GS5  830   // G#5
#define NOTE_E4   330   // E4
#define NOTE_FS4  370   // F#4
#define NOTE_DS4  311   // D#4

// Define a structure to hold each note and its duration
struct Note {
  uint32_t frequency_hz;
  uint32_t duration_ms;
};

// Jingle Bells melody array
struct Note christmas_melody[] = {
  // --- Part 1 ---
  {NOTE_GS4, 300},   // G#4
  {NOTE_A4,  300},   // A4
  {NOTE_B4,  500},   // B4
  {NOTE_B4,  500},   // B4
  {NOTE_CS5, 300},   // C#5
  {NOTE_DS5, 300},   // D#5
  {NOTE_E5,  500},   // E5
  {NOTE_E5,  500},   // E5
  {NOTE_GS4, 300},   // G#4
  {NOTE_A4,  300},   // A4
  {NOTE_B4,  500},   // B4
  {NOTE_B4,  500},   // B4
  {NOTE_CS5, 300},   // C#5
  {NOTE_B4,  300},   // B4
  {NOTE_A4,  500},   // A4
  {NOTE_A4,  500},   // A4
  {NOTE_GS4, 300},   // G#4
  {NOTE_B4,  300},   // B4
  {NOTE_E4,  300},   // E4
  {NOTE_GS4, 300},   // G#4
  {NOTE_FS4, 500},   // F#4
  {NOTE_A4,  500},   // A4
  {NOTE_DS4, 600},   // D#4
  {NOTE_E4,  600},   // E4

  // --- Part 2 (all +, octave 5) ---
  {NOTE_E5,  300},   // E5
  {NOTE_FS5, 300},   // F#5
  {NOTE_E5,  300},   // E5
  {NOTE_DS5, 300},   // D#5
  {NOTE_E5,  300},   // E5
  {NOTE_CS5, 300},   // C#5
  {NOTE_CS5, 300},   // C#5

  {NOTE_E5,  300},   // E5
  {NOTE_FS5, 300},   // F#5
  {NOTE_E5,  300},   // E5
  {NOTE_DS5, 300},   // D#5
  {NOTE_E5,  300},   // E5
  {NOTE_CS5, 300},   // C#5

  {NOTE_FS5, 300},   // F#5
  {NOTE_GS5, 300},   // G#5
  {NOTE_FS5, 300},   // F#5
  {NOTE_E5,  300},   // E5
  {NOTE_FS5, 300},   // F#5
  {NOTE_DS5, 300},   // D#5
  {NOTE_DS5, 300},   // D#5

  {NOTE_DS5, 300},   // D#5
  {NOTE_E5,  300},   // E5
  {NOTE_FS5, 300},   // F#5
  {NOTE_E5,  300},   // E5
  {NOTE_DS5, 300},   // D#5
  {NOTE_CS5, 300},   // C#5

  // --- Part 3 (no +, octave 4) ---
  {NOTE_B4,  500},   // B4
  {NOTE_A4,  500},   // A4
  {NOTE_GS4, 500},   // G#4
  {NOTE_FS4, 600},   // F#4
};


/* --- BLE CONFIGURATION --- */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_LBS_VAL),
};

static const struct bt_le_adv_param* adv_params = BT_LE_ADV_CONN_FAST_1;


// Passive Buzzer
static const struct pwm_dt_spec buzzer = PWM_DT_SPEC_GET(DT_NODELABEL(buzzer_pwm));
static const struct pwm_dt_spec buzzer_inv = PWM_DT_SPEC_GET(DT_NODELABEL(buzzer_pwm_inv));


// void play_beep(uint32_t frequency_hz, uint32_t duration_ms) {

//   if (frequency_hz == 0) {
//     pwm_set_dt(&buzzer, 0, 0);
//   }
//   else {
//     uint32_t period_ns = 1000000000 / frequency_hz;
//     uint32_t pulse_ns = period_ns / 2;
//     pwm_set_dt(&buzzer, period_ns, pulse_ns);
//   }

//   k_msleep(duration_ms);
//   pwm_set_dt(&buzzer, 0, 0);
// }
void play_beep(uint32_t frequency_hz, uint32_t duration_ms) {
  if (frequency_hz == 0) {
    pwm_set_dt(&buzzer, 0, 0);
    pwm_set_dt(&buzzer_inv, 0, 0);
  }
  else {
    uint32_t period_ns = 1000000000 / frequency_hz;
    uint32_t pulse_ns = period_ns / 2;
    pwm_set_dt(&buzzer, period_ns, pulse_ns);
    pwm_set_dt(&buzzer_inv, period_ns, pulse_ns);
  }

  k_msleep(duration_ms);

  pwm_set_dt(&buzzer, 0, 0);
  pwm_set_dt(&buzzer_inv, 0, 0);
}

void play_christmas_lights() {
  int num_notes = sizeof(christmas_melody) / sizeof(christmas_melody[0]);

  for (int i = 0; i < num_notes; i++) {
    // Play the current note
    play_beep(christmas_melody[i].frequency_hz, christmas_melody[i].duration_ms);

    // Add a tiny 50ms silence between notes so they do not blend together into one long beep
    play_beep(0, 50);
  }
}

/* --- CALLBACKS --- */
static void app_led_cb(bool led_state) {
  gpio_pin_set_dt(&led, led_state);
  gpio_pin_set_dt(&led1, led_state);
  gpio_pin_set_dt(&led2, led_state);
  gpio_pin_set_dt(&led3, led_state);
  printk("BLE Command received: Set LEDs to %d\n", led_state);
}

static struct bt_lbs_cb lbs_cb = {
    .led_cb = app_led_cb,
};

void button_pressed(const struct device* dev, struct gpio_callback* cb, uint32_t pins) {
  int val = gpio_pin_get_dt(&button);
  gpio_pin_set_dt(&led, val);
  gpio_pin_set_dt(&led1, val);
  gpio_pin_set_dt(&led2, val);
  gpio_pin_set_dt(&led3, val);
  printk("Physical Button state: %d\n", val);
}

void button_pressed1(const struct device* dev, struct gpio_callback* cb, uint32_t pins) {
  int val = gpio_pin_get_dt(&button1);
  printk("Physical Button 1 state: %d\n", val);

  // Only submit work if the button is pressed (assuming active high for this check)
  if (val == 1) {
    k_work_submit(&play_song_work);
  }
}

void play_song_handler(struct k_work* work) {
  printk("Playing Christmas Lights Song...\n");

  // /* Jin-gle bells */
  // play_beep(1319, 80);
  // k_msleep(90);
  // play_beep(1319, 80);
  // k_msleep(90);
  // play_beep(1319, 160);
  // k_msleep(170);

  // /* Jin-gle bells */
  // play_beep(1319, 80);
  // k_msleep(90);
  // play_beep(1319, 80);
  // k_msleep(90);
  // play_beep(1319, 160);
  // k_msleep(170);

  // /* Jin-gle all the way */
  // play_beep(1319, 80);
  // k_msleep(90);
  // play_beep(1568, 80);
  // k_msleep(90);
  // play_beep(1047, 80);
  // k_msleep(90);
  // play_beep(1175, 80);
  // k_msleep(90);
  // play_beep(1319, 240);
  // k_msleep(250);

  play_christmas_lights();
}

/* --- MAIN --- */
int main(void) {
  if (!pwm_is_ready_dt(&buzzer)) {
    printk("Error: Buzzer PWM device is not ready\n");
    return -1;
  }

  printk("Buzzer initialized. Starting audio loop!\n");

  k_work_init(&play_song_work, play_song_handler);


  int err;

  /* 1. Verify & Configure GPIO */
  gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
  gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
  gpio_pin_configure_dt(&led2, GPIO_OUTPUT_INACTIVE);
  gpio_pin_configure_dt(&led3, GPIO_OUTPUT_INACTIVE);

  gpio_pin_configure_dt(&button, GPIO_INPUT | (button.dt_flags & (GPIO_PULL_UP | GPIO_PULL_DOWN | GPIO_ACTIVE_LOW)));
  gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
  gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
  gpio_add_callback(button.port, &button_cb_data);

  gpio_pin_configure_dt(&button1, GPIO_INPUT | (button1.dt_flags & (GPIO_PULL_UP | GPIO_PULL_DOWN | GPIO_ACTIVE_LOW)));
  gpio_pin_interrupt_configure_dt(&button1, GPIO_INT_EDGE_BOTH);
  gpio_init_callback(&button1_cb_data, button_pressed1, BIT(button1.pin));
  gpio_add_callback(button1.port, &button1_cb_data);



  /* 2. BLE Init */
  err = bt_lbs_init(&lbs_cb);
  if (err) {
    printk("bt_lbs_init failed (err %d)\n", err);
    return err;
  }

  err = bt_enable(NULL);
  if (err) {
    printk("bt_enable failed (err %d)\n", err);
    return err;
  }

  err = bt_le_adv_start(adv_params, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
  if (err) {
    printk("BLE failed to start (err %d)\n", err);
  }
  printk("Bluetooth active. Use button or nRF Connect App.\n");


  /* 4. Main Loop */
  while (1) {


    k_msleep(1000);

  }
  return 0;
}