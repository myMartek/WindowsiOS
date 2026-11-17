/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "winvm.h"

#include <CoreFoundation/CoreFoundation.h>
#include <TargetConditionals.h>
#include <os/lock.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "audio_mac.h"
#include "dispatch.h"
#include "net_slirp.h"
#include "share.h"

static HvMachine m;
static HvCpu cpus[HV_MAX_CPUS];
static pthread_t sec_threads[HV_MAX_CPUS];
static int ncpus = 1;
static HvWinVirt board;
static HvDisk disk0 = {.fd = -1};
static HvDisk disk1 = {.fd = -1};
static int net_on;
static int share_on;
static _Atomic int stop_requested;
static _Atomic int reset_count; /* restarts the guest asked for in this run (visionmac: hv_winvm_x_resets) */

static os_unfair_lock display_lock = OS_UNFAIR_LOCK_INIT;
static CFTypeRef display_layer;
static int display_layer_set;
static _Atomic(HvFbDisplay *) display_fb;

static void (*exit_cb)(int reason);
/* visionmac: for hv_winvm_destroy. */
static int machine_built, handlers_installed;
static pthread_t kicker_pthread;
static int kicker_started;

int hv_winvm_share_up(void) { return share_on; }
HvMachine *hv_winvm_machine(void) { return &m; }
HvWinVirt *hv_winvm_board(void) { return &board; }

const char *hv_winvm_reason_name(int r) {
    switch (r) {
    case HV_EXIT_HALT: return "halt";
    case HV_EXIT_WFI: return "wfi with nothing to wake it";
    case HV_EXIT_UNSUPPORTED: return "unsupported instruction";
    case HV_EXIT_FAULT_LOOP: return "fault loop on exception vector";
    default: return "other";
    }
}

static void board_poll(void) {
    if (net_on) hv_net_poll();
}

static int fail(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
    return -1;
}

static struct sigaction prior_ill, prior_bus, prior_segv;
static void on_ill(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = ctx;
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    uint64_t rx = (uint64_t)(uintptr_t)m.jit.base;
    uint64_t size = (uint64_t)m.jit.capacity * 4;
    int in_rx = pc >= rx && pc < rx + size;
    if (sig == SIGILL || in_rx) {
        fprintf(stderr, "winvm: signal %d in JIT: host_pc=%llx (rx=%llx +%lld) addr=%p\n",
                sig, (unsigned long long)pc, (unsigned long long)rx, (long long)(pc - rx), info->si_addr);
        for (int i = 0; i < ncpus; i++) {
            fprintf(stderr, "  cpu %d: guest_pc=%llx el=%u sctlr=%llx\n",
                    i, (unsigned long long)cpus[i].pc, cpus[i].el, (unsigned long long)cpus[i].sctlr_el1);
        }
        fflush(stderr);
    }
    struct sigaction *prior = sig == SIGILL ? &prior_ill : sig == SIGBUS ? &prior_bus : &prior_segv;
    sigaction(sig, prior, NULL);
}

static void prepare_cpu_thread(void) {
    static __thread int done;
    if (done) return;
    done = 1;
    stack_t ss = {.ss_sp = malloc(1u << 16), .ss_size = 1u << 16, .ss_flags = 0};
    if (ss.ss_sp) sigaltstack(&ss, NULL);
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGSEGV);
    sigdelset(&set, SIGBUS);
    sigdelset(&set, SIGILL);
    sigdelset(&set, SIGTRAP);
    sigdelset(&set, SIGFPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    const char *q = getenv("HV_QOS");
#if TARGET_OS_IPHONE
    /* Schedule guest vCPUs on P-cores while staying strictly below UI (USER_INTERACTIVE) */
    qos_class_t c = QOS_CLASS_USER_INITIATED;
#else
    qos_class_t c = QOS_CLASS_USER_INTERACTIVE;
#endif
    if (q && !strcmp(q, "ui")) c = QOS_CLASS_USER_INTERACTIVE;
    else if (q && !strcmp(q, "init")) c = QOS_CLASS_USER_INITIATED;
    else if (q && !strcmp(q, "default")) c = QOS_CLASS_DEFAULT;
    if (c != QOS_CLASS_DEFAULT) pthread_set_qos_class_self_np(c, 0);
}

