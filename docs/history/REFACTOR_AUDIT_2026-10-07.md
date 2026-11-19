# Aufräum- und Refactoring-Prüfung vom 7. Oktober 2026

**Umgesetzt:** Die sechs Schritte aus der empfohlenen Reihenfolge sind
abgearbeitet und im Container auf fuzzy mit Nix geprüft. Der
[Umsetzungsbericht](REFACTOR_IMPLEMENTATION_2026-10-07.md)
beschreibt Änderungen und Validierung. Die folgenden Befunde dokumentieren den
Arbeitsbaum vor dieser Umsetzung; die damaligen Hinweise auf noch ausstehende
Löschungen und Builds gehören zu diesem ursprünglichen Prüfstand.

Geprüft wurde der aktuelle Arbeitsbaum von thunderbolt-ibverbs: Kernel,
Protokollhelfer, Userspace-Provider, Benchmarks, Build-, Packaging- und
CI-Einstiegspunkte. Gufo war nicht Gegenstand dieser projektweiten Prüfung.
Es gab keine Hardwarezugriffe, Linux-Kernelbuilds oder Performanceversuche.

Die Prüfung kombiniert projektweite Referenzsuchen mit gezielter Prüfung
der Aufrufer, Callbacks, Zustandsverwaltung und Build-Einstiegspunkte.
Sie ist keine vollständige Nebenläufigkeits- oder Speicherfehleranalyse.
Bereits vorhandene, nicht zu diesem Auftrag gehörende Änderungen bleiben
erhalten. Historische Messberichte bleiben als Nachweise erhalten.

## 1. Vor dem nächsten Build: Nix-Quellfilter vervollständigen

**Hohe Priorität; konkreter Buildfehler im aktuellen Quellstand.**

[nix/module.nix](/Users/user/Documents/thunderbolt-ibverbs/nix/module.nix:16)
nimmt nur `Makefile`, `kernel/` und `proto/` in den Modulquellbaum auf.
[kernel/gpu_direct.c](/Users/user/Documents/thunderbolt-ibverbs/kernel/gpu_direct.c:18)
bindet jedoch `../include/tbv_gpu_direct.h` ein und wird laut
[Kernel-Makefile](/Users/user/Documents/thunderbolt-ibverbs/kernel/Makefile:10)
unbedingt mitgebaut. Der Header fehlt daher im gefilterten Nix-Quellbaum,
auch wenn `gpu_direct=0` ist.

Die DKMS-Paketierung nimmt `include/` bereits auf. Hier sind zwei
unterschiedliche Listen der benötigten Quellen auseinandergewachsen.
Zunächst `include/` in den Nix-Filter aufnehmen; anschließend die benötigten
Quellen zentral beschreiben oder ihre Vollständigkeit automatisch prüfen.
Ein lokaler Vergleich der relativen, mit Anführungszeichen eingebundenen
Kernel-Header gegen den Filter bestätigt genau diesen ausgelassenen Header.
Ein tatsächlicher Nix-/Linux-Build steht noch aus.

## 2. Sicherer Löschkandidat: fünf interne Funktionen

**Kleine, isolierte Bereinigung.** Die folgenden Namen kommen im aktuellen
Produktionscode jeweils nur in Definition und Headerdeklaration vor.
Es gibt weder Aufrufer noch registrierte Funktionszeiger oder
`EXPORT_SYMBOL`-Exporte dafür.

| Funktion | Fundstelle | Warum entbehrlich |
| --- | --- | --- |
| `tbv_native_control_exchange` | [native_control.c](/Users/user/Documents/thunderbolt-ibverbs/kernel/native_control.c:729) | Ungenutzter synchroner HELLO-Retry-Wrapper. Der aktive Worker ruft weiterhin `tbv_native_control_exchange_once` auf; dieser Helper bleibt erforderlich. |
| `tbv_service_create_native_dir` | [service.c](/Users/user/Documents/thunderbolt-ibverbs/kernel/service.c:87) | Ungenutzter Property-Verzeichnis-Wrapper. Die gemeinsame Property-Erzeugung bleibt erforderlich. |
| `tbv_path_reset` | [path.c](/Users/user/Documents/thunderbolt-ibverbs/kernel/path.c:626) | Ungenutzte zweite Initialisierung eines Pfades; dupliziert große Teile von `tbv_path_init`. |
| `tbv_path_send_owned_list_reserved` | [path.c](/Users/user/Documents/thunderbolt-ibverbs/kernel/path.c:2326) | Ungenutzter Wrapper um die weiterhin verwendeten getrennten Prepare-/Enqueue-Schritte. |
| `tbv_tbnet_minimal_neighbor_ready` | [tbnet_minimal.c](/Users/user/Documents/thunderbolt-ibverbs/kernel/tbnet_minimal.c:491) | Ungenutzter Neighbor-Getter. Die verwendete Pfadbereitschaft und die Neighbor-Zustandsverwaltung bleiben bestehen. |

