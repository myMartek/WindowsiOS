# Windows auf visionOS: Stand der Arbeit (visionmac)

Ziel: Windows 11 für ARM in derselben App wie macOS (Henge JIT auf der Vision Pro), vom Download bei Microsoft bis
zum ersten Desktop, ohne Mac dazwischen.

Stand 2026-11-13 abends:

| Stufe | Ergebnis |
|---|---|
| Ablauf in der VM auf dem Mac (`build/winvm`) | durchgelaufen: 21 Minuten vom leeren Datenträger bis zum Herunterfahren, danach startet Windows 11 Pro (Deutsch) bis zum Desktop (`work/run/desktop.png`) |
| App im visionOS-Simulator | Katalog, Sprachwahl, Medium bauen (rund 90 s), PC starten, Bild im Sitzungsfenster: gesehen. Ganze Installation dort: läuft noch |
| Brille | App mit Windows-Teil installiert, JIT steht. Download, Medium und Installation dort noch nicht gelaufen |

## Was dieser Ordner ist

`WindowsiOS/` ist ein Klon von ammaarreshi/WindowsiOS (GPL-2.0, Stand 0071d69) mit Änderungen im Arbeitsstand:

| Datei | Zweck |
|---|---|
| `Makefile` (Ende) | Ziel `xros-lib`: Engine und Geräte als **ein** Objekt, sichtbar sind nur `hv_winvm_*` und `winmedia_*` |
| `host/winvm.c` (Ende) | schlanke Schnittstelle `hv_winvm_x_*` für die App (`xros/winvm_xros.h`, nur einfache Typen) |
| `xros/winmedia/` | Installationsmedium aus dem ESD von Microsoft bauen: libwim + FatFs, läuft auf Mac und Gerät |
| `xros/build_wimlib_xros.sh` | wimlib 1.14.4 als statische Bibliothek für visionOS |
| `work/` | Downloads, Mac-Abhängigkeiten, Testläufe (nicht im Git) |

Warum ein Objekt: WindowsiOS ist Henges Engine mit 4-KB-Seiten (`HV_TLB_PAGE_BITS 12`) und anderer Maschine. Gleiche
Namen, andere Strukturen. `ld -r -exported_symbols_list` macht alles Übrige lokal; `-fno-common` ist dafür nötig,
sonst bleiben Variablen wie `hv_knob` sichtbar. `host/prof_ios.c` bleibt draußen.

## Ablauf der Installation

1. Katalog: `https://go.microsoft.com/fwlink/?LinkId=2156292` liefert `products.cab` (LZX) mit `products.xml`.
   Darin je Sprache ein ESD für ARM64 (`CLIENTCONSUMER_RET`), mit Größe, SHA-1 und Adresse auf
   `dl.delivery.mp.microsoft.com`. Der Server spricht nur HTTP, kein HTTPS.
2. `winmedia_build`: Platte mit MBR, Partition 1 FAT32 „WINPE“ (Bootdateien aus Abbild 1, `sources\boot.wim` aus dem
   Windows-PE-Abbild mit eigenem `startnet.cmd` und Antwortdatei), Partition 2 exFAT „WINESD“ mit dem ESD selbst.
   Auf dem Mac 27 Sekunden. Der MBR braucht eine Datenträgersignatur, sonst zeigt Windows auf der
   schreibgeschützten Platte kein Volume.
3. VM mit leerer Platte und Installationsmedium: Windows PE findet die Zielplatte über die Volume-Namen
   (sprachunabhängig), partitioniert, `dism /Apply-Image` direkt aus dem ESD, `bcdboot`, Neustart.
4. Windows richtet sich nach der Antwortdatei ein (lokales Konto ohne Passwort, Sprache aus dem ESD), meldet sich
   einmal an und fährt herunter. Das ist das Ende der Installation; die App löscht dann das Installationsmedium.

## Auf dem Mac nachstellen