void hv_winvm_set_display_layer(void *layer) {
    CFTypeRef retained = layer ? CFRetain(layer) : NULL;
    os_unfair_lock_lock(&display_lock);
    CFTypeRef previous = display_layer;
    display_layer = retained;
    display_layer_set = 1;
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    if (fb) hv_fb_metal_set_layer(fb, layer);
    os_unfair_lock_unlock(&display_lock);
    if (previous) CFRelease(previous);
}

int hv_winvm_display_size(uint32_t *w, uint32_t *h) {
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    if (!fb) return -1;
    const uint8_t *px = NULL;
    uint32_t fw = 0, fh = 0, st = 0;
    if (hv_fb_display_get_frame(fb, &px, &fw, &fh, &st) != 0) return -1;
    if (w) *w = fw;
    if (h) *h = fh;
    return 0;
}

uint64_t hv_winvm_display_frames(void) {
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    return fb ? hv_fb_metal_frames(fb) : 0;
}

int hv_winvm_save_png(const char *path) {
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    return fb ? hv_fb_save_png(fb, path) : -1;
}

int hv_winvm_create(const HvWinVmConfig *c, char *err, size_t errlen) {
    if (!c || !c->code_fd || !c->vars_fd)
        return fail(err, errlen, "UEFI firmware (code.fd) and NVRAM (vars.fd) are required.");

    if (machine_built) return fail(err, errlen, "The previous PC has not been taken down.");
    atomic_store(&stop_requested, 0);
    atomic_store(&reset_count, 0);
    machine_built = 1; /* from here hv_winvm_destroy has something to undo, however far this gets */

    struct sigaction ill = {.sa_flags = SA_SIGINFO | SA_ONSTACK};
    ill.sa_sigaction = on_ill;
    sigaction(SIGILL, &ill, &prior_ill);
    sigaction(SIGBUS, &ill, &prior_bus);
    sigaction(SIGSEGV, &ill, &prior_segv);
    handlers_installed = 1;
    signal(SIGPIPE, SIG_IGN);

    HvDisk *d0_ptr = NULL, *d1_ptr = NULL;
    if (c->disk0 && *c->disk0) {
        if (hv_disk_open(&disk0, c->disk0, 0, 0) != 0)
            return fail(err, errlen, "Cannot open primary NVMe disk image.");
        d0_ptr = &disk0;
    }
    if (c->disk1 && *c->disk1) {
        /* visionmac: HV_DISK1_RW, for a service run: the second disk is the PC's own then, and has to take writes. */
        if (hv_disk_open(&disk1, c->disk1, 0, getenv("HV_DISK1_RW") ? 0 : 1) != 0)
            return fail(err, errlen, "Cannot open secondary/installer disk image.");
        d1_ptr = &disk1;
    }

    size_t jit = c->jit_bytes ? c->jit_bytes : ((size_t)768 << 20);
    int r = c->jit_rx ? hv_machine_init_jit(&m, c->jit_rx, c->jit_rw, jit) : hv_machine_init(&m, jit);
    if (r != 0) return fail(err, errlen, "Cannot initialize ARM64 JIT region (is JIT active?).");

#if TARGET_OS_IPHONE
    /* Defer synchronous guest NVMe FLUSH fsyncs on iOS APFS NAND; explicit host checkpoints call hv_disk_sync(). */
    if (!getenv("HV_FLUSH")) hv_knob[HV_K_FLUSH] = 1;
#endif

    m.log = c->log;
    m.stats_interval = c->stats_interval;
    if (getenv("HV_SLOW_MEM")) m.slow_mem = 1;
    ncpus = c->ncpus < 1 ? 1 : c->ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : c->ncpus;
    m.big_lock_on = ncpus > 1 && getenv("HV_BIG_LOCK") != NULL;

    uint64_t ram_bytes = (c->ram_mb ? c->ram_mb : 4096ULL) << 20;
    static const uint8_t mac[6] = {0x02, 0x57, 0x49, 0x4e, 0x00, 0x01};
    if (hv_winvirt_create(&board, &m, cpus, ncpus, ram_bytes, c->prealloc_ram, c->code_fd, c->vars_fd,
                          d0_ptr, d1_ptr, mac, c->enable_sound) != 0) {
        return fail(err, errlen, "Cannot create virtual ARM64 Windows board (insufficient RAM or missing firmware).");
    }
    hv_winvirt_reset_cpu(&board);

    share_on = 0;
    if (c->share_dir && *c->share_dir && c->share_sock && *c->share_sock) {
        if (hv_share_start(c->share_dir, c->share_sock, NULL) == 0) {
            hv_net_set_share_socket(c->share_sock);
            share_on = 1;
        }
    }
    if (board.net && !c->no_net) {
        if (hv_net_attach(&m, board.net) == 0) net_on = 1;
        else fprintf(stderr, "winvm: user-mode SLIRP networking unavailable\n");
    }
    if (board.hda && c->enable_sound) hv_audio_attach_hda(board.hda);
    board.poll = board_poll;

    os_unfair_lock_lock(&display_lock);
    if (display_layer_set && board.fb) hv_fb_metal_set_layer(board.fb, (void *)display_layer);
    atomic_store_explicit(&display_fb, board.fb, memory_order_release);
    os_unfair_lock_unlock(&display_lock);

    return 0;
}

