/*
 * Copyright (c) 2016 Intel Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>


 /* I2S */
#include <string.h> // <-- FIX: Required for memcpy
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>

/* BLE */
#include <bluetooth/services/lbs.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>

LOG_MODULE_REGISTER(mixed_sample, LOG_LEVEL_INF);

// --- GPIO MACROS ---
#define LED0_NODE DT_ALIAS(ledwhite1)
#define LED1_NODE DT_ALIAS(ledwhite2)
#define LED2_NODE DT_ALIAS(ledwhite3)
#define LED3_NODE DT_ALIAS(ledwhite4)
#define SW0_NODE DT_ALIAS(sw0)

// --- AUDIO MACROS ---
#define SAMPLE_FREQ 16000
#define BLOCK_SIZE 1024
K_MEM_SLAB_DEFINE(i2s_mem_slab, BLOCK_SIZE, 4, 4);

// --- GPIO GLOBALS ---
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(LED2_NODE, gpios);
static const struct gpio_dt_spec led3 = GPIO_DT_SPEC_GET(LED3_NODE, gpios);

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;

// --- I2S GLOBALS ---
static const struct device* i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s20));
static void* audio_pattern_block;     // Stores the 500 Hz square wave pattern
static bool sound_is_playing = false; // <-- NEW: State flag

/* --- BLE CONFIGURATION (Standard) --- */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_LBS_VAL),
};

/* --- AUDIO CONTROL FUNCTION --- */
// Function to start or stop the I2S peripheral
void control_audio(bool enable) {
  if (enable && !sound_is_playing) {
    // Write the first block to load the DMA BEFORE starting the clocks
    i2s_write(i2s_dev, audio_pattern_block, BLOCK_SIZE);
    i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    sound_is_playing = true;
    LOG_INF("Audio ON (500 Hz Square Wave)");
  }
  else if (!enable && sound_is_playing) {
    i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);
    sound_is_playing = false;
    LOG_INF("Audio OFF");
  }
}

/* --- CALLBACKS --- */
// Called when the BLE client (e.g., nRF Connect) changes the LED state
static void app_led_cb(bool led_state) {
  // Set all LEDs to the received state
  gpio_pin_set_dt(&led, led_state);
  gpio_pin_set_dt(&led1, led_state);
  gpio_pin_set_dt(&led2, led_state);
  gpio_pin_set_dt(&led3, led_state);

  // Control audio based on LED state
  control_audio(led_state);

  printk("BLE Command received: Set LEDs to %d\n", led_state);
}

static struct bt_lbs_cb lbs_cb = {
    .led_cb = app_led_cb,
};

// Called when the physical button (SW0) is pressed/released
void button_pressed(const struct device* dev, struct gpio_callback* cb,
  uint32_t pins) {
  // Read the current pin state (0 or 1)
  int val = gpio_pin_get_dt(&button);

  // Set all LEDs to the button state
  gpio_pin_set_dt(&led, val);
  gpio_pin_set_dt(&led1, val);
  gpio_pin_set_dt(&led2, val);
  gpio_pin_set_dt(&led3, val);

  // Control audio based on LED state (val = 1 means ON)
  control_audio(val);

  printk("Physical Button state: %d\n", val);
}

/* --- I2S SETUP FUNCTION --- */
int configure_audio() {
  if (!device_is_ready(i2s_dev)) {
    printk("I2S device not ready\n");
    return -1;
  }

  struct i2s_config config;
  config.word_size = 16;
  config.channels = 2; // Stereo mixes to mono on MAX98357
  config.format = I2S_FMT_DATA_FORMAT_I2S;
  config.options = I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER;
  config.frame_clk_freq = SAMPLE_FREQ;
  config.mem_slab = &i2s_mem_slab;
  config.block_size = BLOCK_SIZE;
  config.timeout = 1000;

  int ret = i2s_configure(i2s_dev, I2S_DIR_TX, &config);
  if (ret < 0)
    return ret;

  // Create the "Beep" sound pattern once
  ret = k_mem_slab_alloc(&i2s_mem_slab, &audio_pattern_block, K_FOREVER);
  if (ret < 0)
    return ret;

  int16_t* buffer = (int16_t*)audio_pattern_block;
  // Square Wave Generation (500 Hz tone)
  for (int i = 0; i < (BLOCK_SIZE / 2); i++) {
    buffer[i] = (i % 32 < 16) ? 2000 : -2000;
  }

  // Set the initial state to STOPPED
  i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);
  sound_is_playing = false;

  return 0;
}

/* --- MAIN --- */
extern "C" {

  int main(void) {
    int err;

    /* --- 1. Verify & Configure GPIO --- */
    // (Verification logic omitted for brevity, assuming successful execution)
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led2, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led3, GPIO_OUTPUT_INACTIVE);

    gpio_pin_configure_dt(&button, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
    gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);

    /* --- 2. BLE Init --- */
    err = bt_lbs_init(&lbs_cb);
    err = bt_enable(NULL);
    err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

    if (err) {
      printk("BLE failed to start (err %d)\n", err);
    }
    printk("Bluetooth active. Use button or nRF Connect App.\n");

    /* --- 3. Audio Init --- */
    if (configure_audio() == 0) {
      printk("Audio system ready.\n");
    }
    else {
      printk("Audio Failed to Configure.\n");
    }

    /* --- 4. Main Loop (Audio Feeder) --- */
    while (1) {
      if (sound_is_playing) { // Only run if the audio is active
        void* next_block;

        // Allocate a fresh block
        if (k_mem_slab_alloc(&i2s_mem_slab, &next_block, K_FOREVER) == 0) {

          // Copy the tone pattern
          memcpy(next_block, audio_pattern_block, BLOCK_SIZE);

          // Send it to I2S
          i2s_write(i2s_dev, next_block, BLOCK_SIZE);
        }
      }

      // Yield execution time to other threads
      k_msleep(10);
    }
    return 0;
  }

} // End extern "C"