```bash
work/build_macdeps.sh                 # libslirp für den Mac
make build/winvm build/hvsparse SLIRP_CFLAGS=… SLIRP_LIBS=…   # siehe work/run/run_install.sh
build/winmedia ESD work/run/installer.img work/run/tmp Professional User
work/run/run_install.sh               # ganze Installation mit Bildschirmfotos
```

## Erster Lauf auf der Brille (2026-11-13) und was daraus folgte

Die ganze Kette lief auf der Vision Pro durch: Download bei Microsoft (zwei Abbrüche, jeweils fortgesetzt),
Prüfsumme, Medium in 87 s, Installation in 20,5 Minuten. Rückmeldung des Nutzers: Auflösung niedrig, Netzwerk ging
nicht, nicht schnell.

- **Tempo:** Das Protokoll zeigt die Ursache. Nach der Installation war die Brille heiß, drei Minuten nach dem
  Start von Windows stand sie auf „Critical“, und die Übersetzungsleistung fiel von 350000 auf 28000 Blöcke pro
  Sekunde. Dagegen: `winmedia_tune.cmd` (in Windows PE, nach DISM) schaltet Hintergrundarbeit ab, per `reg add` in
  die Hives des frisch aufgespielten Windows. Liste aus WindowsiOS (`scripts/optimize_windows_lite.py`), geteilt:
  immer aus sind Suchindex, SysMain, Telemetrie, Xbox-Dienste, Widgets, Copilot, Edge-Hintergrund, Animationen
  und Transparenz. Nur auf Wunsch (`WinMediaJob.lean`, in der App ein Schalter, Standard aus) auch Defender,
  SmartScreen, Windows Update und die Spectre-Abschwächungen: größter Gewinn, aber echter Schutzverlust.
- **Auflösung:** Windows behält den Modus, in dem die Firmware startet. Der ramfb-Treiber von EDK2
  (QemuRamfbDxe) kennt nur 640×480, 800×600 und 1024×768. `xros/patch_firmware.py` ersetzt diese drei im
  Firmware-Abbild durch 1280×720, 1600×900 und 1920×1080 (DXE-Volume entpacken, ändern, neu packen, auf alte
  Länge mit Nullen auffüllen: keine Größe und keine Prüfsumme ändert sich). Ohne Vorgabe startet sie dann in
  1600×900. Welcher Modus gilt, sagt die UEFI-Variable `PlatformConfig` (GUID 7235c51c-…, zwei UINT32). Sie wird
  direkt in `vars.fd` geschrieben (`winmedia_set_screen`, Format steht im Quelltext), vor jedem Start aus der
  Bildschirmwahl des PCs. Auf dem Mac geprüft: 1280×720 und 1920×1080 kommen an.
- **Netzwerk:** Ursache gefunden und behoben. Windows hatte gar keine Netzwerkkarte (`ipconfig` leer, Gerät
  `PCI\VEN_1AF4&DEV_1041` mit Problemcode 28): `dism /Add-Driver` bricht in Windows PE gegen dieses Abbild mit
  0x80004002 „Schnittstelle nicht unterstützt“ ab (stand in `winmedia.log`). Jetzt kopiert Windows PE die
  Treiberdateien nach `\Drivers` der Windows-Platte, und Windows bindet sie selbst mit `pnputil` ein: bei einer
  Neuinstallation im specialize-Durchlauf der Antwortdatei, beim Nachrüsten über `Setup\CmdLine` mit
  `SetupType 2` (läuft einmal als System vor der Anmeldung und stellt beides zurück). Geprüft: Ping an 1.1.1.1
  und an microsoft.com aus beiden Systemen.
- **Servicemodus** (`WinMediaJob.service`): Medium nur mit Windows PE, das ein installiertes Windows nachrüstet
  (Treiber, Entlastungen, gemerkte Bildschirmgröße löschen) und herunterfährt. Dafür muss die Firmware das Medium
  vor Windows starten: `hv_winvm_x_boot(…, installer_first)` tauscht die Platten (die Windows-Platte hängt dann
  beschreibbar an zweiter Stelle, `HV_DISK1_RW`) und die App gibt für den Lauf frische Firmware-Variablen mit.
  In der App: Einstellungen des PCs, Karte „Network and speed“, dann den PC einmal starten.
  Geprüft an der von der Brille kopierten VM: danach Netz und volle 1600 × 900.