static void *kicker_thread(void *unused) {
    (void)unused;
    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
#if TARGET_OS_IPHONE
        usleep(2500);
#else
        usleep(1000);
#endif
        hv_machine_kick_all(&m);
    }
    return NULL;
}

static void *secondary_thread(void *arg) {
    HvCpu *c = arg;
    prepare_cpu_thread();
    int r = hv_run(c);
    if (!c->halted) {
        fprintf(stderr, "winvm: cpu %u stopped (%s) at pc=%llx el=%u\n",
                c->index, hv_winvm_reason_name(r), (unsigned long long)c->pc, c->el);
    }
    return NULL;
}

static void spawn_secondaries(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    for (int i = 1; i < ncpus; i++) {
        pthread_create(&sec_threads[i], &attr, secondary_thread, &cpus[i]);
    }
    pthread_attr_destroy(&attr);
}

static void join_secondaries(void) {
    for (int i = 1; i < ncpus; i++) {
        cpus[i].halted = 1;
        hv_cpu_wake(&cpus[i]);
    }
    hv_machine_kick_all(&m);
    for (int i = 1; i < ncpus; i++) {
        if (sec_threads[i]) {
            pthread_join(sec_threads[i], NULL);
            sec_threads[i] = 0;
        }
    }
}

void hv_winvm_flush_disks(void) {
    if (disk0.fd >= 0) hv_disk_sync(&disk0);
}


int hv_winvm_run_cpu0(void) {
    prepare_cpu_thread();
    for (;;) {
        int r = hv_run(&cpus[0]);
        hv_winvm_flush_disks();
        /* PSCI SYSTEM_RESET sets halted == 2: perform warm reboot without tearing down RAM/JIT! */
        if (cpus[0].halted == 2 && !atomic_load(&stop_requested)) {
            fprintf(stderr, "winvm: guest requested PSCI SYSTEM_RESET — rebooting...\n");
            atomic_fetch_add(&reset_count, 1);
            join_secondaries();
            /* Reset GIC, ITS, PCIe, and pflash state for warm reboot */
            if (board.gic) {
                pthread_mutex_lock(&board.gic->mu);
                board.gic->ctlr = 0;
                memset(board.gic->enabled, 0, sizeof board.gic->enabled);
                memset(board.gic->latch, 0, sizeof board.gic->latch);
                memset(board.gic->level, 0, sizeof board.gic->level);
                memset(board.gic->active, 0, sizeof board.gic->active);
                memset(board.gic->spi_on, 0, sizeof board.gic->spi_on);
                for (int c = 0; c < HV_MAX_CPUS; c++) {
                    memset(&board.gic->cpu[c], 0, sizeof board.gic->cpu[c]);
                    board.gic->cpu[c].waker = 6;
                    for (int q = 0; q < 16; q++) board.gic->cpu[c].edge[q] = 1;
                    m.percpu[c].irq_line = 0;
                    m.percpu[c].fiq_line = 0;
                }
                pthread_mutex_unlock(&board.gic->mu);
            }
            hv_gicv3_its_reset(board.its);
            hv_pcie_reset(board.pcie);
            if (board.flash1) {
                board.flash1->cmd = 0xff;
                board.flash1->wcycle = 0;
                board.flash1->status = 0x80;
            }
            if (board.fb) board.fb->active = 0;
            hv_winvirt_reset_cpu(&board);
            m.flush_pending = 1;
            spawn_secondaries();
            continue;
        }
        return r;
    }
}

