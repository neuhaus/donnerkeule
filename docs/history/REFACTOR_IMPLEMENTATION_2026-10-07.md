# Umsetzung der Aufräumarbeiten vom 7. Oktober 2026

Die sechs Schritte aus der empfohlenen Reihenfolge sind umgesetzt. Ausgangspunkt
war der bereits geänderte Arbeitsbaum einschließlich der DONE- und Polling-Arbeit.
Gufo war nicht Teil dieses Auftrags. Die Umsetzung verwendet eigene Quellkopien
im vorhandenen `nixbox`-Container auf fuzzy; es wurde kein Hardwaretest durchgeführt.

Die später freigegebene [Hardwareprüfung vom 8. Oktober](../../bench/results/2026-10-08-hardware.md)
prüfte den aufgeräumten Stand mit Host- und GPU-MRs sowie DONE-Fehlerfällen.
Dieser Bericht dokumentiert weiterhin die ursprüngliche Build- und Refactoringrunde.

## 1. Quellen und Provider-Patches

- Der Modulfilter in `nix/module.nix` enthält jetzt `include/`. Eine Prüfung der
  lokalen Headerabhängigkeiten sichert die Vollständigkeit des gefilterten Baums.
- `packaging/rdma-core-patches/series` ist die gemeinsame Reihenfolge für Nix,
  Distro-Paketierung und Patchtests. Alle drei Patches werden angewendet.
- Regeneration und der Distro-Patchtest beziehen ihre Standardquelle aus dem
  gesperrten Flake-Eingang über `rdma-core-source`. Beide können mit `RDMA_SRC`
  auch ein Verzeichnis oder ein Archiv verwenden. Ein ausdrücklich gesetztes
  `RDMA_CORE_TAG` ermöglicht weiterhin Tests anderer Upstream-Versionen.
- Regeneration prüft die Upstream-Anker und erzeugten Dateinamen, bevor sie die
  drei ausgelieferten Patches ersetzt. Die Fixture-Prüfung umfasst Verzeichnis,
  Archiv, Container-Staging und einen Fehler vor dem Ersetzen der Patches.
- Die Provider-Dateien aus Patch 0001 werden bytegenau mit ihren Quellen
  verglichen. Der Distro-Test baut mit `ENABLE_STATIC=1`; dafür gibt es zusätzlich
  den Nix-Check `checks.x86_64-linux.rdma-core-static`.

## 2. Toter Code und gemeinsame CI-Einstiege

Entfernt wurden die Definitionen und Deklarationen von
`tbv_native_control_exchange`, `tbv_service_create_native_dir`, `tbv_path_reset`,
`tbv_path_send_owned_list_reserved` und `tbv_tbnet_minimal_neighbor_ready`.
Das sind 100 Zeilen Funktionskörper zuzüglich der Deklarationen. Entfernt sind
außerdem `TBV_TBIP_HDR_LENGTH_MASK` und `forDarwinSystems`.

`tools/ci/run-smoke.py` ist der gemeinsame Einstieg für Nix und Distro-Builds.
Er führt sechs C-Prüfungen und acht Python-Prüfungen aus, einschließlich der
Produktionsfunktionen für DONE und DMA-Wartezeiten. Die C-Prüfungen verwenden
ASan/UBSan und den konfigurierten Compiler über `CC`. Die Distro-Abhängigkeiten
enthalten Python und die benötigten Sanitizer-Laufzeitbibliotheken.
`tools/ci/check-syntax.py` prüft die 32 gepflegten Shell- und Python-Skripte.

Der bislang unreferenzierte Route-Matching-Trace-Patch ist in der Patch-README
als manuelles Diagnosewerkzeug dokumentiert.

## 3. DONE-Tracker und Ringspeicher

`tbv_control` enthält die gemeinsamen Transaktionen und deren Lebensdauer.
`tbv_control_ring` enthält die NHI-Ringe, DMA-Frame-Metadaten, Ereignispools und
Ring-Worker. Dieses zweite Objekt wird ausschließlich für den separaten
Ringtransport angelegt. Damit entfallen im XDomain-Modus unter anderem die
16 KiB RX-Ereignispayloads und sämtliche Frame-Arrays des Ringtransports.

Nach Stop und Drain wird der Ringspeicher freigegeben. Eine von RX auf einem
anderen Link gehaltene Referenz kann den kleinen Tracker bis zu ihrem letzten
Abschluss erhalten. Eine neue Produktionsfunktionsprüfung deckt beide
Transportarten, wiederholten Stop, Drain, diese gehaltene Referenz sowie die
Freigabe bei unvollständiger Allokation ab.

## 4. MR und CQ

