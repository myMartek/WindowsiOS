// SPDX-License-Identifier: GPL-2.0-or-later
/* See winmedia.h. */
#include "winmedia.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wimlib.h>

#include "fatfs/ff.h"
#include "fatfs/diskio.h"

#define SECTOR 512u
#define PE_MB 1200u /* the FAT32 partition: boot files and boot.wim (about 600 MB as LZX) */

/* ---- FatFs's disk: the image file ---- */

static int img_fd = -1;
static uint64_t img_sectors;

PARTITION VolToPart[FF_VOLUMES] = {{0, 1}, {0, 2}}; /* "0:" the first partition, "1:" the second */

DSTATUS disk_status(BYTE pdrv) { return pdrv == 0 && img_fd >= 0 ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || img_fd < 0) return RES_NOTRDY;
    size_t len = (size_t)count * SECTOR;
    ssize_t got = pread(img_fd, buff, len, (off_t)sector * SECTOR);
    if (got < 0) return RES_ERROR;
    if ((size_t)got < len) memset(buff + got, 0, len - (size_t)got); /* past what was written yet: a sparse file's zeros */
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || img_fd < 0) return RES_NOTRDY;
    size_t len = (size_t)count * SECTOR;
    return pwrite(img_fd, buff, len, (off_t)sector * SECTOR) == (ssize_t)len ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv || img_fd < 0) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buff = (LBA_t)img_sectors; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buff = SECTOR; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
    default: return RES_PARERR;
    }
}

/* ---- small things ---- */

static int fail(WinMediaJob *job, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(job->error, sizeof job->error, fmt, ap);
    va_end(ap);
    return -1;
}

static void say(WinMediaJob *job, const char *step, double fraction) {
    if (job->progress) job->progress(job->ctx, step, fraction);
}

static int write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    /* Windows's line ends: cmd.exe reads labels wrongly in a batch file without them. */
    for (const char *p = text; *p; p++) {
        if (*p == '\n') fputc('\r', f);
        fputc(*p, f);
    }
    return fclose(f);
}

static void remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sub[1024];
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            struct stat st;
            if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(sub);
            else unlink(sub);
        }
        closedir(d);
    }
    rmdir(path);
}

/* A host file into the FAT or exFAT volume, with the step's progress if it is worth saying. */
static int copy_in(WinMediaJob *job, const char *host, const char *target, const char *step) {
    int in = open(host, O_RDONLY);
    if (in < 0) return fail(job, "cannot read %s: %s", host, strerror(errno));
    struct stat st;
    fstat(in, &st);
    FIL f;
    FRESULT r = f_open(&f, target, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) {
        close(in);
        return fail(job, "cannot create %s on the disk (FatFs %d)", target, r);
    }
    if (st.st_size > (64 << 20)) f_expand(&f, (FSIZE_t)st.st_size, 1); /* in one piece if there is room: no chain to follow */
    enum { CHUNK = 4 << 20 };
    BYTE *buf = malloc(CHUNK);
    off_t done = 0;
    int result = 0;
    while (buf) {
        ssize_t n = read(in, buf, CHUNK);
        if (n < 0) { result = fail(job, "reading %s: %s", host, strerror(errno)); break; }
        if (n == 0) break;
        UINT wrote = 0;
        r = f_write(&f, buf, (UINT)n, &wrote);
        if (r != FR_OK || wrote != (UINT)n) { result = fail(job, "writing %s to the disk (FatFs %d)", target, r); break; }
        done += n;
        if (step) say(job, step, st.st_size ? (double)done / (double)st.st_size : -1);
    }
    if (!buf) result = fail(job, "out of memory");
    free(buf);
    close(in);
    r = f_close(&f);
    if (!result && r != FR_OK) result = fail(job, "closing %s on the disk (FatFs %d)", target, r);
    return result;
}

