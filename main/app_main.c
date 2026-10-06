// Entry point: start the SDK, then serve the line shell on the chip's USB
// port. The same USB port carries JTAG for GDB through OpenOCD.
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sfp.h"
#include "sfp_shell.h"

static void shell_task(void *arg) {
    (void)arg;
    static char line[128], out[1024];
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        sfp_shell_handle(line, out, sizeof out);
        printf("%s\n", out);
        fflush(stdout);
    }
}

void app_main(void) {
    // blocking, line-buffered stdin over USB Serial/JTAG
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&usb);
    esp_vfs_usb_serial_jtag_use_driver();
    esp_vfs_dev_usb_serial_jtag_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    esp_vfs_dev_usb_serial_jtag_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    setvbuf(stdin, NULL, _IONBF, 0);

    sfp_init(NULL);
    xTaskCreate(shell_task, "shell", 4096, NULL, 5, NULL);
}
