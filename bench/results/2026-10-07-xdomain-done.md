# XDomain-DONE: lokaler Implementierungsstand vom 7. Oktober 2026

Der Treiber kann den Batch-Abschluss jetzt aktiv über den bestehenden
XDomain-Konfigurationskanal senden. Der Modus `dmabuf_push_done=1` benötigt
keine zusätzlichen NHI-Ringe und kann die vier bisherigen Datenrails behalten.
Er ist standardmäßig ausgeschaltet. Dieser Bericht hält die lokale Prüfung
vom 7. Oktober fest; die spätere [Hardwareprüfung vom 8. Oktober](2026-10-08-hardware.md)
enthält Builds, Daten- und Fehlerqualifikation sowie den Leistungsvergleich.

## Hintergrund und Verhalten

Der [separate NHI-Steuerkanal](2026-10-06-verbs-control-ring.md) senkte den
isolierten Kontroll-Roundtrip nur wenig. Die notwendige Reservierung von
Ringen halbierte zugleich die Zahl der Datenrails auf den Test-NHIs und
verschlechterte große Transfers. Die neue Variante verwendet deren bereits
vorhandenen Konfigurationsring.

- Beide Peers handeln `PUSH_DONE` über HELLO aus. Fehlt die Fähigkeit, bleibt
  das bisherige PREPARE-/STATUS-Verfahren bestehen. Ein ausgehandelter
  separater NHI-Kontrollring hat Vorrang; laufende Batches wechseln den
  Transport nicht.
- PREPARE und seine Antwort bleiben synchron auf XDomain. Der Empfänger
  meldet den tatsächlichen Batch-Abschluss aktiv. Die Quelle registriert den
  vollständigen Token samt Generation und Batch vor PREPARE und behält
  früh eintreffendes DONE bis zum lokalen TX-DMA-Abschluss.
- Leere Übergabe-WRs schließen direkt mit der PREPARE-Antwort ab. Sie senden
  weder einen zusätzlichen DONE-Frame noch eine STATUS-Abfrage.
- Nach 10 ms ohne DONE fragt die Quelle STATUS ab. Ein verlorenes oder wegen
  Speichermangel nicht sendbares DONE erzwingt keine erneute Datenübertragung;
  der Empfänger behält die bekannte Platzierung.
- Source-CQ benötigt weiterhin lokalen DMA-Abschluss und bestätigte remote
  Platzierung. Receiver-CQ erscheint einmal für den ganzen WR. Fehler,
  RNR, QP-/MR-Lebensdauer und die Quarantäne fehlerhafter Datenrails folgen dem
  bestehenden Vertrag.