static void *cpu0_thread(void *unused) {
    (void)unused;
    int r = hv_winvm_run_cpu0();
    atomic_store(&stop_requested, 1);
    join_secondaries();
    if (exit_cb) exit_cb(r);
    return NULL;
}

void hv_winvm_start(int spawn_cpu0, void (*on_exit)(int reason)) {
    exit_cb = on_exit;
    spawn_secondaries();
    kicker_started = pthread_create(&kicker_pthread, NULL, kicker_thread, NULL) == 0;
    if (spawn_cpu0) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);
        pthread_t ct;
        pthread_create(&ct, &attr, cpu0_thread, NULL);
        pthread_detach(ct);
        pthread_attr_destroy(&attr);
    }
}

/* visionmac: takes the PC down so that another can be built in the same process (the app could start Windows only
   once in a launch). For after its CPUs have ended: from on_exit's callback, which runs on the first CPU's own
   thread once the others are joined, or after a create that failed. Order matters: everything that reads the PC's
   memory or its devices stops before they go. */
void hv_winvm_destroy(void) {
    if (!machine_built) return;
    atomic_store(&stop_requested, 1);
    if (kicker_started) {
        pthread_join(kicker_pthread, NULL);
        kicker_started = 0;
    }
    os_unfair_lock_lock(&display_lock);
    HvFbDisplay *fb = atomic_exchange_explicit(&display_fb, NULL, memory_order_acq_rel);
    os_unfair_lock_unlock(&display_lock);
    if (fb) hv_fb_metal_stop(fb);
    hv_audio_shutdown();
    if (net_on) hv_net_detach();
    net_on = 0;
    share_on = 0;
    hv_winvirt_destroy(&board);
    hv_disk_close(&disk0);
    hv_disk_close(&disk1);
    hv_machine_free(&m);
    memset(&m, 0, sizeof m);
    memset(cpus, 0, sizeof cpus);
    memset(sec_threads, 0, sizeof sec_threads);
    ncpus = 1;
    if (handlers_installed) {
        sigaction(SIGILL, &prior_ill, NULL);
        sigaction(SIGBUS, &prior_bus, NULL);
        sigaction(SIGSEGV, &prior_segv, NULL);
        handlers_installed = 0;
    }
    machine_built = 0;
    fprintf(stderr, "winvm: the PC is taken down\n");
}

void hv_winvm_stop(void) {
    atomic_store(&stop_requested, 1);
    hv_audio_shutdown();
    for (int i = 0; i < ncpus; i++) cpus[i].halted = 3;
    hv_machine_kick_all(&m);
    for (int i = 0; i < ncpus; i++) hv_cpu_wake(&cpus[i]);
    hv_winvm_flush_disks();
}

void hv_winvm_power_button(void) {
    hv_winvirt_power_button(&board);
}

void hv_winvm_key_hid(uint8_t usage, int down) {
    if (board.usb) hv_xhci_key(board.usb, usage, down);
}

void hv_winvm_key_linux(uint16_t code, int down) {
    if (board.usb) hv_xhci_key_linux(board.usb, code, down);
}

void hv_winvm_tablet(double x_norm, double y_norm, uint8_t buttons, int8_t wheel) {
    if (!board.usb) return;
    if (x_norm < 0.0) x_norm = 0.0;
    if (x_norm > 1.0) x_norm = 1.0;
    if (y_norm < 0.0) y_norm = 0.0;
    if (y_norm > 1.0) y_norm = 1.0;
    int ax = (int)(x_norm * 32767.0 + 0.5);
    int ay = (int)(y_norm * 32767.0 + 0.5);
    hv_xhci_tablet(board.usb, ax, ay, buttons, wheel);
}

void hv_winvm_send_ctrl_alt_del(void) {
    if (!board.usb) return;
    /* Left Ctrl (0xe0), Left Alt (0xe2), Delete (0x4c) */
    hv_xhci_key(board.usb, 0xe0, 1);
    hv_xhci_key(board.usb, 0xe2, 1);
    hv_xhci_key(board.usb, 0x4c, 1);
    hv_xhci_key(board.usb, 0x4c, 0);
    hv_xhci_key(board.usb, 0xe2, 0);
    hv_xhci_key(board.usb, 0xe0, 0);
}

