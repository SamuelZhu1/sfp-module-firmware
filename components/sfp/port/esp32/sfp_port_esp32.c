// ESP32-C3 platform layer: I2C master on the module's two addresses, GPIO
// interrupts on MOD_ABS / RX_LOS / TX_FAULT, a 10 Hz esp_timer for polls,
// and the one FreeRTOS task that owns the bus and the state machine.
//
// The ISR only timestamps the edge, toggles a debug pin for latency
// measurements and queues an event. Every I2C transfer happens in the owner
// task, never in an interrupt.
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sfp_internal.h"
#include "sfp_port.h"

#define PIN_SDA CONFIG_SFP_PIN_SDA
#define PIN_SCL CONFIG_SFP_PIN_SCL
#define PIN_MOD_ABS CONFIG_SFP_PIN_MOD_ABS
#define PIN_RX_LOS CONFIG_SFP_PIN_RX_LOS
#define PIN_TX_FAULT CONFIG_SFP_PIN_TX_FAULT
#define PIN_TX_DISABLE CONFIG_SFP_PIN_TX_DISABLE
#define PIN_POWER CONFIG_SFP_PIN_POWER
#define PIN_DEBUG CONFIG_SFP_PIN_DEBUG
#define PIN_SDA_HOLD CONFIG_SFP_PIN_SDA_HOLD

#define I2C_HZ 100000
#define I2C_TIMEOUT_MS 20  // a 96-byte read takes about 9 ms at 100 kHz
#define QUEUE_LEN 32
#define TASK_STACK 4096
#define TASK_PRIO 10

static const char *TAG = "sfp_port";

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev_a0, dev_a2;
static QueueHandle_t queue;
static SemaphoreHandle_t lock;
static esp_timer_handle_t poll_timer, sda_hold_timer;
static volatile int debug_level;

// ---------------- I2C ----------------

static void bus_install(void) {
    i2c_master_bus_config_t cfg = {
        .i2c_port = 0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  // the cage breakout has 4.7k too
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    i2c_device_config_t d = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .scl_speed_hz = I2C_HZ};
    d.device_address = SFP_A0;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &d, &dev_a0));
    d.device_address = SFP_A2;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &d, &dev_a2));
}

static void bus_remove(void) {
    i2c_master_bus_rm_device(dev_a0);
    i2c_master_bus_rm_device(dev_a2);
    i2c_del_master_bus(bus);
    bus = NULL;
}

sfp_port_xfer_t sfp_port_i2c_read(uint8_t dev, uint8_t offset, uint8_t *buf, size_t n) {
    i2c_master_dev_handle_t h = dev == SFP_A0 ? dev_a0 : dev_a2;
    esp_err_t err = i2c_master_transmit_receive(h, &offset, 1, buf, n, I2C_TIMEOUT_MS);
    if (err == ESP_OK) return SFP_PORT_OK;
    if (err == ESP_ERR_TIMEOUT) return SFP_PORT_TIMEOUT;
    return SFP_PORT_NACK;  // ESP_FAIL / ESP_ERR_INVALID_STATE: no ACK
}

int sfp_port_sda(void) { return gpio_get_level(PIN_SDA); }
int sfp_port_scl(void) { return gpio_get_level(PIN_SCL); }

void sfp_port_bb_begin(void) {
    // the I2C driver owns the pins; hand them over to plain GPIO first
    bus_remove();
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_SDA) | (1ULL << PIN_SCL),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    gpio_set_level(PIN_SDA, 1);
    gpio_set_level(PIN_SCL, 1);
}

void sfp_port_bb_drive(int scl_high, int sda_high) {
    gpio_set_level(PIN_SDA, sda_high);
    gpio_set_level(PIN_SCL, scl_high);
}

void sfp_port_bb_end(void) {
    gpio_reset_pin(PIN_SDA);
    gpio_reset_pin(PIN_SCL);
    bus_install();
}

// ---------------- time, pins, lock ----------------

void sfp_port_delay_us(uint32_t us) {
    // short waits spin; long ones (backoff, power-off settle) let other tasks run
    if (us >= 2000) {
        vTaskDelay(pdMS_TO_TICKS(us / 1000));
        us %= 1000;
    }
    if (us) esp_rom_delay_us(us);
}

uint64_t sfp_port_now_us(void) { return (uint64_t)esp_timer_get_time(); }

bool sfp_port_mod_present(void) { return gpio_get_level(PIN_MOD_ABS) == 0; }
bool sfp_port_rx_los(void) { return gpio_get_level(PIN_RX_LOS) == 1; }
bool sfp_port_tx_fault(void) { return gpio_get_level(PIN_TX_FAULT) == 1; }
void sfp_port_set_tx_disable(bool disable) { gpio_set_level(PIN_TX_DISABLE, disable); }
void sfp_port_set_power(bool on) { gpio_set_level(PIN_POWER, on); }

