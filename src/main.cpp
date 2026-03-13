/*
 * Copyright (c) 2016 Intel Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>


 /* I2S */
#include <string.h>
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
#define TONE_FREQ_HZ 500
#define AUDIO_DIAG_ALWAYS_ON 1
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
static bool sound_is_playing = false;
static volatile bool audio_enable_requested = false;

/* --- BLE CONFIGURATION (Standard) --- */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_LBS_VAL),
};

static const struct bt_le_adv_param *adv_params = BT_LE_ADV_CONN_FAST_1;

/* --- AUDIO CONTROL FUNCTION --- */
static void request_audio_state(bool enable) {
  audio_enable_requested = enable;
}

static int i2s_submit_block(k_timeout_t alloc_timeout) {
  void* block;
  int ret = k_mem_slab_alloc(&i2s_mem_slab, &block, alloc_timeout);
  if (ret < 0) {
    // This typically means all blocks are in-flight in the I2S driver queue.
    return ret;
  }

  memcpy(block, audio_pattern_block, BLOCK_SIZE);

  ret = i2s_write(i2s_dev, block, BLOCK_SIZE);
  if (ret < 0) {
    k_mem_slab_free(&i2s_mem_slab, block);
    LOG_ERR("i2s_write failed: %d", ret);
    return ret;
  }

  return 0;
}

static int audio_start(void) {
  int ret = i2s_submit_block(K_MSEC(10));
  if (ret < 0) {
    LOG_ERR("Initial audio block alloc/write failed: %d", ret);
    return ret;
  }

  ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
  if (ret < 0) {
    LOG_ERR("I2S start failed: %d", ret);
    i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
    return ret;
  }

  sound_is_playing = true;
  LOG_INF("Audio ON (%d Hz square wave)", TONE_FREQ_HZ);
  return 0;
}

static void audio_stop(void) {
  int ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);
  if (ret < 0) {
    LOG_WRN("I2S stop failed: %d", ret);
  }

  ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
  if (ret < 0) {
    LOG_WRN("I2S drop failed: %d", ret);
  }

  sound_is_playing = false;
  LOG_INF("Audio OFF");
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
  request_audio_state(led_state);

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

    // gpio_pin_get_dt returns logical state, so active-low press reads as 1.
    request_audio_state(val > 0);

    LOG_INF("Button logical state=%d, audio_request=%d", val,
      audio_enable_requested ? 1 : 0);

  printk("Physical Button state: %d\n", val);
}

/* --- I2S SETUP FUNCTION --- */
int configure_audio() {
  if (!device_is_ready(i2s_dev)) {
    printk("I2S device not ready\n");
    return -1;
  }

  struct i2s_config config = {};
  config.word_size = 16;
  config.channels = 2; // Stereo mixes to mono on MAX98357
  config.format = I2S_FMT_DATA_FORMAT_I2S;
  config.options = I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER;
  config.frame_clk_freq = SAMPLE_FREQ;
  config.mem_slab = &i2s_mem_slab;
  config.block_size = BLOCK_SIZE;
  config.timeout = 20;

  int ret = i2s_configure(i2s_dev, I2S_DIR_TX, &config);
  if (ret < 0) {
    printk("Failed to configure I2S stream\n");
    return ret;
  }

  // Create the "Beep" sound pattern once
  ret = k_mem_slab_alloc(&i2s_mem_slab, &audio_pattern_block, K_FOREVER);
  if (ret < 0) {
    printk("Failed to allocate audio memory\n");
    return ret;
  }

  int16_t* buffer = (int16_t*)audio_pattern_block;

  // Generate interleaved stereo frames: [L, R, L, R, ...]
  // with identical left/right values for a mono-compatible stream.
  const int samples_per_channel = BLOCK_SIZE / (2 * sizeof(int16_t));
  const int half_period_samples = SAMPLE_FREQ / (2 * TONE_FREQ_HZ);
  for (int frame = 0; frame < samples_per_channel; frame++) {
    int16_t value = ((frame / half_period_samples) % 2 == 0) ? 16000 : -16000;
    buffer[2 * frame] = value;
    buffer[2 * frame + 1] = value;
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

    /* --- 3. Audio Init --- */
    if (configure_audio() == 0) {
      printk("Audio system ready.\n");
    }
    else {
      printk("Audio Failed to Configure.\n");
    }

#if AUDIO_DIAG_ALWAYS_ON
    request_audio_state(true);
    LOG_INF("AUDIO_DIAG_ALWAYS_ON enabled: forcing continuous tone");
#endif

/* --- 4. Main Loop (Audio Feeder) --- */
    while (1) {
#if AUDIO_DIAG_ALWAYS_ON
      audio_enable_requested = true;
#endif

      if (audio_enable_requested && !sound_is_playing) {
        (void)audio_start();
      }

      if (!audio_enable_requested && sound_is_playing) {
        audio_stop();
      }

      if (sound_is_playing) {
        int ret = i2s_submit_block(K_NO_WAIT);
        if (ret == -ENOMEM) {
          // Queue is full; let I2S drain and check control state again.
          k_msleep(1);
          continue;
        }

        if (ret < 0) {
          LOG_ERR("Audio stream stalled: %d", ret);
          audio_stop();
          k_msleep(50);
        }
      }
      else {
        k_msleep(20);
      }
    }
    return 0;
  }

} 