Das sind **100 Zeilen Funktionskörper**, zuzüglich der Deklarationen in
[tbv.h](/Users/user/Documents/thunderbolt-ibverbs/kernel/tbv.h:768).
Zusätzlich sind das private Makro `TBV_TBIP_HDR_LENGTH_MASK` in
[tbnet_identity.c](/Users/user/Documents/thunderbolt-ibverbs/kernel/tbnet_identity.c:33)
und die private Nix-Bindung `forDarwinSystems` in
[flake.nix](/Users/user/Documents/thunderbolt-ibverbs/flake.nix:33)
ohne Verwendung.

Diese Löschungen sind hier noch nicht durchgeführt. Sie sollten als eigener
kleiner Änderungsschritt erfolgen, mit abschließender Referenzsuche und
anschließendem Kernelbuild. Aus nur einer textuellen Verwendung folgt nicht
generell toter Code: Die `debugfs`-Show-Funktionen werden beispielsweise über
`DEFINE_SHOW_ATTRIBUTE` verwendet und gehören ausdrücklich nicht dazu.

## 3. Transaktionsverwaltung und NHI-Ringspeicher trennen

**Sinnvolles erstes strukturelles Refactoring.**

[tbv_control](/Users/user/Documents/thunderbolt-ibverbs/kernel/verbs_control.c:73)
enthält gleichzeitig die DONE-/Antwort-Transaktionen und alle TX-/RX-Frames
samt RX-Ereignispool für den separaten NHI-Kontrollring.
[tbv_ctrl_alloc](/Users/user/Documents/thunderbolt-ibverbs/kernel/verbs_control.c:73)
alloziert dieses vollständige Objekt auch im XDomain-Modus, obwohl dieser
vor der Einrichtung der Ring-Pools zurückkehrt. Allein die ungenutzten
RX-Ereignis-Payloads belegen dann 64 × 256 Byte = 16 KiB pro Objekt; weitere
Listen-, Frame- und Ringspeicher-Metadaten kommen hinzu.

Empfehlung: einen gemeinsamen Transaktionstracker und ein nur im Ring-Modus
alloziertes `tbv_control_ring`-Objekt. Der kref-basierte Tracker muss weiterhin
einen Abbau überleben können, wenn ein RX-Batch auf einem anderen Link noch
seine Referenz hält. Das Refactoring sollte zuerst die Speicheraufteilung
ändern und den bestehenden Abschluss- und Fehlervertrag beibehalten.
Die vorhandenen DONE-Zustandsprüfungen sind dafür ein Ausgangspunkt;
Stop-/Drain-/Disconnect-Prüfungen bleiben zusätzlich nötig.

## 4. Verbs-Datei nach Verantwortlichkeiten aufteilen

**Größter Wartungsgewinn, aber in kleinen Schritten.**

[ibdev.c](/Users/user/Documents/thunderbolt-ibverbs/kernel/ibdev.c:1)
hat knapp 11.000 Zeilen. Darin liegen Device-Registrierung, QP-/MR-/CQ-
Lebensdauer, native SEND/WRITE/READ-Verarbeitung, ACK-/RNR-/Timeout-Logik,
Reassembly, Apple-TX/RX und DMA-BUF-Verbs-Anbindung. So können Änderungen
an einer dieser Aufgaben schwer gegen die übrigen abgegrenzt werden.

Mit den CQ- und MR-Operationen beginnen, danach Apple-TX/RX und native
Reassembly als getrennte Einheiten herauslösen. Ein privater Verbs-Header
sollte die dafür tatsächlich gemeinsam benötigten Objekte und Verträge
tragen; nicht alle bisherigen `static`-Funktionen öffentlich machen.
Jeder Schritt muss Referenzbesitz, Lock-Reihenfolge, geordnete Completion-
Ausgabe und verzögerte Freigaben unverändert erhalten. Die bestehende
Dateilänge allein ist kein Grund für einen großen gleichzeitigen Umbau.

## 5. DMA-BUF-Zustand in einem Engine-Objekt zusammenfassen

**Zunächst Struktur verbessern; Parallelisierung separat behandeln.**

[verbs_dmabuf.c](/Users/user/Documents/thunderbolt-ibverbs/kernel/verbs_dmabuf.c:209)
verwaltet TX-Queues, RX-Transaktion, aktiven TX-Kontext, Locks, Watchdog und
Completion-Arbeit als modulweite Globals. TX benutzt eine geordnete
Workqueue; RX hat einen einzigen gemeinsamen aktiven Zustand. Die RX-
Booleans `active`, `armed` und `cached` verteilen die Zustandsübergänge über
PREPARE, Reap, Finish und Fehlerbehandlung.

