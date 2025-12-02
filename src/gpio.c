#include "gpio.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

// --------------------------------------------------------------
// Global GPIO base and LED configuration
// --------------------------------------------------------------
volatile uint32_t *gpio = NULL;
static int gpio_fd = -1;

// Define LED GPIO lines once globally (shared across all modules)
// Pins correspond to: GPIO 22, GPIO 5, GPIO 6, and so on, of the
// Raspberry Pi Pinout.
// This means 4 pins on the left and 4 pins on the right.
// The corresponding physical pins are, in order:
// 11, 13, 27, 29, 31, 33, 35, 37.

const unsigned int led_lines[8] = {17, 27, 0, 5, 6, 13, 19, 26};

// --------------------------------------------------------------
// Initialization and cleanup
// --------------------------------------------------------------
void gpio_init(void) {
    // Try /dev/gpiomem first (works without root, preferred on Raspberry Pi)
    gpio_fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (gpio_fd >= 0) {
        // /dev/gpiomem maps GPIO registers directly at offset 0
        gpio = (volatile uint32_t *)mmap(
            NULL, GPIO_LEN, PROT_READ | PROT_WRITE, MAP_SHARED,
            gpio_fd, 0
        );
        if (gpio != MAP_FAILED) {
            return;  // Success with /dev/gpiomem
        }
        close(gpio_fd);
    }

    // Fall back to /dev/mem (requires root)
    gpio_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (gpio_fd < 0) {
        perror("open /dev/gpiomem and /dev/mem both failed");
        exit(1);
    }

    gpio = (volatile uint32_t *)mmap(
        NULL, GPIO_LEN, PROT_READ | PROT_WRITE, MAP_SHARED,
        gpio_fd, GPIO_BASE_ADDR
    );
    if (gpio == MAP_FAILED) {
        perror("mmap");
        close(gpio_fd);
        exit(1);
    }
}

void gpio_cleanup(void) {
    if (!gpio || gpio == MAP_FAILED)
        return;
    munmap((void *)gpio, GPIO_LEN);
    close(gpio_fd);
    gpio_fd = -1;
    gpio = NULL;
}

// --------------------------------------------------------------
// Helper functions
// --------------------------------------------------------------
void gpio_set_outputs(const unsigned int *lines, int count) {
    for (int i = 0; i < count; ++i) {
        int gpio_num = lines[i];
        volatile uint32_t *fsel = gpio + (gpio_num / 10);
        int shift = (gpio_num % 10) * 3;
        *fsel = (*fsel & ~(7 << shift)) | (1 << shift);  // set to 001 (output)
    }
}

void gpio_all_off(const unsigned int *lines, int count) {
    if (!gpio || gpio == MAP_FAILED)
        return;
    volatile uint32_t *GPCLR0 = gpio + 0x28 / 4;
    uint32_t mask = 0;
    for (int i = 0; i < count; ++i)
        mask |= (1u << lines[i]);
    *GPCLR0 = mask;
    __sync_synchronize();  // Memory barrier to ensure write completes
}