/* A host folder and everything under it into the volume. */
static int copy_tree(WinMediaJob *job, const char *host, const char *target) {
    DIR *d = opendir(host);
    if (!d) return fail(job, "cannot list %s: %s", host, strerror(errno));
    struct dirent *e;
    int result = 0;
    while (!result && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char from[1024], to[512];
        snprintf(from, sizeof from, "%s/%s", host, e->d_name);
        snprintf(to, sizeof to, "%s/%s", target, e->d_name);
        struct stat st;
        if (stat(from, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            FRESULT r = f_mkdir(to);
            if (r != FR_OK && r != FR_EXIST) result = fail(job, "cannot make %s on the disk (FatFs %d)", to, r);
            else result = copy_tree(job, from, to);
        } else if (S_ISREG(st.st_mode)) {
            result = copy_in(job, from, to, NULL);
        }
    }
    closedir(d);
    return result;
}

/* ---- what Windows PE runs, and the answer file Windows finds at its first start ---- */

/* Drivers: copied to \Drivers of the new Windows, which adds them itself with pnputil as it sets itself up (the
   answer file's specialize pass). Not with DISM from Windows PE: its /Add-Driver ends with 0x80004002, "interface
   not supported", against this image (seen in winmedia.log), and Windows came up without a network card. What is
   copied and what the tuning script prints goes to winmedia.log at the root of the new Windows.
   The disk to install on is the one that is not this one: told apart by the labels of this disk's volumes, which
   diskpart prints whatever language it speaks (its own words differ: "Disk", "Datentraeger", ...). With find:
   Windows PE has no findstr (a first version used it, and without it took the first disk there was). */
static const char startnet_fmt[] =
    "@echo off\n"
    "wpeinit\n"
    "echo [WINMEDIA] Windows PE is running. Looking for the disks...\n"
    "set ESD=\n"
    "for %%%%l in (C D E F G H I J K L M N O P) do if exist %%%%l:\\sources\\install.esd set ESD=%%%%l:\\sources\\install.esd\n"
    "if \"%%ESD%%\"==\"\" (\n"
    "  echo [WINMEDIA] ERROR: sources\\install.esd was not found on any volume.\n"
    "  goto fail\n"
    ")\n"
    "set TARGET=\n"
    "for %%%%n in (0 1 2 3) do call :probe %%%%n\n"
    "if \"%%TARGET%%\"==\"\" (\n"
    "  echo [WINMEDIA] ERROR: no disk to install on was found.\n"
    "  goto fail\n"
    ")\n"
    "echo [WINMEDIA] Installing on disk %%TARGET%% from %%ESD%%.\n"
    "(\n"
    "  echo select disk %%TARGET%%\n"
    "  echo clean\n"
    "  echo convert gpt\n"
    "  echo create partition efi size=100\n"
    "  echo format quick fs=fat32 label=\"System\"\n"
    "  echo assign letter=S\n"
    "  echo create partition msr size=16\n"
    "  echo create partition primary\n"
    "  echo format quick fs=ntfs label=\"Windows\"\n"
    "  echo assign letter=W\n"
    ") > X:\\part.txt\n"
    "diskpart /s X:\\part.txt\n"
    "if errorlevel 1 (\n"
    "  echo [WINMEDIA] ERROR: the disk could not be partitioned.\n"
    "  goto fail\n"
    ")\n"
    "echo [WINMEDIA] Applying %s. This is the long part.\n"
    "dism /Apply-Image /ImageFile:%%ESD%% /Index:%d /ApplyDir:W:\\\n"
    "if errorlevel 1 (\n"
    "  echo [WINMEDIA] ERROR: DISM could not apply the image.\n"
    "  find /i \"Error\" X:\\windows\\Logs\\DISM\\dism.log\n"
    "  goto fail\n"
    ")\n"
    "echo winmedia: what Windows PE did after applying the image > W:\\winmedia.log\n"
    "for %%%%l in (C D E F G H I J K L M N O P) do if exist %%%%l:\\drivers\\winmedia.txt call :drivers %%%%l\n"
    "call X:\\Windows\\System32\\winmedia_tune.cmd W: >> W:\\winmedia.log 2>&1\n"
    "echo [WINMEDIA] The answer file and the settings a virtual PC needs...\n"
    "mkdir W:\\Windows\\Panther\n"
    "copy /y X:\\Windows\\System32\\unattend_panther.xml W:\\Windows\\Panther\\unattend.xml\n"
    "reg load HKLM\\OFFSYS W:\\Windows\\System32\\config\\SYSTEM\n"
    "reg add HKLM\\OFFSYS\\ControlSet001\\Control\\BitLocker /v PreventDeviceEncryption /t REG_DWORD /d 1 /f\n"
    "reg add HKLM\\OFFSYS\\Setup\\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f\n"
    "reg add HKLM\\OFFSYS\\Setup\\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f\n"
    "reg add HKLM\\OFFSYS\\Setup\\LabConfig /v BypassRAMCheck /t REG_DWORD /d 1 /f\n"
    "reg add HKLM\\OFFSYS\\Setup\\LabConfig /v BypassCPUCheck /t REG_DWORD /d 1 /f\n"
    "reg unload HKLM\\OFFSYS\n"
    "reg load HKLM\\OFFSOFT W:\\Windows\\System32\\config\\SOFTWARE\n"
    "reg add HKLM\\OFFSOFT\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f\n"
    "reg unload HKLM\\OFFSOFT\n"
    "echo [WINMEDIA] Making the disk bootable...\n"
    "bcdboot W:\\Windows /s S: /f UEFI\n"
    "if errorlevel 1 (\n"
    "  echo [WINMEDIA] ERROR: bcdboot failed.\n"
    "  goto fail\n"
    ")\n"
    "bcdedit /store S:\\EFI\\Microsoft\\Boot\\BCD /set {default} recoveryenabled no\n"
    "bcdedit /store S:\\EFI\\Microsoft\\Boot\\BCD /set {default} bootmenupolicy standard\n"
    "echo [WINMEDIA] DONE. Windows is on the disk. Restarting into it.\n"
    "wpeutil reboot\n"
    "goto :eof\n"
    "\n"
    ":probe\n"
    "if not \"%%TARGET%%\"==\"\" goto :eof\n"
    "(\n"
    "  echo select disk %%1\n"
    "  echo detail disk\n"
    ") > X:\\probe.txt\n"
    "diskpart /s X:\\probe.txt > X:\\disk%%1.log 2>&1\n"
    "if errorlevel 1 goto :eof\n"
    "find \"WINESD\" X:\\disk%%1.log > nul\n"
    "if not errorlevel 1 goto :eof\n"
    "find \"WINPE\" X:\\disk%%1.log > nul\n"
    "if not errorlevel 1 goto :eof\n"
    "set TARGET=%%1\n"
    "goto :eof\n"
    "\n"
    ":drivers\n"
    "echo [WINMEDIA] Copying the drivers from %%1:\\drivers for Windows to install at its first start...\n"
    "mkdir W:\\Drivers\n"
    "copy /y %%1:\\drivers\\*.* W:\\Drivers\\ >> W:\\winmedia.log 2>&1\n"
    "goto :eof\n"
    "\n"
    ":fail\n"
    "echo [WINMEDIA] FAILED. This window stays open; the virtual PC can be stopped.\n"
    "cmd /k\n";

/* For a Windows that is installed already: the same drivers and tuning, nothing else touched, but for the screen
   size Windows remembers: one installed under the firmware's old 800x600 keeps drawing that, with black bars, in the
   bigger mode the firmware has now, until what it saved for the display is gone. The drivers are
   added at Windows's next start, by Windows, before anyone logs in: the Setup key's CmdLine with SetupType 2 runs
   once as the system, and the script it runs puts both back. (Used as it is, not through printf.)
   At the end this disk takes its own boot loader out of the way and restarts: the firmware, finding nothing to start
   on it, goes on to the PC's disk, and Windows comes up in the same run. (The PC can be set up only once in a
   launch of the app: a run that ended in a shutdown had to be followed by quitting the app and starting again,
   a step too easy to miss.) If the disk cannot be written, it shuts down as before. */
static const char service_script[] =
    "@echo off\n"
    "wpeinit\n"
    "echo [WINMEDIA] Service run. Looking for the Windows that is installed...\n"
    "set WIN=\n"
    "for %%l in (C D E F G H I J K L M N O P) do if exist %%l:\\Windows\\System32\\config\\SYSTEM set WIN=%%l:\n"
    "if \"%WIN%\"==\"\" (\n"
    "  echo [WINMEDIA] ERROR: no installed Windows was found.\n"
    "  goto fail\n"
    ")\n"
    "echo [WINMEDIA] Windows is on %WIN%.\n"
    "echo winmedia: service run >> %WIN%\\winmedia.log\n"
    "set DRV=\n"
    "for %%l in (C D E F G H I J K L M N O P) do if exist %%l:\\drivers\\winmedia.txt set DRV=%%l:\n"
    "if \"%DRV%\"==\"\" goto tune\n"
    "echo [WINMEDIA] Copying the drivers for Windows to install at its next start...\n"
    "mkdir %WIN%\\Drivers 2>nul\n"
    "copy /y %DRV%\\drivers\\*.* %WIN%\\Drivers\\ >> %WIN%\\winmedia.log 2>&1\n"
    "reg load HKLM\\OFFSYS %WIN%\\Windows\\System32\\config\\SYSTEM >nul\n"
    "reg add HKLM\\OFFSYS\\Setup /v CmdLine /t REG_SZ /d \"cmd.exe /c C:\\Drivers\\winmedia_once.cmd\" /f >> %WIN%\\winmedia.log 2>&1\n"
    "reg add HKLM\\OFFSYS\\Setup /v SetupType /t REG_DWORD /d 2 /f >> %WIN%\\winmedia.log 2>&1\n"
    "reg unload HKLM\\OFFSYS >nul\n"
    ":tune\n"
    "echo [WINMEDIA] Forgetting the screen size Windows had...\n"
    "reg load HKLM\\OFFSYS %WIN%\\Windows\\System32\\config\\SYSTEM >nul\n"
    "reg delete HKLM\\OFFSYS\\ControlSet001\\Control\\GraphicsDrivers\\Configuration /f >> %WIN%\\winmedia.log 2>&1\n"
    "reg delete HKLM\\OFFSYS\\ControlSet001\\Control\\GraphicsDrivers\\Connectivity /f >> %WIN%\\winmedia.log 2>&1\n"
    "reg unload HKLM\\OFFSYS >nul\n"
    "echo [WINMEDIA] Turning off background work...\n"
    "call X:\\Windows\\System32\\winmedia_tune.cmd %WIN% >> %WIN%\\winmedia.log 2>&1\n"
    "set MEDIA=\n"
    "for %%l in (C D E F G H I J K L M N O P) do if exist %%l:\\sources\\boot.wim set MEDIA=%%l:\n"
    "if \"%MEDIA%\"==\"\" goto off\n"
    "echo ran > %MEDIA%\\winmedia.ran\n"
    "if not exist %MEDIA%\\winmedia.ran goto off\n"
    "ren %MEDIA%\\efi\\boot\\bootaa64.efi bootaa64.off\n"
    "if exist %MEDIA%\\efi\\boot\\bootaa64.efi goto off\n"
    "echo [WINMEDIA] DONE. Restarting into Windows.\n"
    "wpeutil reboot\n"
    "goto :eof\n"
    "\n"
    ":off\n"
    "echo [WINMEDIA] DONE. Shutting down: start the PC again for Windows.\n"
    "wpeutil shutdown\n"
    "goto :eof\n"
    "\n"
    ":fail\n"
    "echo [WINMEDIA] FAILED. This window stays open; the virtual PC can be stopped.\n"
    "cmd /k\n";

/* What Windows runs once, as the system, at its next start after a service run. */
static const char once_script[] =
    "@echo off\n"
    "echo winmedia: adding drivers at start >> C:\\winmedia.log\n"
    "pnputil /add-driver C:\\Drivers\\*.inf /install >> C:\\winmedia.log 2>&1\n"
    "reg add HKLM\\SYSTEM\\Setup /v SetupType /t REG_DWORD /d 0 /f >> C:\\winmedia.log 2>&1\n"
    "reg add HKLM\\SYSTEM\\Setup /v CmdLine /t REG_SZ /d \"\" /f >> C:\\winmedia.log 2>&1\n";

static const char unattend_fmt[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\" xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
    "  <settings pass=\"specialize\">\n"
    "    <component name=\"Microsoft-Windows-Shell-Setup\" processorArchitecture=\"arm64\" publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\n"
    "      <ComputerName>%s</ComputerName>\n"
    "      <TimeZone>%s</TimeZone>\n"
    "    </component>\n"
    "    <component name=\"Microsoft-Windows-Deployment\" processorArchitecture=\"arm64\" publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\n"
    "      <RunSynchronous>\n"
    "        <RunSynchronousCommand wcm:action=\"add\">\n"
    "          <Order>1</Order>\n"
    "          <Path>reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>\n"
    "        </RunSynchronousCommand>\n"
    "        <RunSynchronousCommand wcm:action=\"add\">\n"
    "          <Order>2</Order>\n"
    "          <Path>reg add HKLM\\SYSTEM\\CurrentControlSet\\Control\\BitLocker /v PreventDeviceEncryption /t REG_DWORD /d 1 /f</Path>\n"
    "        </RunSynchronousCommand>\n"
    "        <RunSynchronousCommand wcm:action=\"add\">\n"
    "          <Order>3</Order>\n"
    "          <Path>cmd /c \"if exist C:\\Drivers\\*.inf pnputil /add-driver C:\\Drivers\\*.inf /install &gt;&gt; C:\\winmedia.log 2&gt;&amp;1\"</Path>\n"
    "        </RunSynchronousCommand>\n"
    "      </RunSynchronous>\n"
    "    </component>\n"
    "  </settings>\n"
    "  <settings pass=\"oobeSystem\">\n"
    "    <component name=\"Microsoft-Windows-International-Core\" processorArchitecture=\"arm64\" publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\n"
    "      <InputLocale>%s</InputLocale>\n"
    "      <SystemLocale>%s</SystemLocale>\n"
    "      <UILanguage>%s</UILanguage>\n"
    "      <UserLocale>%s</UserLocale>\n"
    "    </component>\n"
    "    <component name=\"Microsoft-Windows-Shell-Setup\" processorArchitecture=\"arm64\" publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\n"
    "      <OOBE>\n"
    "        <HideEULAPage>true</HideEULAPage>\n"
    "        <HideLocalAccountScreen>true</HideLocalAccountScreen>\n"
    "        <HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>\n"
    "        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n"
    "        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n"
    "        <ProtectYourPC>3</ProtectYourPC>\n"
    "      </OOBE>\n"
    "      <UserAccounts>\n"
    "        <LocalAccounts>\n"
    "          <LocalAccount wcm:action=\"add\">\n"
    "            <Password><Value></Value><PlainText>true</PlainText></Password>\n"
    "            <Description>%s</Description>\n"
    "            <DisplayName>%s</DisplayName>\n"
    "            <Group>Administrators</Group>\n"
    "            <Name>%s</Name>\n"
    "          </LocalAccount>\n"
    "        </LocalAccounts>\n"
    "      </UserAccounts>\n"
    "      <AutoLogon>\n"
    "        <Password><Value></Value><PlainText>true</PlainText></Password>\n"
    "        <Enabled>true</Enabled>\n"
    "        <Username>%s</Username>\n"
    "      </AutoLogon>\n"
    "      <FirstLogonCommands>\n"
    "        <SynchronousCommand wcm:action=\"add\">\n"
    "          <Order>1</Order>\n"
    "          <CommandLine>cmd /c \"powercfg /h off &amp; powercfg /change monitor-timeout-ac 0 &amp; powercfg /change standby-timeout-ac 0\"</CommandLine>\n"
    "          <Description>No hibernation, no display or sleep timeouts</Description>\n"
    "        </SynchronousCommand>\n"
    "        <SynchronousCommand wcm:action=\"add\">\n"
    "          <Order>2</Order>\n"
    "          <CommandLine>cmd /c \"bcdedit /set {default} bootmenupolicy standard &amp; bcdedit /set {default} recoveryenabled no\"</CommandLine>\n"
    "          <Description>No automatic repair loop after a hard stop</Description>\n"
    "        </SynchronousCommand>\n"
    "        <SynchronousCommand wcm:action=\"add\">\n"
    "          <Order>3</Order>\n"
    "          <CommandLine>cmd /c \"timeout /t 15 /nobreak &amp; shutdown /s /t 2 /f\"</CommandLine>\n"
    "          <Description>The first desktop is the end of the installation: shut down</Description>\n"
    "        </SynchronousCommand>\n"
    "      </FirstLogonCommands>\n"
    "    </component>\n"
    "  </settings>\n"
    "</unattend>\n";

/* What Windows does in the background, turned off before it first starts: under emulation every one of these is paid
   for many times over, and on a headset it is paid for in heat. The list is WindowsiOS's (scripts/optimize_windows_lite.py),
   split in two. `guard` marks what protects Windows (Defender, SmartScreen, Windows Update, the Spectre mitigations):
   those only if the person asked for it (WinMediaJob.lean). Written with reg.exe into the hives of the Windows just
   applied, each line on its own: one that is refused (a key only Windows itself may change) costs only itself. */
struct tune { char hive; /* S: SYSTEM\ControlSet001, W: SOFTWARE, U: the default user's */ char guard; const char *key, *value, *type, *data; };
#define SVC(name, guard) {'S', guard, "Services\\" name, "Start", "REG_DWORD", "4"}
static const struct tune tunes[] = {
    SVC("SysMain", 0), SVC("WSearch", 0), SVC("DiagTrack", 0), SVC("dmwappushservice", 0), SVC("WerSvc", 0), SVC("PcaSvc", 0),
    SVC("DPS", 0), SVC("WdiServiceHost", 0), SVC("WdiSystemHost", 0), SVC("MapsBroker", 0), SVC("lfsvc", 0), SVC("wisvc", 0),
    SVC("RetailDemo", 0), SVC("XblAuthManager", 0), SVC("XblGameSave", 0), SVC("XboxGipSvc", 0), SVC("XboxNetApiSvc", 0),
    SVC("WinDefend", 1), SVC("WdNisSvc", 1), SVC("WdNisDrv", 1), SVC("WdFilter", 1), SVC("WdBoot", 1), SVC("Sense", 1),
    SVC("SecurityHealthService", 1), SVC("wscsvc", 1), SVC("MDCoreSvc", 1), SVC("webthreatdefsvc", 1), SVC("webthreatdefusersvc", 1),
    SVC("wuauserv", 1), SVC("UsoSvc", 1), SVC("WaaSMedicSvc", 1), SVC("DoSvc", 1),
    {'S', 0, "Control\\Power", "HibernateEnabled", "REG_DWORD", "0"},
    {'S', 0, "Control\\Power", "HibernateEnabledDefault", "REG_DWORD", "0"},
    {'S', 0, "Control\\PriorityControl", "Win32PrioritySeparation", "REG_DWORD", "38"},
    {'S', 0, "Control\\Session Manager\\Memory Management", "DisablePagingExecutive", "REG_DWORD", "1"},
    {'S', 1, "Control\\Session Manager\\Memory Management", "FeatureSettingsOverride", "REG_DWORD", "3"},
    {'S', 1, "Control\\Session Manager\\Memory Management", "FeatureSettingsOverrideMask", "REG_DWORD", "3"},
    {'W', 0, "Policies\\Microsoft\\Windows\\CloudContent", "DisableWindowsConsumerFeatures", "REG_DWORD", "1"},
    {'W', 0, "Policies\\Microsoft\\Windows\\CloudContent", "DisableSoftLanding", "REG_DWORD", "1"},
    {'W', 0, "Policies\\Microsoft\\Windows\\CloudContent", "DisableCloudOptimizedContent", "REG_DWORD", "1"},
    {'W', 0, "Policies\\Microsoft\\Dsh", "AllowNewsAndInterests", "REG_DWORD", "0"},
    {'W', 0, "Policies\\Microsoft\\Windows\\WindowsCopilot", "TurnOffWindowsCopilot", "REG_DWORD", "1"},
    {'W', 0, "Policies\\Microsoft\\Windows\\DataCollection", "AllowTelemetry", "REG_DWORD", "0"},
    {'W', 0, "Policies\\Microsoft\\Windows\\Explorer", "DisableSearchBoxSuggestions", "REG_DWORD", "1"},
    {'W', 0, "Policies\\Microsoft\\Edge", "StartupBoostEnabled", "REG_DWORD", "0"},
    {'W', 0, "Policies\\Microsoft\\Edge", "BackgroundModeEnabled", "REG_DWORD", "0"},
    {'W', 0, "Policies\\Microsoft\\Edge", "HubsSidebarEnabled", "REG_DWORD", "0"},
    {'W', 0, "Policies\\Microsoft\\WindowsStore", "AutoDownload", "REG_DWORD", "2"},
    {'W', 0, "Microsoft\\Windows NT\\CurrentVersion\\Schedule\\Maintenance", "MaintenanceDisabled", "REG_DWORD", "1"},
    {'W', 0, "Microsoft\\Windows\\CurrentVersion\\Run", "OneDriveSetup", NULL, NULL},
    {'W', 1, "Policies\\Microsoft\\Windows Defender", "DisableAntiSpyware", "REG_DWORD", "1"},
    {'W', 1, "Policies\\Microsoft\\Windows Defender", "DisableRoutinelyTakingAction", "REG_DWORD", "1"},
    {'W', 1, "Policies\\Microsoft\\Windows Defender\\Real-Time Protection", "DisableRealtimeMonitoring", "REG_DWORD", "1"},
    {'W', 1, "Policies\\Microsoft\\Windows Defender\\Real-Time Protection", "DisableBehaviorMonitoring", "REG_DWORD", "1"},
    {'W', 1, "Policies\\Microsoft\\Windows Defender\\Real-Time Protection", "DisableOnAccessProtection", "REG_DWORD", "1"},
    {'W', 1, "Policies\\Microsoft\\Windows Defender\\Real-Time Protection", "DisableScanOnRealtimeEnable", "REG_DWORD", "1"},
    {'W', 1, "Policies\\Microsoft\\Windows Defender\\Spynet", "SpynetReporting", "REG_DWORD", "0"},
    {'W', 1, "Policies\\Microsoft\\Windows Defender\\Spynet", "SubmitSamplesConsent", "REG_DWORD", "2"},
    {'W', 1, "Microsoft\\Windows Defender\\Features", "TamperProtection", "REG_DWORD", "0"},
    {'W', 1, "Policies\\Microsoft\\Windows\\WindowsUpdate\\AU", "NoAutoUpdate", "REG_DWORD", "1"},
    {'W', 1, "Policies\\Microsoft\\Windows\\System", "EnableSmartScreen", "REG_DWORD", "0"},
    {'W', 1, "Microsoft\\Windows\\CurrentVersion\\Explorer", "SmartScreenEnabled", "REG_SZ", "Off"},
    {'W', 1, "Microsoft\\Windows\\CurrentVersion\\Run", "SecurityHealth", NULL, NULL},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", "EnableTransparency", "REG_DWORD", "0"},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects", "VisualFXSetting", "REG_DWORD", "3"},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", "TaskbarAnimations", "REG_DWORD", "0"},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", "ListviewAlphaSelect", "REG_DWORD", "0"},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", "ListviewShadow", "REG_DWORD", "0"},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", "TaskbarDa", "REG_DWORD", "0"},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", "ShowCopilotButton", "REG_DWORD", "0"},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", "ShowTaskViewButton", "REG_DWORD", "0"},
    {'U', 0, "Control Panel\\Desktop", "UserPreferencesMask", "REG_BINARY", "9012038010000000"},
    {'U', 0, "Control Panel\\Desktop", "MenuShowDelay", "REG_SZ", "0"},
    {'U', 0, "Control Panel\\Desktop", "DragFullWindows", "REG_SZ", "0"},
    {'U', 0, "Control Panel\\Desktop\\WindowMetrics", "MinAnimate", "REG_SZ", "0"},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\Search", "BingSearchEnabled", "REG_DWORD", "0"},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", "ContentDeliveryAllowed", "REG_DWORD", "0"},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", "PreInstalledAppsEnabled", "REG_DWORD", "0"},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", "SilentInstalledAppsEnabled", "REG_DWORD", "0"},
    {'U', 0, "Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", "SystemPaneSuggestionsEnabled", "REG_DWORD", "0"},
};
#undef SVC

