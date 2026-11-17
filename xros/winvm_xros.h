// SPDX-License-Identifier: GPL-2.0-or-later
/* visionmac: the virtual PC as an app sees it that also holds the other engine (Henge's, for macOS). Plain types
   only: this header pulls in none of the engine's, whose names are the other engine's too. The functions are at
   the end of host/winvm.c; build/<platform>/libwinvm.a shows these and winmedia.h's and nothing else. */
#ifndef HV_WINVM_XROS_H
#define HV_WINVM_XROS_H

#include <stddef.h>
#include <stdint.h>

/* How a run ended, as on_exit is told. */
enum { HV_WINVM_X_ERROR = 0, HV_WINVM_X_SHUTDOWN = 1, HV_WINVM_X_HOST_STOP = 3 };

/* Builds the PC: firmware, its variables, the disk, an installer disk or NULL (read-only), a folder to share or
   NULL. installer_first: the installer disk is the one the firmware starts from even if the PC's disk holds a
   Windows (a service run; give it fresh variables). The JIT region is the app's (rx to run from, rw to write through). 0, or -1 with err filled in (and nothing
   left built). One at a time: hv_winvm_x_destroy takes it down. */
int hv_winvm_x_boot(const char *code_fd, const char *vars_fd, const char *disk, const char *installer, int installer_first,
                    const char *share_dir, const char *share_sock, int cpus, uint64_t ram_mb,
                    void *jit_rx, void *jit_rw, size_t jit_bytes, int sound, char *err, size_t errlen);
/* Runs it. on_exit(outcome) is called on the PC's own thread once it has stopped for good (a restart is not that). */
void hv_winvm_x_run(void (*on_exit)(int outcome));
const char *hv_winvm_x_exit_text(void);
void hv_winvm_x_stop(void);          /* pull the plug */
/* Takes the stopped PC down: its memory, firmware, network and display are given back and another can be built.
   From on_exit's callback (the PC's own thread, its other CPUs already ended), or not at all. */
void hv_winvm_x_destroy(void);
void hv_winvm_x_power_button(void);  /* ask Windows to shut down */

void hv_winvm_x_set_layer(void *ca_metal_layer);
int hv_winvm_x_screen(uint32_t *width, uint32_t *height);
uint64_t hv_winvm_x_frames(void);
uint64_t hv_winvm_x_blocks(void);
uint64_t hv_winvm_x_net_frames(int sent); /* Ethernet frames the PC sent (1) or was sent (0) */
int hv_winvm_x_resets(void);              /* how often the PC restarted itself in this run */
int hv_winvm_x_net_up(void);              /* its network card has a network behind it */
int hv_winvm_x_cpus_on(void);
int hv_winvm_x_user_code(void);

void hv_winvm_x_key_hid(uint8_t usage, int down);
void hv_winvm_x_key_linux(uint16_t code, int down);
/* The pointer: a place (0..32767 each way), its buttons (0 left, 1 right, 2 middle), the wheel; and a step from
   where it is, for a finger on the screen used as a trackpad. Where it is, in the screen's pixels. */
void hv_winvm_x_pointer_at(int x, int y);
void hv_winvm_x_pointer_button(int button, int down);
void hv_winvm_x_pointer_wheel(int amount);
void hv_winvm_x_pointer_step(int dx, int dy);
int hv_winvm_x_pointer(int *x, int *y);

#endif
