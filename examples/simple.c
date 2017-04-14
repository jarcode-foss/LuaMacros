#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include <unistd.h>

#include <gmacros.h>

#define _(R, ...) ({ R _f __VA_ARGS__; _f; })

int main(int argc, char** argv) {
    gm_handle H = gm_init("/dev/input/by-path/pci-0000:00:1d.0-usb-0:1.6.3:1.0-event-kbd", NULL);
    gm_latch latch = gm_latch_new();
    gm_macro m = {
        .arg = NULL, .key = "D",
        .f = _(void, (int value, void* ignored) {
                printf("value: %d\n", value);
                if (value == 0) {
                    puts("user handler invoked!");
                    gmh_key(H, 1, "a");
                    gmh_sleep(H, 500);
                    puts("resumed!");
                    gmh_key(H, 0, "a");
                    gmh_sleep(H, 500);
                    puts("returning");
                }
            })
    };
    if (gm_register(H, &m)) {
        puts("gm_register failed");
        return EXIT_FAILURE;
    }
    gm_start(H);

    /* schedule some task */
    void f(void* ignored) {
        puts("scheduled handler invoked!");
        gmh_wait(H, latch);
        puts("returned from gmh_wait");
    }
    gm_sched(H, f, NULL);

    void n(void* ignored) {
        gmh_sleep(H, 500);
        puts("opening latch...");
        gmh_latch_open(H, latch);
    }
    gm_sched(H, n, NULL);
    
    puts("Registered test handler for 'D', scheduled some tasks, and tested a latch, waiting 1 second...");
    
    sleep(1);
    puts("Stopping and closing...");
    gm_stop(H);
    gm_close(H);
    return EXIT_SUCCESS;
}