- **Nachrüsten von selbst, in einem Durchgang** (2026-11-14): Der Nutzer startete Windows nach dem Update
  direkt und fand die Netzwerkkarte ohne Treiber: Den Extraschritt in den Einstellungen hatte er nicht gemacht
  (im Protokoll kein Servicelauf, `media-tmp` unberührt). Ein Schritt, den man übersehen kann, ist ein Fehler im
  Ablauf. Jetzt merkt sich die App je PC einen Stand (`winmedia.level`, aktuell 2). Liegt er darunter, baut
  `SessionView.run` beim Start das Servicemedium selbst und startet damit. Das Servicemedium benennt am Ende
  seinen eigenen Bootloader um und startet neu; die Firmware findet darauf nichts mehr und startet Windows
  (`Windows Boot Manager`) im selben Lauf. Die App erkennt das am Neustart-Zähler (`henge_vm_restarts`) und räumt
  beim Ende der Sitzung auf. An der Kopie der Brillen-VM geprüft: 1:38 min Service, danach Windows mit Netz und
  1600 × 900, ohne App-Neustart.
- **Wo getestet:** auf einem zweiten Mac per SSH (MacBook Pro, Ordner `~/visionmac-test`), weil der Mac mini mit
  zwei VMs überlastet war. Dort dauert eine Installation 11 Minuten.
  Die VM der Brille kam per `devicectl … copy from` (64 GB roh, 35 Minuten) und gepackt mit `hvsparse` hinüber.
- Die App schreibt jetzt, ob die Netzwerkkarte ein Netz hinter sich hat, und zählt Pakete im `[perf]`-Protokoll.
- **Fehler gefunden:** Windows PE hat kein `findstr`. Die Erkennung der Zielplatte nahm deshalb einfach die erste
  Platte; das war bisher zufällig die richtige. Jetzt mit `find`.

## Bekannte Grenzen

- Abbau der Windows-VM (2026-11-14, Wunsch des Nutzers): `hv_winvm_destroy` gibt Arbeitsspeicher, Firmware-
  Abbilder, Platten, Netzwerk-Thread (`hv_net_detach`), Bildausgabe (`hv_fb_metal_stop`) und Signal-Handler
  zurück; die App ruft es aus dem Ende-Rückruf und kann danach im selben Start wieder eine Maschine starten.
  Die kleinen Strukturen der Geräte bleiben stehen (sie haben keine Freigabefunktionen). Test:
  `build/test_cycle` baut, startet, stoppt und baut ab, mehrfach in einem Prozess. Gemessen auf dem Mac: 4 Runden
  nur Firmware und 3 Runden mit Windows (einmal sauber heruntergefahren), jedes Mal mit Netz; Speicher nach dem
  Abbau 0,7 GB nach Runde 1, dann rund 1,6 GB und etwa 50 MB mehr je Runde. Auf der Brille noch nicht gelaufen.
- Auflösung: drei Modi (1280×720, 1600×900, 1920×1080), wählbar in den Einstellungen des PCs. Windows zeichnet
  ohne Grafikkarte, größer ist langsamer.
- Netzwerk: der NetKVM-Treiber (virtio-win 0.1.302, `NetKVM/w11/ARM64`) liegt in der App und kommt auf das
  Installationsmedium; Windows PE fügt ihn mit DISM hinzu. Ob Windows damit Netz hat, ist noch nicht geprüft
  (der Lauf auf dem Mac war ohne Treiber und ohne Netz).
- Im Simulator fand sich ein Fehler mit Datenverlust: Die Maschinenliste kannte nur `disk.img` und räumte den
  Ordner eines Windows-PCs beim nächsten Start weg. Behoben (`VirtualMac.disk`).
- Firmware: `edk2-aarch64-code.fd` und `edk2-arm-vars.fd` aus QEMU 9.2.0, in der App als bz2.