DONE wird im Protokollhandler abgefangen, bevor es eine synchrone Anfrage
beantworten kann. Das ist notwendig, weil der allgemeine Matcher nur Route
und Protokoll-UUID zuordnet. Auch identifizierbare beschädigte, abgeschnittene
oder verspätete Notifications werden konsumiert, aber nicht als Erfolg
gewertet. Nur eine vollständige passende Transaktion weckt den Source-Worker.
Grundlage sind der [Linux-XDomain-Matcher](https://github.com/torvalds/linux/blob/master/drivers/thunderbolt/xdomain.c)
und die [Reihenfolge der Protokolldispatches vor Request-Matching](https://github.com/torvalds/linux/blob/master/drivers/thunderbolt/ctl.c).

Ein eigener RX-Verweis hält den Software-Abschlusstracker bis zur Meldung oder
zum Abbruch gültig. Ein Verbindungsabbau stoppt und drainiert die Hardware
unabhängig davon. Dieses Verhalten schützt auch Batches, deren Daten auf
anderen physischen Links als ihre Steueranfrage laufen.

## Lokale Prüfung

Die Protokollprüfung mit Clang, AddressSanitizer und UndefinedBehaviorSanitizer
prüft ABI, Batches, kurze Restframes, vollständige Generationszuordnung und
ungültige Pakete. Neu sind insbesondere konsumierbare DONE-Nachrichten mit
beschädigtem Body oder widersprüchlichen Opcodes sowie der unveränderte
Durchlass normaler PREPARE-/STATUS-Antworten.

`tools/ci/verbs-control-state-smoke.py` kompiliert die tatsächlichen C-Bodys der
Abschlusshandler mit simulierten Listen, Sperren und Completion-Primitiven.
Es prüft alle 16 Kombinationen der lokalen und entfernten Fähigkeiten,
frühes DONE vor READY, doppelte und verspätete Meldungen nach dem
Schließen, unabhängige Transaktionen, acht Abweichungen der Generation oder
des Batches, ungültige positive Statuswerte, negative Abschlüsse,
STATUS-Wiederherstellung und Aufwecken bei Disconnect. Beide Prüfungen bestehen.
Die Simulation qualifiziert weder Kernel-Nebenläufigkeit noch tatsächliches
DMA. Skriptsyntax und Whitespace-Prüfung bestehen ebenfalls.
Die spätere Hardwareprüfung hält den gebauten Quellstand in
[driver-build-source-hashes.json](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-08-hardware/driver-build-source-hashes.json) fest.

Im lokalen Prüflauf vom 7. Oktober wurde auf dem macOS-Arbeitsplatz kein
Linux-Kernelmodul gebaut. Die frühere Hardwarequalifikation galt für ihren
damaligen Quellstand; die neuen Builds und Messungen folgten am 8. Oktober.

## Damaliger Plan für den Hardwarevergleich

Dieser Plan wurde nach erneuter Freigabe am 8. Oktober ausgeführt. Ergebnisse
und verbleibende Grenzen stehen im oben verlinkten Hardwarebericht.

Der Nutzer hat Builds, Modulwechsel und Tests auf fuzzy und misty wegen
anderer laufender Tests zurückgestellt. In dieser Runde gab es keine
Hostzugriffe und keine Änderungen am laufenden Gufo.
Die korrigierten Ethernet-Adressen für spätere Zugriffe sind:

| Host | Ethernet-IP |
| --- | --- |
| fuzzy | 192.168.211.2 |
| misty | 192.168.211.3 |

Erst nach erneuter Testfreigabe: isoliert mit den tatsächlichen Host-Headern
bauen, aktuelle Module und Gufo-Zustände sichern, dann auf beiden Hosts
`dmabuf_verbs=1 dmabuf_control_ring=0 dmabuf_push_done=1 lanes=2` testen.
Das Laborskript unterstützt dafür `TBV_LAB_PUSH_DONE=1`. Im Vergleichslauf
`TBV_LAB_PUSH_DONE=0` verwenden und alle übrigen Parameter gleich halten.

Die Verifikation muss vier Datenrails, ausgehandelte Bereitschaft,
signaled/unsignaled GPU- und Host-MR-Matrizen, RNR, ungültige Imports,
Remote-Fehler und QP-/MR-Neuerzeugung prüfen. Danach den bisherigen
WRITE_WITH_IMM-Vergleich mit 10.304 Byte, 1 MiB und 20 MiB bei identischen
CQ-Warteparametern durchführen. `done_wait`, STATUS-Anfragen und CPU-Kosten
mit erfassen und anschließend den gesicherten Laufzustand wiederherstellen.
Gezielte Verluste der Abschlussmeldung und Abbau eines anderen Links bleiben
zusätzliche offene Fehlerqualifikation.

Die globale geordnete TX-Queue und der einzelne aktive RX-WR bleiben bestehen.
Die spätere Hardwareprüfung zeigte 16,6 % geringere Zykluslatenz bei 10.304 B,
praktisch gleiche Latenz bei 20 MiB und streuende Ergebnisse bei 1 MiB.
Gezielter DONE-Verlust wurde korrekt über STATUS aufgefangen. Link-Abbau
führte zu einem gestörten Interruptzustand, den erst ein ACPI-NHI-Reset behob;
ein schneller automatischer Wiederanlauf bleibt offen.