void hv_winvm_usb_stats(int *kbd_ready, int *tab_ready, uint64_t *delivered) {
    HvXhci *x = board.usb;
    if (!x) {
        if (kbd_ready) *kbd_ready = 0;
        if (tab_ready) *tab_ready = 0;
        if (delivered) *delivered = 0;
        return;
    }
    pthread_mutex_lock(&x->mu);
    int ks = x->kbd_slot;
    int ts = x->tab_slot;
    if (kbd_ready) *kbd_ready = (ks >= 1 && ks <= HV_XHCI_SLOTS && x->slots[ks].ep_running[3]) ? 1 : 0;
    if (tab_ready) *tab_ready = (ts >= 1 && ts <= HV_XHCI_SLOTS && x->slots[ts].ep_running[3]) ? 1 : 0;
    if (delivered) *delivered = x->delivered;
    pthread_mutex_unlock(&x->mu);
}

void hv_winvm_engine_stats(uint64_t *jit_used_bytes, uint64_t *jit_cap_bytes,
                           uint64_t *jit_full_flushes, uint64_t *wfi_ticks) {
    if (jit_used_bytes) *jit_used_bytes = (uint64_t)m.jit.used * 4ull;
    if (jit_cap_bytes) *jit_cap_bytes = (uint64_t)m.jit.capacity * 4ull;
    if (jit_full_flushes) *jit_full_flushes = m.stats.flush_full;
    if (wfi_ticks) *wfi_ticks = m.stats.wfi_ticks;
}

/* ---- visionmac: what xros/winvm_xros.h declares, for an app that also holds the engine this one came from ---- */

#include "../xros/winvm_xros.h"

static void (*x_exit)(int outcome);
static char x_exit_text[96] = "";
static os_unfair_lock x_ptr_lock = OS_UNFAIR_LOCK_INIT;
static int x_ptr_x = 16384, x_ptr_y = 16384; /* 0..32767 */
static uint8_t x_ptr_buttons;

int hv_winvm_x_boot(const char *code_fd, const char *vars_fd, const char *disk, const char *installer, int installer_first,
                    const char *share_dir, const char *share_sock, int cpu_count, uint64_t ram_mb,
                    void *jit_rx, void *jit_rw, size_t jit_bytes, int sound, char *err, size_t errlen) {
    /* installer_first (a service run on a Windows that is installed): the firmware starts what it finds first, and
       with the PC's disk first that is Windows. So the two change places, and the PC's disk, now second, stays
       writable. vars_fd should then be a fresh one: the PC's own names Windows's boot manager first. */
    if (installer && installer_first) setenv("HV_DISK1_RW", "1", 1);
    else unsetenv("HV_DISK1_RW");
    HvWinVmConfig c = {.code_fd = code_fd, .vars_fd = vars_fd, .disk0 = installer && installer_first ? installer : disk,
                       .disk1 = installer && installer_first ? disk : installer, .share_dir = share_dir,
                       .share_sock = share_sock, .ncpus = cpu_count, .ram_mb = ram_mb, .jit_bytes = jit_bytes, .jit_rx = jit_rx,
                       .jit_rw = jit_rw, .enable_sound = sound, .stats_interval = getenv("HV_STATS") ? atoi(getenv("HV_STATS")) : 0};
    x_exit_text[0] = 0;
    x_ptr_buttons = 0;
    int r = hv_winvm_create(&c, err, errlen);
    if (r != 0) hv_winvm_destroy(); /* whatever it had built */
    return r;
}

void hv_winvm_x_destroy(void) { hv_winvm_destroy(); }

static void x_on_exit(int reason) {
    int halt = (int)cpus[0].halted;
    int outcome = reason == HV_EXIT_HALT && halt == 1 ? HV_WINVM_X_SHUTDOWN : reason == HV_EXIT_HALT && halt == 3 ? HV_WINVM_X_HOST_STOP : HV_WINVM_X_ERROR;
    snprintf(x_exit_text, sizeof x_exit_text, "%s", outcome == HV_WINVM_X_SHUTDOWN ? "Windows shut down" : outcome == HV_WINVM_X_HOST_STOP ? "stopped by host" : hv_winvm_reason_name(reason));
    fprintf(stderr, "winvm: the PC stopped (%s, halt %d)\n", x_exit_text, halt);
    if (x_exit) x_exit(outcome);
}