void sfp_port_lock(void) { xSemaphoreTakeRecursive(lock, portMAX_DELAY); }
void sfp_port_unlock(void) { xSemaphoreGiveRecursive(lock); }

// ---------------- events ----------------

static void IRAM_ATTR pin_isr(void *arg) {
    int pin = (int)(intptr_t)arg;
    int level = gpio_get_level(pin);
    sfp_event_t ev = {.t_us = (uint64_t)esp_timer_get_time()};
    if (pin == PIN_MOD_ABS) {
        ev.type = level ? SFP_EV_MOD_REMOVED : SFP_EV_MOD_PRESENT;
    } else if (pin == PIN_RX_LOS) {
        ev.type = level ? SFP_EV_LOS_ASSERT : SFP_EV_LOS_CLEAR;
        debug_level ^= 1;  // logic analyzer: RX_LOS edge to this edge is ISR latency
        gpio_set_level(PIN_DEBUG, debug_level);
    } else if (level) {
        ev.type = SFP_EV_TX_FAULT;
    } else {
        return;  // TX_FAULT falling edge: the recovery timer checks the pin
    }
    BaseType_t woken = pdFALSE;
    // pin events are never dropped silently: the queue is sized for bursts,
    // and poll ticks (below) give way first
    xQueueSendFromISR(queue, &ev, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void poll_cb(void *arg) {
    (void)arg;
    sfp_event_t ev = {.t_us = (uint64_t)esp_timer_get_time(), .type = SFP_EV_POLL};
    // keep a few slots free for pin edges
    if (uxQueueSpacesAvailable(queue) < 4 || xQueueSend(queue, &ev, 0) != pdTRUE) {
        sfp_core_note_dropped();
    }
}

static void owner_task(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t deadline = sfp_core_next_deadline_us();
        TickType_t wait = portMAX_DELAY;
        if (deadline != UINT64_MAX) {
            uint64_t now = sfp_port_now_us();
            wait = deadline <= now ? 0 : pdMS_TO_TICKS((deadline - now + 999) / 1000);
        }
        sfp_event_t ev;
        if (xQueueReceive(queue, &ev, wait) == pdTRUE) sfp_core_handle(&ev);
        sfp_core_service();
    }
}

static void sda_release_cb(void *arg) {
    (void)arg;
    gpio_set_level(PIN_SDA_HOLD, 1);  // open drain: high = released
}

void sfp_port_hold_sda_low(uint32_t ms) {
    gpio_set_level(PIN_SDA_HOLD, 0);
    esp_timer_stop(sda_hold_timer);
    esp_timer_start_once(sda_hold_timer, (uint64_t)ms * 1000);
}

static void pins_init(void) {
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_TX_DISABLE) | (1ULL << PIN_POWER) | (1ULL << PIN_DEBUG),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(PIN_POWER, 0);
    gpio_set_level(PIN_TX_DISABLE, 0);

    gpio_config_t hold = {
        .pin_bit_mask = 1ULL << PIN_SDA_HOLD,
        .mode = GPIO_MODE_OUTPUT_OD,
    };
    gpio_config(&hold);
    gpio_set_level(PIN_SDA_HOLD, 1);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_MOD_ABS) | (1ULL << PIN_RX_LOS) | (1ULL << PIN_TX_FAULT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // module outputs are open collector
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&in);
}

void sfp_port_init(void) {
    pins_init();
    lock = xSemaphoreCreateRecursiveMutex();
    queue = xQueueCreate(QUEUE_LEN, sizeof(sfp_event_t));
    bus_install();
}

void sfp_port_start(const sfp_config_t *cfg) {
    esp_timer_create_args_t t = {.callback = poll_cb, .name = "sfp_poll"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &poll_timer));
    esp_timer_create_args_t h = {.callback = sda_release_cb, .name = "sfp_sda_hold"};
    ESP_ERROR_CHECK(esp_timer_create(&h, &sda_hold_timer));

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    gpio_isr_handler_add(PIN_MOD_ABS, pin_isr, (void *)PIN_MOD_ABS);
    gpio_isr_handler_add(PIN_RX_LOS, pin_isr, (void *)PIN_RX_LOS);
    gpio_isr_handler_add(PIN_TX_FAULT, pin_isr, (void *)PIN_TX_FAULT);

    xTaskCreate(owner_task, "sfp", TASK_STACK, NULL, TASK_PRIO, NULL);
    ESP_ERROR_CHECK(esp_timer_start_periodic(poll_timer, (uint64_t)cfg->poll_period_ms * 1000));
    ESP_LOGI(TAG, "started, poll every %lu ms", (unsigned long)cfg->poll_period_ms);
}