/* winmedia_tune.cmd for Windows PE: its argument is the drive Windows is on ("W:"). */
static int write_tune(const char *path, int lean) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fputs("@echo off\r\necho tuning: background work turned off\r\n"
          "reg load HKLM\\OFFSYS %1\\Windows\\System32\\config\\SYSTEM >nul\r\n"
          "reg load HKLM\\OFFSOFT %1\\Windows\\System32\\config\\SOFTWARE >nul\r\n"
          "reg load HKLM\\OFFUSR %1\\Users\\Default\\NTUSER.DAT >nul\r\n", f);
    for (size_t i = 0; i < sizeof tunes / sizeof tunes[0]; i++) {
        const struct tune *t = &tunes[i];
        if (t->guard && !lean) continue;
        const char *root = t->hive == 'S' ? "HKLM\\OFFSYS\\ControlSet001\\" : t->hive == 'W' ? "HKLM\\OFFSOFT\\" : "HKLM\\OFFUSR\\";
        if (t->type) fprintf(f, "reg add \"%s%s\" /v %s /t %s /d %s /f >nul 2>&1\r\n", root, t->key, t->value, t->type, t->data);
        else fprintf(f, "reg delete \"%s%s\" /v %s /f >nul 2>&1\r\n", root, t->key, t->value);
    }
    fputs("reg query \"HKLM\\OFFSYS\\ControlSet001\\Services\\SysMain\" /v Start\r\n"
          "reg unload HKLM\\OFFUSR >nul\r\nreg unload HKLM\\OFFSOFT >nul\r\nreg unload HKLM\\OFFSYS >nul\r\n", f);
    return fclose(f);
}

