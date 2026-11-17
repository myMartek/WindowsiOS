// SPDX-License-Identifier: GPL-2.0-or-later
/* visionmac: builds the PC, runs it, stops it and takes it down, several times in one process, through the calls
   an app makes (xros/winvm_xros.h). The app could start Windows once in a launch; this is what has to hold for it
   to start again. Says after each round how far the PC got and what the process still holds.
     test_cycle CODE.fd VARS.fd ROUNDS SECONDS [DISK [png-prefix]] */
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../xros/winvm_xros.h"
int hv_winvm_save_png(const char *path);

static dispatch_semaphore_t ended;
static int last_outcome;

static void on_exit_cb(int outcome) {
    last_outcome = outcome;
    hv_winvm_x_destroy(); /* as the app does: on the PC's own thread, once it has stopped */
    dispatch_semaphore_signal(ended);
}

static double footprint_mb(void) {
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) != KERN_SUCCESS) return -1;
    return (double)vi.phys_footprint / 1048576.0;
}

int main(int argc, char **argv) {
    if (argc < 5) return fprintf(stderr, "usage: %s CODE.fd VARS.fd ROUNDS SECONDS [DISK [png-prefix]]\n", argv[0]), 2;
    int rounds = atoi(argv[3]), seconds = atoi(argv[4]), failures = 0;
    const char *disk = argc > 5 && *argv[5] ? argv[5] : NULL, *png = argc > 6 ? argv[6] : NULL;
    ended = dispatch_semaphore_create(0);
    fprintf(stderr, "cycle: before anything %.0f MB\n", footprint_mb());
    for (int i = 1; i <= rounds; i++) {
        char err[256] = "";
        if (hv_winvm_x_boot(argv[1], argv[2], disk, NULL, 0, NULL, NULL, 4, 3072, NULL, NULL, 0, 0, err, sizeof err) != 0) {
            fprintf(stderr, "cycle %d: FAIL: the PC could not be built: %s\n", i, err);
            failures++;
            break;
        }
        hv_winvm_x_run(on_exit_cb);
        sleep((unsigned)seconds);
        uint32_t w = 0, h = 0;
        int screen = hv_winvm_x_screen(&w, &h);
        uint64_t blocks = hv_winvm_x_blocks(), tx = hv_winvm_x_net_frames(1), rx = hv_winvm_x_net_frames(0);
        if (png) {
            char path[512];
            snprintf(path, sizeof path, "%s-%d.png", png, i);
            hv_winvm_save_png(path);
        }
        double running = footprint_mb();
        if (i % 2) hv_winvm_x_stop(); /* the plug, and by turns the power button, which a guest may ignore */
        else hv_winvm_x_power_button();
        if (dispatch_semaphore_wait(ended, dispatch_time(DISPATCH_TIME_NOW, 60ll * NSEC_PER_SEC)) != 0) {
            hv_winvm_x_stop();
            dispatch_semaphore_wait(ended, DISPATCH_TIME_FOREVER);
        }
        usleep(300000);
        int ok = blocks > 100000 && screen == 0 && w >= 640;
        fprintf(stderr, "cycle %d: %s: %llu blocks, screen %ux%u, net tx %llu rx %llu, outcome %d (%s); %.0f MB running, %.0f MB after\n",
                i, ok ? "ok" : "FAIL", (unsigned long long)blocks, w, h, (unsigned long long)tx, (unsigned long long)rx, last_outcome,
                hv_winvm_x_exit_text(), running, footprint_mb());
        failures += !ok;
    }
    fprintf(stderr, "cycle: %d rounds, %d failures, %.0f MB at the end\n", rounds, failures, footprint_mb());
    return failures != 0;
}