Zuerst diese Daten in einem `tbv_dmabuf_engine` zusammenfassen und die
zulässigen RX-Zustände und Übergänge ausdrücklich modellieren. Das lässt
sich bei gleichem Serialisierungsverhalten besser prüfen. Mehrere Engines
oder parallele RX-WRs wären eine eigenständige Protokolländerung: Rohframes
haben keine Operations-ID, und ein Batch kann Rails mehrerer physischer
Links zum selben entfernten Host verwenden. Einfach einen Kontext pro QP
oder physischem XDomain einzuführen wäre daher unzureichend.

## 6. Configfs-Transportabstraktion vereinfachen

**Mittlere Priorität; keine unbenutzte Funktion, aber leere Abstraktion.**

Die Activate-/Deactivate-Hooks in
[native.c](/Users/user/Documents/thunderbolt-ibverbs/kernel/native.c:30) und
[apple.c](/Users/user/Documents/thunderbolt-ibverbs/kernel/apple.c:28)
geben nur Logmeldungen aus. Dennoch behandelt
[configfs.c](/Users/user/Documents/thunderbolt-ibverbs/kernel/configfs.c:391)
sie wie eine Transportaktivierung mit eigenem Rollback. Die tatsächliche
Configfs-Aktivierung ändert den Konfigurationszustand und veröffentlicht
einen Eintrag in `configured_links`; diese Hooks richten keine NHI-Pfade ein.

Gemeinsame Configfs-Veröffentlichung und Backend-Validierung direkt
ausdrücken. Entweder die reinen Logging-Hooks entfernen oder ihnen eine
konkrete Transportaufgabe geben. Die Benutzeroberfläche und vorhandenen
Configfs-VM-Tests dabei erhalten; Configfs selbst ist kein toter Code.

## 7. Provider-Patchpflege zuverlässig und reproduzierbar machen

**Mittlere Priorität; zwei konkrete Inkonsistenzen.**

[regen-rdma-core-patches.sh](/Users/user/Documents/thunderbolt-ibverbs/packaging/regen-rdma-core-patches.sh:15)
fragt zuerst `legacyPackages.x86_64-linux.linux_thunderbolt.outPath` ab.
Dieses Exportat existiert in der aktuellen Flake nicht. Der Fallback benutzt
`<nixpkgs>`, statt die gesperrte Flake-Eingabe zu verwenden. Außerdem ist das
Kopieren mit `cp -r "$RDMA_SRC"/.` nur für ein Verzeichnis geeignet, obwohl
das Skript auch eine Tarball-Quelle beschreibt.

[test-rdma-patches.sh](/Users/user/Documents/thunderbolt-ibverbs/packaging/test-rdma-patches.sh:63)
wendet nur Patch 0001 und 0002 an. Ausgeliefert werden drei Patches; 0003
deklariert den Provider für den statischen Buildpfad. Der Test prüft somit
nicht den vollständigen ausgelieferten Patchsatz.

Eine Quelle und eine Patchliste für Regeneration, Nix, Distro-Paketierung
und Tests verwenden. Die Regeneration muss Verzeichnis und Archiv korrekt
behandeln. Der Test sollte sämtliche Patches anwenden und danach die fünf
Provider-Dateien mit `userspace/usb4_rdma/` vergleichen. Das lokale
Kommentar-Update besteht diesen letzten Vergleich bereits.

## 8. Lokale Zustandsprüfungen in CI aufnehmen

**Mittlere Priorität; wichtig vor weiterem Refactoring.**

Die Produktionsfunktionen prüfenden Skripte
`tools/ci/verbs-control-state-smoke.py` und
`tools/ci/verbs-dma-wait-smoke.py` sind bisher nicht in den Nix- und Distro-
Smoke-Einstiegspunkten registriert. Die Listen der C-Protokolltests werden
außerdem in [flake.nix](/Users/user/Documents/thunderbolt-ibverbs/flake.nix:156)
und [distro-build.sh](/Users/user/Documents/thunderbolt-ibverbs/tools/ci/distro-build.sh:119)
separat gepflegt. Der Nix-Syntaxcheck nennt ebenfalls nur eine Teilmenge der
Shell- und Python-Dateien.

Einen gemeinsamen Smoke-Einstiegspunkt mit klaren Compiler-/Sanitizer-
Abhängigkeiten einführen und diese beiden Zustandsprüfungen darin
registrieren. Nur die Dateinamen in die bestehenden Listen einzutragen
reicht nicht: Die Python-Prüfungen starten derzeit ausdrücklich Clang,
während die Distro-Abhängigkeiten GCC installieren.