21 Funktionen liegen jetzt in `kernel/verbs_mr.c` und `kernel/verbs_cq.c`.
`kernel/verbs_internal.h` beschreibt die gemeinsam benötigten privaten Layouts
und Aufrufverträge. QP-Layout und QP-Fehlerreferenzen verbleiben bei der QP-
Verwaltung; der CQ-Überlauf ruft dafür eine kleine interne Brücke auf.
`ibdev.c` schrumpft von 10.946 auf 10.358 Zeilen.

Ein Vergleich der C-Tokens bestätigt identische verschobene Funktionskörper,
abgesehen von der Sichtbarkeit der benötigten internen Funktionen und der
äquivalenten CQ-Überlaufbrücke. MR-Lookup, Closing, Referenzbesitz, verzögerte
Freigabe, CQ-Locks und Benachrichtigungsreihenfolge bleiben erhalten. Die weitere
Zerlegung von Apple-TX/RX und nativer Reassembly ist ein möglicher Folgeschritt;
dieser Umsetzungsschritt umfasst die vorgeschlagene erste MR/CQ-Aufteilung.

## 5. DMA-BUF-Engine und Configfs

`tbv_dmabuf_engine` bündelt Queues, Locks, aktiven TX-Kontext/-Batch, RX-Zustand,
Watchdog, Completion-Worker und Token. Die RX-Zustände sind ausdrücklich
`IDLE`, `OPEN`, `ARMED` und `CACHED`. `OPEN` hält den WR zwischen Batches;
`CACHED` hält den endgültigen Status. Die geordnete TX-Queue und die gemeinsame
RX-Transaktion über alle beteiligten Rails bleiben erhalten.

Die neue RX-Prüfung verwendet die tatsächlichen PREPARE-, Reap-, Finish-,
Abort-/Cancel- und Disconnect-Funktionen mit simuliertem DMA. Sie deckt mehrere
Batches, wiederholte PREPAREs, veraltete Tokens, leere Barrieren, kopierte Daten,
Fehler und Abschlussreferenzen ab.

Die ausschließlich protokollierenden Activate-/Deactivate-Transporthooks sind
entfernt. Configfs aktiviert den Konfigurationszustand und veröffentlicht den
Link direkt; bei fehlgeschlagener Veröffentlichung kehrt er zu `SEALED` zurück.
Eine Produktionsfunktionsprüfung sichert Aktivierung, Rollback und Release für
beide Backends. Die Aktivierungsrückgabe verwendet eine ausdrücklich vorzeichen-
behaftete Länge, sodass auch der GCC-Test ohne Typwarnung übersetzt.

## 6. Benchmark-Helfer

`userspace/bench/tbv_bench.h` enthält die identischen Zeit- und TCP-I/O-Helfer.
Die besonderen TCP-Verbindungsfunktionen von `uc_oneway` bleiben bei diesem
Programm. Nix übernimmt jetzt auch Header und baut `rc_write_imm_verify` im
Linux-Bench-Paket mit.

`tbv_bench.py` bündelt Prozessausführung, SSH-Argumentbildung, numerische
Ergebniszeilen und den identischen Latenzparser. Die beiden Bandbreitenparser
behalten ihre jeweilige Auswahl und Formatierung der Zeilen. Das installierte
Bench-Paket und der Nix-Plan-Wrapper stellen den gemeinsamen Python-Helper bereit.

Die fünf C-Helfer stimmen nach Namensanpassung mit den Ausgangsfunktionen
überein. Die Parser liefern für 604 Prüfausgaben, darunter 100 archivierte
Ausgaben, dieselben Ergebnisse bzw. Fehler wie vorher. Die CI-Prüfung umfasst
außerdem partielle I/O, EINTR, EOF, SSH-Argumente und Prozessfehler.

## Validierung und Nachweise

Erfolgreich im Container auf fuzzy, mit höchstens einem Buildjob und zwei Kernen:

- `thunderbolt-ibverbs`, gegen die Header von Linux 7.2.8;
- `bench-tools`, einschließlich aller fünf bearbeiteten C-Programme;
- `checks.x86_64-linux.proto-smoke`, mit GCC 15.3 und ASan/UBSan;
- `checks.x86_64-linux.script-syntax`;
- `checks.x86_64-linux.rdma-core-static`, rdma-core 64.0 mit statischem Provider.

Der gewöhnliche `rdma-core-usb4`-Build war ebenfalls erfolgreich. Die lokale
Smoke-Suite besteht mit Clang und ASan/UBSan. Der aktualisierte Nix-Plan-Wrapper
wurde erfolgreich ausgewertet. `git diff --check` besteht.

Buildlogs, lokale Prüfergebnisse und Quellhashes liegen in
[den Nachweisen](/Users/user/Documents/thunderbolt-ibverbs/bench/results/2026-10-07-refactor/receipt.json).
Diese prüfen Übersetzung und simulierte Zustandsverträge. Die anschließende
Hardwareprüfung ergänzt DMA-, Fehler- und Performanceversuche; ihre Fälle
und verbleibenden Grenzen sind im verlinkten Hardwarebericht dokumentiert.