void hv_winvm_x_run(void (*on_exit)(int outcome)) {
    x_exit = on_exit;
    hv_winvm_start(1, x_on_exit);
}

const char *hv_winvm_x_exit_text(void) { return x_exit_text; }
void hv_winvm_x_stop(void) { hv_winvm_stop(); }
void hv_winvm_x_power_button(void) { hv_winvm_power_button(); }
void hv_winvm_x_set_layer(void *layer) { hv_winvm_set_display_layer(layer); }
int hv_winvm_x_screen(uint32_t *w, uint32_t *h) { return hv_winvm_display_size(w, h); }
uint64_t hv_winvm_x_frames(void) { return hv_winvm_display_frames(); }
uint64_t hv_winvm_x_blocks(void) { return m.stats.blocks; }
uint64_t hv_winvm_x_net_frames(int sent) { return sent ? hv_net_tx_frames : hv_net_rx_frames; }
int hv_winvm_x_net_up(void) { return net_on; }
int hv_winvm_x_resets(void) { return atomic_load(&reset_count); }

int hv_winvm_x_cpus_on(void) {
    int n = 0;
    for (int i = 0; i < ncpus; i++) n += !cpus[i].off;
    return n;
}

int hv_winvm_x_user_code(void) {
    for (int i = 0; i < ncpus; i++)
        if (!cpus[i].off && cpus[i].el == 0) return 1;
    return 0;
}

void hv_winvm_x_key_hid(uint8_t usage, int down) { hv_winvm_key_hid(usage, down); }
void hv_winvm_x_key_linux(uint16_t code, int down) { hv_winvm_key_linux(code, down); }

static void x_pointer_send(int8_t wheel) {
    os_unfair_lock_lock(&x_ptr_lock);
    int x = x_ptr_x, y = x_ptr_y;
    uint8_t buttons = x_ptr_buttons;
    os_unfair_lock_unlock(&x_ptr_lock);
    hv_winvm_tablet(x / 32767.0, y / 32767.0, buttons, wheel);
}

static int x_clamp(int v) { return v < 0 ? 0 : v > 32767 ? 32767 : v; }

void hv_winvm_x_pointer_at(int x, int y) {
    os_unfair_lock_lock(&x_ptr_lock);
    x_ptr_x = x_clamp(x), x_ptr_y = x_clamp(y);
    os_unfair_lock_unlock(&x_ptr_lock);
    x_pointer_send(0);
}

void hv_winvm_x_pointer_button(int button, int down) {
    if (button < 0 || button > 2) return;
    os_unfair_lock_lock(&x_ptr_lock);
    uint8_t bit = (uint8_t)(1u << button);
    x_ptr_buttons = down ? x_ptr_buttons | bit : x_ptr_buttons & (uint8_t)~bit;
    os_unfair_lock_unlock(&x_ptr_lock);
    x_pointer_send(0);
}

void hv_winvm_x_pointer_wheel(int amount) {
    while (amount) {
        int piece = amount > 100 ? 100 : amount < -100 ? -100 : amount;
        x_pointer_send((int8_t)piece);
        amount -= piece;
    }
}

void hv_winvm_x_pointer_step(int dx, int dy) {
    uint32_t w = 0, h = 0;
    if (hv_winvm_display_size(&w, &h) != 0 || !w || !h) w = 1920, h = 1080;
    os_unfair_lock_lock(&x_ptr_lock);
    x_ptr_x = x_clamp(x_ptr_x + (int)((int64_t)dx * 32767 / (int)w));
    x_ptr_y = x_clamp(x_ptr_y + (int)((int64_t)dy * 32767 / (int)h));
    os_unfair_lock_unlock(&x_ptr_lock);
    x_pointer_send(0);
}

int hv_winvm_x_pointer(int *x, int *y) {
    uint32_t w = 0, h = 0;
    if (hv_winvm_display_size(&w, &h) != 0 || !w || !h) return -1;
    os_unfair_lock_lock(&x_ptr_lock);
    *x = (int)((int64_t)x_ptr_x * (int)(w - 1) / 32767), *y = (int)((int64_t)x_ptr_y * (int)(h - 1) / 32767);
    os_unfair_lock_unlock(&x_ptr_lock);
    return 0;
}