## 9. Kleine gemeinsame Benchmark-Helfer statt weiterer Kopien

**Niedrigere Priorität; konkrete Duplikate vorhanden.**

In den C-Verifiern sind `now_ns`, `send_all` und `recv_all` jeweils in fünf
Programmen identisch. `tcp_listen` und `tcp_connect` sind jeweils in vier
Programmen identisch. Betroffen sind `rc_write_imm_verify`, `rc_write_poll`,
`rc_write_verify`, `uc_write_verify` und teilweise `uc_oneway`.

Diese kleinen Helfer gemeinsam halten; die unterschiedlichen RC-/UC-
Prüfprotokolle und GPU-Verifikationen bleiben eigenständige Tests. Beim
Einführen eines gemeinsamen Headers auch den Nix-Bench-Quellfilter
anpassen: [bench-tools.nix](/Users/user/Documents/thunderbolt-ibverbs/nix/bench-tools.nix:54)
übernimmt derzeit nur `.c`, `.py` und `.sh`.

Die Python-Sweeps duplizieren SSH-Ausführung und perftest-Ausgabeparser.
Auch dort zuerst kleine reine Parser-/Formatierungshelfer teilen. Die
lokale README-Chart-Erzeugung und der Linux-/Darwin-Runner haben verschiedene
Aufgaben; die bloße Existenz mehrerer Sweeps rechtfertigt keine Löschung.

## Bereits bereinigte Kommentare und Beschreibungen

- Provider: `Tier 1 skeleton`, angebliche generelle `-ENOSYS`-Rückgaben und
  `query stubs` entfernt. Der Kommentar beschreibt jetzt die vorhandenen
  QP-/MR-/DMA-BUF-/CQ-Kommandos und die tatsächliche Gerätezuordnung.
- Provider-ABI: spekulative künftige UAPI-Erweiterung entfernt; die aktuelle
  Nutzung generischer uverbs-Nachrichten bleibt beschrieben.
- `register_verbs`: veraltete Skeleton-Beschreibung durch die tatsächliche
  Gerätefunktion ersetzt. Nur die Parameterbeschreibung änderte sich.
- Patch-Regeneration: Kommentar von zwei auf drei erzeugte Patches korrigiert.
- Die Provider-Kommentare im ausgelieferten Patch 0001 synchronisiert,
  einschließlich Hunk-Längen, Blob-Hashes und Einfügungsstatistik.

Beim Provider und ABI-Header sind die Inhalte nach Entfernen der Kommentare
vorher/nachher identisch. Patch 0001 lässt sich in einem leeren temporären
Quellbaum anwenden; alle fünf erzeugten Provider-Dateien stimmen danach
bytegenau mit ihren Quellen überein. Whitespace-Prüfung sowie Syntaxprüfung
von 14 Shell-Skripten und zehn Python-Dateien bestehen. Laufzeitverhalten
wurde in diesem Auftrag nicht geändert.

## Was weiterhin benötigt wird

Legacy-XDomain-Callbacks, alte rdma-core-Signaturen, Apple-Transport und
TBnet-Identität sind verwendete Kompatibilitätspfade. `gpu_direct` und der
separate NHI-Kontrollring sind weiterhin auswählbare Experimente. Hinweise
zu unsicherem Apple-RAW-RX, Raw-DMA-Quarantäne und fehlenden Interrupts
beschreiben vorhandene Einschränkungen und bleiben stehen. Historische
Benchmarkberichte sind keine aktuellen TODO-Kommentare.

Der einzelne Trace-Patch
`kernel-workflow/patches/0010-thunderbolt-trace-XDomain-route-matching.patch`
ist nicht in den aktiven Patchstacks referenziert. Seine Rolle als manuelles
Diagnosewerkzeug sollte dokumentiert oder der Patch in ein Diagnosearchiv
verschoben werden; allein daraus folgt keine sichere Löschentscheidung.

## Empfohlene Reihenfolge

1. Nix-Quellfilter und vollständige Patchsatzprüfung korrigieren.
2. Die fünf ungenutzten Funktionen, das private Makro und die Nix-Bindung
   entfernen; die Zustandsprüfungen als gemeinsame CI-Einstiegspunkte sichern.
3. Den DONE-Tracker vom Ringspeicher trennen.
4. MR/CQ-Verantwortlichkeiten aus `ibdev.c` herauslösen.
5. DMA-BUF-Zustände und Configfs-Veröffentlichung vereinfachen.
6. Benchmark-Helfer vereinheitlichen.

Die architektonischen Änderungen gehören in getrennte überprüfbare Schritte.
Ein lokaler Kontrollflusscheck ersetzt anschließend weder den Linux-Build
noch die ausstehende DMA-/Disconnect- und Performancequalifikation.
