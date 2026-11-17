// SPDX-License-Identifier: GPL-2.0-or-later
/* visionOS spike: the disk a virtual PC installs Windows from, made from the ESD image Microsoft's own servers hand
   out, with nothing but this process: no Mac, no mounted volumes, no helper programs. What
   scripts/provision_windows_arm64.py does with wimlib-imagex, hdiutil and diskutil on a Mac, done with libwim and
   FatFs so that it runs on the device as well.

   The disk has two partitions:
     1  FAT32 "WINPE":   the UEFI boot files of the ESD's first image and sources\boot.wim, which is the ESD's
                         Windows PE image with a startnet.cmd that partitions the other disk, applies Windows to it
                         with DISM, puts the answer file in place and makes it bootable;
     2  exFAT "WINESD":  sources\install.esd, the ESD itself (it is over 4 GB, more than FAT32 holds in a file).
   Windows PE applies the edition straight from the ESD: nothing is converted or compressed again. */
#ifndef WINMEDIA_H
#define WINMEDIA_H

#include <stdint.h>

typedef struct WinMediaJob {
    const char *esd;       /* the ESD file */
    const char *image;     /* the disk image to write (replaced if it exists) */
    const char *tmpdir;    /* a folder for what is unpacked on the way (about 1 GB); emptied afterwards */
    const char *edition;   /* EDITIONID to install: "Professional" (NULL), "Core" for Home, ... */
    const char *user;      /* the local account, an administrator with no password (NULL: "User") */
    const char *computer;  /* the computer's name (NULL: "VISIONPRO") */
    const char *timezone;  /* a Windows time zone name (NULL: "W. Europe Standard Time") */
    const char *drivers;   /* a folder of drivers (.inf with their files) for Windows PE to add, or NULL */
    int service;           /* not an installation: a Windows that is installed already gets the drivers and the
                              tuning (the disk then holds Windows PE only, and Windows PE touches nothing else) */
    int lean;              /* also turn off what guards Windows: Defender, SmartScreen, Windows Update and the
                              Spectre mitigations. Much the largest saving under emulation, and a real loss of
                              protection: the person's choice. What costs no protection is turned off either way. */
    /* What is being done and how far along it is (0...1, or negative if it cannot be told). May be NULL. */
    void (*progress)(void *ctx, const char *step, double fraction);
    void *ctx;
    /* Filled in: */
    char language[32];     /* the ESD's language, "de-DE" */
    char edition_name[96]; /* the image's name, "Windows 11 Pro" */
    int edition_index;     /* its index in the ESD */
    char error[256];
} WinMediaJob;

/* 0 if the disk was written; otherwise job->error says what went wrong. */
int winmedia_build(WinMediaJob *job);

/* The screen a PC starts with, in its firmware's variables (vars.fd): one of the modes the firmware has
   (xros/patch_firmware.py: 1280x720, 1600x900, 1920x1080). Windows keeps the mode the firmware starts in.
   0 if the variable is set; 1 if the file has no variable store yet (a PC that never ran: its firmware makes one at
   the first start, with the firmware's own mode until then); -1 on an error. */
int winmedia_set_screen(const char *vars_fd, uint32_t width, uint32_t height);

/* The editions an ESD holds, one per line as "EDITIONID\tname", into out. Returns how many, or -1. */
int winmedia_editions(const char *esd, char *out, uint32_t outlen);

#endif