/* A name for XML and for a batch file: letters, digits, space, dash and dot stay; the rest goes. */
static void plain(char *out, size_t outlen, const char *in, const char *fallback) {
    size_t n = 0;
    for (const char *p = in ? in : ""; *p && n + 1 < outlen; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ' || c == '-' || c == '.' || c >= 0x80) out[n++] = (char)c;
    }
    out[n] = 0;
    if (!n) snprintf(out, outlen, "%s", fallback);
}

/* ---- the ESD ---- */

struct wim_progress {
    WinMediaJob *job;
    const char *step;
};

static enum wimlib_progress_status wim_progress(enum wimlib_progress_msg msg, union wimlib_progress_info *info, void *ctx) {
    struct wim_progress *p = ctx;
    if (msg == WIMLIB_PROGRESS_MSG_WRITE_STREAMS && info->write_streams.total_bytes)
        say(p->job, p->step, (double)info->write_streams.completed_bytes / (double)info->write_streams.total_bytes);
    else if (msg == WIMLIB_PROGRESS_MSG_EXTRACT_STREAMS && info->extract.total_bytes)
        say(p->job, p->step, (double)info->extract.completed_bytes / (double)info->extract.total_bytes);
    return WIMLIB_PROGRESS_STATUS_CONTINUE;
}

static const char *prop(WIMStruct *wim, int image, const char *name) {
    const char *v = wimlib_get_image_property(wim, image, name);
    return v ? v : "";
}

int winmedia_editions(const char *esd, char *out, uint32_t outlen) {
    WIMStruct *wim = NULL;
    if (!outlen) return -1;
    out[0] = 0;
    wimlib_global_init(0);
    if (wimlib_open_wim(esd, 0, &wim)) return -1;
    struct wimlib_wim_info info;
    wimlib_get_wim_info(wim, &info);
    int n = 0;
    for (int i = 1; i <= (int)info.image_count; i++) {
        const char *ed = prop(wim, i, "WINDOWS/EDITIONID");
        if (!*ed || !strcmp(ed, "WindowsPE") || !*prop(wim, i, "WINDOWS/INSTALLATIONTYPE")) continue;
        size_t at = strlen(out);
        snprintf(out + at, outlen - at, "%s\t%s\n", ed, wimlib_get_image_name(wim, i));
        n++;
    }
    wimlib_free(wim);
    return n;
}

int winmedia_build(WinMediaJob *job) {
    job->error[0] = 0;
    char path[1024], stage[1024];
    WIMStruct *esd = NULL, *boot = NULL;
    int result = -1, err;
    FATFS *fs0 = NULL, *fs1 = NULL;
    BYTE *work = NULL;
    struct wim_progress wp = {job, ""};

    struct stat st;
    if (stat(job->esd, &st) != 0) return fail(job, "the ESD is not there: %s", job->esd);
    uint64_t esd_size = (uint64_t)st.st_size;
    mkdir(job->tmpdir, 0755);
    snprintf(stage, sizeof stage, "%s/winmedia-stage", job->tmpdir);
    remove_tree(stage);
    if (mkdir(stage, 0755) != 0) return fail(job, "cannot make %s: %s", stage, strerror(errno));

    say(job, "Reading the ESD", -1);
    wimlib_global_init(0);
    if ((err = wimlib_open_wim(job->esd, 0, &esd))) { fail(job, "the ESD cannot be opened: %s", wimlib_get_error_string(err)); goto out; }
    struct wimlib_wim_info info;
    wimlib_get_wim_info(esd, &info);
    int pe = 0, edition = 0;
    const char *want = job->edition && *job->edition ? job->edition : "Professional";
    for (int i = 1; i <= (int)info.image_count; i++) {
        const char *ed = prop(esd, i, "WINDOWS/EDITIONID");
        if (!strcmp(ed, "WindowsPE")) { if (!pe) pe = i; } /* the first is Windows PE itself, the second Windows Setup on it */
        else if (!strcmp(ed, want) && !edition) edition = i;
    }
    if (!pe) { fail(job, "the ESD has no Windows PE image"); goto out; }
    if (!edition) { fail(job, "the ESD has no edition \"%s\"", want); goto out; }
    job->edition_index = edition;
    snprintf(job->edition_name, sizeof job->edition_name, "%s", wimlib_get_image_name(esd, edition));
    snprintf(job->language, sizeof job->language, "%s", prop(esd, edition, "WINDOWS/LANGUAGES/DEFAULT"));
    if (!job->language[0]) snprintf(job->language, sizeof job->language, "en-US");

    /* The disk: FAT32 for the firmware to boot from, exFAT for the one file too big for it. */
    say(job, "Making the installer disk", -1);
    uint64_t bytes = ((uint64_t)PE_MB << 20) + (job->service ? 0 : esd_size + (192ull << 20)) + (64ull << 20);
    img_sectors = bytes / SECTOR;
    unlink(job->image);
    img_fd = open(job->image, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (img_fd < 0 || ftruncate(img_fd, (off_t)(img_sectors * SECTOR)) != 0) { fail(job, "cannot make %s: %s", job->image, strerror(errno)); goto out; }
    enum { WORK = 1 << 20 };
    work = malloc(WORK);
    fs0 = calloc(1, sizeof *fs0), fs1 = calloc(1, sizeof *fs1);
    if (!work || !fs0 || !fs1) { fail(job, "out of memory"); goto out; }
    LBA_t parts[4] = {(LBA_t)(((uint64_t)PE_MB << 20) / SECTOR), job->service ? 0 : 100, 0, 0}; /* then all of the rest */
    FRESULT r = f_fdisk(0, parts, work);
    if (r != FR_OK) { fail(job, "cannot partition the disk (FatFs %d)", r); goto out; }
    {   /* The disk's signature. FatFs leaves it zero, and Windows gives a disk without one a signature before it
           looks at its partitions: on a disk it cannot write to (this one is attached read-only) it then shows
           no volume at all (seen: Windows PE started from the disk and found nothing on it). */
        BYTE mbr[SECTOR];
        uint32_t id = arc4random() | 1;
        if (disk_read(0, mbr, 0, 1) != RES_OK) { fail(job, "cannot read the new partition table"); goto out; }
        memcpy(mbr + 0x1b8, &id, 4);
        if (disk_write(0, mbr, 0, 1) != RES_OK) { fail(job, "cannot write the disk's signature"); goto out; }
    }
    MKFS_PARM fat = {FM_FAT32, 2, 0, 0, 0}, exfat = {FM_EXFAT, 1, 0, 0, 128 * 1024};
    if ((r = f_mkfs("0:", &fat, work, WORK)) != FR_OK) { fail(job, "cannot format the FAT32 partition (FatFs %d)", r); goto out; }
    if (!job->service && (r = f_mkfs("1:", &exfat, work, WORK)) != FR_OK) { fail(job, "cannot format the exFAT partition (FatFs %d)", r); goto out; }
    if ((r = f_mount(fs0, "0:", 1)) != FR_OK || (!job->service && (r = f_mount(fs1, "1:", 1)) != FR_OK)) { fail(job, "cannot open the new partitions (FatFs %d)", r); goto out; }
    f_setlabel("0:WINPE");
    if (!job->service) f_setlabel("1:WINESD");

    /* What the firmware starts: the first image's boot files. */
    say(job, "Unpacking the boot files", 0);
    wp.step = "Unpacking the boot files";
    wimlib_register_progress_function(esd, wim_progress, &wp);
    snprintf(path, sizeof path, "%s/pe", stage);
    mkdir(path, 0755);
    const char *boot_paths[] = {"/efi", "/boot", "/bootmgr.efi"};
    if ((err = wimlib_extract_paths(esd, 1, path, boot_paths, 3, WIMLIB_EXTRACT_FLAG_NO_ACLS | WIMLIB_EXTRACT_FLAG_NO_ATTRIBUTES))) {
        fail(job, "the boot files cannot be unpacked: %s", wimlib_get_error_string(err));
        goto out;
    }
    if (copy_tree(job, path, "0:")) goto out;
    remove_tree(path);

    /* Windows PE with our start script and the answer file in it. */
    char user[64], computer[32], zone[96], script[1024], answer[1024];
    plain(user, sizeof user, job->user, "User");
    plain(computer, sizeof computer, job->computer, "VISIONPRO");
    plain(zone, sizeof zone, job->timezone, "W. Europe Standard Time");
    {
        size_t len = sizeof startnet_fmt + sizeof service_script + sizeof job->edition_name + 64;
        char *text = malloc(len);
        if (!text) { fail(job, "out of memory"); goto out; }
        char label[sizeof job->edition_name];
        plain(label, sizeof label, job->edition_name, "Windows");
        if (job->service) snprintf(text, len, "%s", service_script);
        else snprintf(text, len, startnet_fmt, label, edition);
        snprintf(script, sizeof script, "%s/startnet.cmd", stage);
        int bad = write_text(script, text);
        free(text);
        len = sizeof unattend_fmt + 8 * 96;
        text = malloc(len);
        if (!text || bad) { free(text); fail(job, "cannot write the start script"); goto out; }
        snprintf(text, len, unattend_fmt, computer, zone, job->language, job->language, job->language, job->language, user, user, user, user);
        snprintf(answer, sizeof answer, "%s/unattend_panther.xml", stage);
        bad = write_text(answer, text);
        free(text);
        if (bad) { fail(job, "cannot write the answer file"); goto out; }
    }
    say(job, "Preparing Windows PE", 0);
    wp.step = "Preparing Windows PE";
    if ((err = wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_LZX, &boot))) { fail(job, "cannot start boot.wim: %s", wimlib_get_error_string(err)); goto out; }
    wimlib_register_progress_function(boot, wim_progress, &wp);
    if ((err = wimlib_export_image(esd, pe, boot, NULL, NULL, WIMLIB_EXPORT_FLAG_BOOT))) { fail(job, "Windows PE cannot be taken from the ESD: %s", wimlib_get_error_string(err)); goto out; }
    snprintf(path, sizeof path, "%s/boot.wim", stage);
    if ((err = wimlib_write(boot, path, WIMLIB_ALL_IMAGES, 0, 0))) { fail(job, "boot.wim cannot be written: %s", wimlib_get_error_string(err)); goto out; }
    wimlib_free(boot), boot = NULL;
    /* An image taken from another file is that file's too until it is written: only then can it be changed. */
    say(job, "Adding the start script", -1);
    if ((err = wimlib_open_wim(path, WIMLIB_OPEN_FLAG_WRITE_ACCESS, &boot))) { fail(job, "boot.wim cannot be opened again: %s", wimlib_get_error_string(err)); goto out; }
    char tune[1024];
    snprintf(tune, sizeof tune, "%s/winmedia_tune.cmd", stage);
    if (write_tune(tune, job->lean) != 0) { fail(job, "cannot write the tuning script"); goto out; }
    struct wimlib_update_command add[3];
    memset(add, 0, sizeof add);
    add[0].op = WIMLIB_UPDATE_OP_ADD, add[0].add.fs_source_path = script, add[0].add.wim_target_path = "/Windows/System32/startnet.cmd";
    add[1].op = WIMLIB_UPDATE_OP_ADD, add[1].add.fs_source_path = answer, add[1].add.wim_target_path = "/Windows/System32/unattend_panther.xml";
    add[2].op = WIMLIB_UPDATE_OP_ADD, add[2].add.fs_source_path = tune, add[2].add.wim_target_path = "/Windows/System32/winmedia_tune.cmd";
    if ((err = wimlib_update_image(boot, 1, add, 3, 0))) { fail(job, "the start script cannot be put into Windows PE: %s", wimlib_get_error_string(err)); goto out; }
    if ((err = wimlib_overwrite(boot, 0, 0))) { fail(job, "boot.wim cannot be written with the start script: %s", wimlib_get_error_string(err)); goto out; }
    wimlib_free(boot), boot = NULL;
    f_mkdir("0:/sources");
    if (copy_in(job, path, "0:/sources/boot.wim", "Copying Windows PE")) goto out;
    unlink(path);

    /* Drivers Windows does not bring (the network card's), for Windows PE to add to the installed Windows. */
    if (job->drivers && *job->drivers) {
        f_mkdir("0:/drivers");
        if (copy_tree(job, job->drivers, "0:/drivers")) goto out;
        FIL f;
        if (f_open(&f, "0:/drivers/winmedia.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) f_close(&f);
        if (f_open(&f, "0:/drivers/winmedia_once.cmd", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            UINT wrote = 0;
            for (const char *c = once_script; *c; c++) { /* Windows's line ends */
                if (*c == '\n') f_write(&f, "\r", 1, &wrote);
                f_write(&f, c, 1, &wrote);
            }
            f_close(&f);
        }
    }

    /* And Windows itself, as it came. */
    wimlib_free(esd), esd = NULL;
    if (!job->service) {
        f_mkdir("1:/sources");
        if (copy_in(job, job->esd, "1:/sources/install.esd", "Copying Windows")) goto out;
    }
    say(job, "Done", 1);
    result = 0;

out:
    if (boot) wimlib_free(boot);
    if (esd) wimlib_free(esd);
    if (fs0) f_unmount("0:");
    if (fs1) f_unmount("1:");
    free(fs0), free(fs1), free(work);
    if (img_fd >= 0) {
        if (result == 0 && fsync(img_fd) != 0) result = fail(job, "the disk could not be written out: %s", strerror(errno));
        close(img_fd);
        img_fd = -1;
    }
    remove_tree(stage);
    if (result != 0) unlink(job->image);
    return result;
}

/* ---- the firmware's variables ---- */

/* vars.fd as EDK2 lays it out once the firmware has run: a firmware volume header (0x48 bytes here), a variable
   store header (28 bytes: GUID, size, 0x5a formatted, 0xfe healthy), then variables one after another, each a
   60-byte header (0x55aa, state, attributes, counter, time stamp, key index, name size, data size, vendor GUID),
   the name in UTF-16 and the data, the next one at a multiple of four. State 0x3f is a variable in use; free
   space is 0xff. PlatformConfig {width, height} is what OvmfPkg/PlatformDxe reads for the console's resolution. */
int winmedia_set_screen(const char *vars_fd, uint32_t width, uint32_t height) {
    static const uint8_t guid[16] = {0x1c, 0xc5, 0x35, 0x72, 0x80, 0x0c, 0xab, 0x4c, 0x87, 0xac, 0x3b, 0x08, 0x4a, 0x63, 0x04, 0xb1};
    static const char name8[] = "PlatformConfig";
    enum { HEAD = 60, NAME = 2 * sizeof name8, DATA = 8 };
    int fd = open(vars_fd, O_RDWR), result = -1;
    if (fd < 0) return -1;
    enum { MAX = 1 << 20 };
    uint8_t *d = malloc(MAX);
    ssize_t have = d ? pread(fd, d, MAX, 0) : -1;
    if (have >= 0x100 && !memcmp(d + 0x28, "_FVH", 4)) {
        uint32_t fvh = (uint32_t)(d[0x30] | d[0x31] << 8), store;
        memcpy(&store, d + fvh + 16, 4);
        size_t end = (size_t)fvh + store, p = (size_t)fvh + 28, found = 0;
        if (fvh + 28 < (size_t)have && d[fvh + 20] == 0x5a && d[fvh + 21] == 0xfe && end <= (size_t)have) {
            while (p + HEAD <= end && d[p] == 0xaa && d[p + 1] == 0x55) {
                uint32_t ns, ds;
                memcpy(&ns, d + p + 36, 4), memcpy(&ds, d + p + 40, 4);
                if (p + HEAD + (size_t)ns + ds > end) break;
                if (d[p + 2] == 0x3f && ns == NAME && ds == DATA && !memcmp(d + p + 44, guid, 16)) {
                    int same = 1;
                    for (size_t i = 0; i < sizeof name8; i++) same = same && d[p + HEAD + 2 * i] == (uint8_t)name8[i] && d[p + HEAD + 2 * i + 1] == 0;
                    if (same) found = p;
                }
                p = (p + HEAD + ns + ds + 3) & ~(size_t)3;
            }
            uint8_t rec[HEAD + NAME + DATA];
            memset(rec, 0, sizeof rec);
            rec[0] = 0xaa, rec[1] = 0x55, rec[2] = 0x3f;
            rec[4] = 3; /* non-volatile, boot service access */
            uint32_t ns = NAME, ds = DATA;
            memcpy(rec + 36, &ns, 4), memcpy(rec + 40, &ds, 4), memcpy(rec + 44, guid, 16);
            for (size_t i = 0; i < sizeof name8; i++) rec[HEAD + 2 * i] = (uint8_t)name8[i];
            memcpy(rec + HEAD + NAME, &width, 4), memcpy(rec + HEAD + NAME + 4, &height, 4);
            if (found) { /* there already: only its eight bytes change */
                result = pwrite(fd, rec + HEAD + NAME, DATA, (off_t)(found + HEAD + NAME)) == DATA ? 0 : -1;
            } else if (p + sizeof rec <= end) {
                int blank = 1;
                for (size_t i = 0; i < sizeof rec; i++) blank = blank && d[p + i] == 0xff;
                if (blank) result = pwrite(fd, rec, sizeof rec, (off_t)p) == (ssize_t)sizeof rec ? 0 : -1;
            }
        } else {
            result = 1;
        }
    } else if (have >= 0) {
        result = 1; /* not formatted yet: the firmware does that at its first start */
    }
    free(d);
    if (result == 0) fsync(fd);
    close(fd);
    return result;
}

#ifdef WINMEDIA_MAIN
static void show(void *ctx, const char *step, double fraction) {
    static char last[64];
    static int last_pct = -2;
    (void)ctx;
    int pct = fraction < 0 ? -1 : (int)(fraction * 100);
    if (!strcmp(last, step) && pct / 10 == last_pct / 10) return;
    snprintf(last, sizeof last, "%s", step);
    last_pct = pct;
    if (pct < 0) fprintf(stderr, "%s\n", step);
    else fprintf(stderr, "%s: %d%%\n", step, pct);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "editions")) {
        static char list[8192];
        int n = winmedia_editions(argv[2], list, sizeof list);
        if (n < 0) return fprintf(stderr, "cannot read %s\n", argv[2]), 1;
        fputs(list, stdout);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "screen")) {
        int r = winmedia_set_screen(argv[2], (uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4]));
        fprintf(stderr, "screen: %s\n", r == 0 ? "set" : r == 1 ? "no variable store yet" : "failed");
        return r < 0;
    }
    if (argc < 4) return fprintf(stderr, "usage: %s ESD IMAGE TMPDIR [EDITION [USER [DRIVERS [lean]]]]\n       %s editions ESD\n       %s screen VARS.fd W H\n", argv[0], argv[0], argv[0]), 2;
    WinMediaJob job = {.esd = argv[1], .image = argv[2], .tmpdir = argv[3], .edition = argc > 4 ? argv[4] : NULL,
                       .user = argc > 5 ? argv[5] : NULL, .drivers = argc > 6 && *argv[6] ? argv[6] : NULL, .lean = argc > 7 && !strcmp(argv[7], "lean"), .service = argc > 4 && !strcmp(argv[4], "service"), .progress = show};
    if (job.service) job.edition = NULL;
    if (winmedia_build(&job) != 0) return fprintf(stderr, "winmedia: %s\n", job.error), 1;
    fprintf(stderr, "winmedia: %s (%s), image %d, on %s\n", job.edition_name, job.language, job.edition_index, argv[2]);
    return 0;
}
#endif
