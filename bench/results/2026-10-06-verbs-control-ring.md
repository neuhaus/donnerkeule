# NHI-Steuerkanal hinter DMA-BUF-Verbs, 6. Oktober 2026

Der zuschaltbare Prototyp mit PREPARE/READY und aktivem DONE ist implementiert
und auf **fuzzy und misty** qualifiziert. Die vollständigen GPU-, Host-Staging-
und normalen Verbs-Matrizen bestehen. Die erwartete große Steuerpfad-Einsparung
hat sich nicht bestätigt: Der Kontroll-PING ist nur etwa 5 % schneller, die
Gesamt-CPU-Arbeit ist in dieser Diagnose höher. Auf diesen NHIs müssen zudem
Datenrails für Kontrollringe aufgegeben werden. Bei 20 MiB ist der Prototyp
deshalb etwa 55 % langsamer als der direkte Pfad mit allen vier Datenrails.

`dmabuf_control_ring` bleibt standardmäßig **0**. Die ursprünglichen Module
mit Build-ID `e898287be93660f988b733c065ab41633a49793c` und exakt gesicherten
Parametern sind seit **02:15:38 CEST** wiederhergestellt, Gufo seit **02:16:36**.
Beide Hosts haben vier bereite Rails; Images, Startargumente, Umgebungswerte,
Host-Konfiguration und Mounts stimmen. `/ready` auf fuzzy liefert seit der
Prüfung um **02:18:45** HTTP 200. Das Wartungsfenster begann am 5. Oktober
um 23:20 CEST. Private Containerkonfigurationen bleiben ausschließlich in den
geschützten Wiederherstellungsverzeichnissen auf den Hosts.

## Implementierung und Ressourcen

Ausgangspunkt ist die [erste Optimierung](2026-10-05-verbs-dmabuf-optimization.md)
auf `feat/write-striping`, Upstream-Merge `ca9eafd`. Der neue Kanal liegt in
`kernel/verbs_control.c` und `proto/verbs_control.h` und wird in die bestehende
DMA-BUF-Verbs-Implementierung eingebunden. Die Anwendung verwendet weiterhin
dieselben MR-, SEND-, RECEIVE- und CQ-Operationen.

- HELLO handelt die Fähigkeit aus; XDomain vermittelt die Kontroll-HopIDs
  und bestätigt die Tunnelbereitschaft. Die bisherigen Kontrollanfragen bleiben
  nutzbar, wenn die zusätzliche Fähigkeit ausgeschaltet ist.
- Je physischer Verbindung: eigenes TX/RX-Ringpaar, Ringgröße 128,
  32 wiederverwendete kohärente TX-Puffer und 64 dauerhaft gepostete RX-Puffer.
  64 vorbereitete Ereignisplätze halten Arbeit aus dem Ring-Callback heraus.
  Die 256-Byte-Frames enthalten Host-Metadaten; GPU-Payload bleibt direkt.
- PREPARE/READY verwendet die bisherigen QP-, PSN-, PD-, rkey-, Bereichs- und
  Lebensdauerprüfungen. DONE bestätigt tatsächliche RX-Platzierung. Es wird
  vor PREPARE registriert, sodass eine frühe Nachricht erhalten bleibt.
  Token, Boot-Identitäten, Domain und vollständige Batch-Metadaten werden
  abgeglichen. Leere Besitzübergabe-WRs werden sofort abgeschlossen.
- Eine DONE-Nachricht je Batch ersetzt die regulären STATUS-Roundtrips.
  Nach 10 ms ohne DONE erlaubt STATUS den Zugriff auf den gecachten Abschluss.
  Ein Transportwechsel während eines begonnenen DMA findet nicht statt.
- Fortschrittsprüfung über die vorhandenen NHI-Worker alle 1 ms und begrenzte
  Wartezeit auf freie TX-Poolplätze sichern die Kontroll-Completions ab.
  Die Nutzdaten bleiben global geordnet; der Kontrollkanal schafft noch keine
  parallelen aktiven RX-WRs.

Die Hardware meldet **`nhi_hop_count: 3`**: Konfigurationsring 0 plus zwei
nutzbare Ringpaare. Mit `lanes=2` sind beide für Daten belegt; der erste Versuch
konnte deshalb keinen zusätzlichen Ring allokieren. Der fertige Prototyp
erkennt dieses Budget und verlangt einen freien Ring. Die Tests verwenden
`lanes=1`, also einen Daten- und einen Kontrollring je NHI. Über zwei Kabel
bleiben zwei Datenrails. Die [Linux-NHI-Implementierung](https://github.com/torvalds/linux/blob/master/drivers/thunderbolt/nhi.c)
allokiert zusätzliche Ringe aus dem freien Bereich des gemeldeten Hopbudgets.

Eine vermeintlich zusätzliche Softwarequeue erweitert dieses Hardwarebudget
nicht. Die Kontrollringe sind deshalb auf dieser Hardware keine überzeugende
Optimierung für große Transfers.

## Separate Kontrollmessung

Die rootgeschützte Funktion `dmabuf_control_bench` spiegelt Kontrollmetadaten
nach Source-/Target-Prüfung und isoliert dadurch den Transport. Sie verwendet
dieselben 252-Byte-Transaktionsmetadaten; der NHI-Frame ergänzt vier Byte
Formatkennung. Der PING benötigt keine MR-Platzierung.
Je zwei physische Peers wurden auf fuzzy drei Serien mit 50.000 Roundtrips
gemessen: **300.000 NHI- und 300.000 XDomain-Roundtrips**, alle erfolgreich.

Die Tabelle enthält den Median der sechs Serien-Mediane, nicht den Median
aller zusammengeführten Einzelproben. Ohne CPU-Pinning oder wechselnd geordnete
Wiederholungen sind kleine Unterschiede kein Nachweis einer dauerhaften
Verbesserung.

| Kennzahl | XDomain | Separater NHI-Kanal |
|---|---:|---:|
| Kontroll-RTT, Median der Serien-Mediane | 17,43 µs | 16,62 µs |
| Bereich der sechs Serien-Mediane | 16,63–17,58 µs | 14,46–17,17 µs |
| CPU-Zeit des Benchmark-Schreibprozesses je RTT | 2,8 µs | 2,5 µs |
| Ganze Host-CPU, aktive Zeit je RTT, näherungsweise | 6,2 µs | 7,8 µs |

Die Prozess-CPU-Zeit enthält die aufrufende Kernelarbeit, aber keine anderen
Kernelworker. Die ganze Host-CPU stammt aus `/proc/stat` mit 100 Ticks/s und
schließt Interrupts, Worker, Fortschrittstimer und Hintergrundarbeit ein.
Sie ist kein isoliertes Profil des Treibers. Diese Werte belegen jedenfalls
keine Entlastung der Gesamt-CPU durch den Prototyp.

Protokolle: [NHI, Peer 1](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-06-verbs-control-ring/fuzzy/v4-nhi-control-p1-r1.result),
[NHI, Peer 2](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-06-verbs-control-ring/fuzzy/v4-nhi-control-p2-r1.result),
[XDomain, Peer 1](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-06-verbs-control-ring/fuzzy/v4-xd-control-p1-r1.result),
[XDomain, Peer 2](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-06-verbs-control-ring/fuzzy/v4-xd-control-p2-r1.result).
Alle Serien, P95/P99/Maximum, CPU-Deltas und Zeiten liegen im Rohdatenordner.

## Gesamter GPU-Verbs-Austausch

Identischer Test: WRITE_WITH_IMM, vier ständig vorgepostete Zero-SGE-WQEs,
Spinfenster 5 ms, vier Runden mit MR-/QP-Neuerzeugung. Beide Ränge senden und
empfangen gleichzeitig. Kernel: `dmabuf_index=1 dmabuf_rx_eager=1
dmabuf_parallel_post=1 dmabuf_profile=1`; der normale Modus verwendet Host-MRs.

Die Werte in µs enthalten GPU-Erzeugung, Vorlesen, Besitzübergabe und vollständige
Byte-/Wächterprüfung. Es sind Mediane von sechs Rundenmitteln, beide Hosts und
Runden 1–3. Runde 0 bleibt erhalten, wird wegen des ersten GPU-Kernelstarts
ausgeschlossen. Das sind keine Einzeltransfer-Perzentile; die zwei Ränge sind
keine unabhängigen Wiederholungen.

| WR-Länge, Tiefe / Iterationen | NHI, 2 Datenrails | XDomain, 2 Datenrails | XDomain, 4 Datenrails | Bisheriges Verbs, 4 Datenrails |
|---|---:|---:|---:|---:|
| 10.304 B, 1 / 256 | 167,5 | 173,1 | 178,5 | 103,9 |
| 1 MiB, 3 / 32 | 529,0 | 548,6 | 422,5 | 427,7 |
| 20 MiB + 64 B, 3 / 16 | 11.523,6 | 11.421,3 | 7.420,8 | 6.494,1 |

Bei gleicher Datenrail-Zahl gewinnt der NHI-Kanal etwa 3 % bei den zwei kleinen
Größen, bei 20 MiB etwa 1 % höhere Latenz. Diese geringen Unterschiede reichen
für keine belastbare Optimierungsentscheidung. Der Vergleich XDomain mit zwei
und vier Datenrails zeigt dagegen den erheblichen Ressourcenverlust bei großen
Transfers. Der direkte Pfad mit vier Datenrails liegt bei 1 MiB ungefähr auf
dem bisherigen Verbs-Niveau; sein einzelner etwas besserer Punkt beweist keine
Überlegenheit. Bei 20 MiB bleibt er etwa 14 % langsamer. Der NHI-Prototyp mit
zwei Datenrails schließt diese Lücke nicht.

Die Host-MR-Variante des Tests kopiert HIP-Payload zu/von Hostspeicher. Gufos
vorhandener Verbs-Adapter verwendet GPU-gemappten Hostspeicher und unterscheidet
sich davon. Dies ist eine Transportdiagnose und keine Gufo-Modellmessung.
DRAM-/UMC-Bandbreite wurde nicht gemessen.

Protokolle jeweils auf beiden Hosts: `v4-nhi-data-*`, `v4-xd-one-data-*`,
`v4-xd-two-data-*` und `v4-normal-data-*`. Die Profiling-Deltas stehen in
`*-before.txt` und `*-after.txt`, die Kontrollzähler in `*-peers.txt`.
[summary.json](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-06-verbs-control-ring/summary.json) enthält Werte,
Bereiche und alle verwendeten Rundenmittel;
[timing-samples.json](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-06-verbs-control-ring/timing-samples.json) enthält
die RESULT-/PHASE-Zeilen einschließlich gekennzeichneter früherer Versuche.

## Qualifikation und behobene Fehler

Beide Hosts bestanden mit dem abschließenden Modulstand v4:

- GPU-SEND, SEND_WITH_IMM, WRITE_WITH_IMM und WRITE; elf Größen von 0 bis
  32 MiB − 64 B, 64 Wächterbytes, zwei Runden mit MR-/QP-Neuerzeugung.
  Signaled-Serie einschließlich 100 ms verzögerten Receive-WQEs und RNR.
- Unsignaled-Serie mit vier Iterationen: falscher rkey erzeugt
  `REM_ACCESS_ERR` bei der unsignaled Send-WQE und `WR_FLUSH_ERR` beim Receiver.
- Host-MRs im neuen Staging-Pfad mit beliebigen Byte-Längen sowie Fehlerprobe.
- Vollständige normale Host-MR-Verbs-Matrix bei abgeschaltetem DMA-BUF-Modus.
- Alle gezielten Größenvergleiche und alle abschließenden Kontroll-PINGs.

Direkte GPU- und Host-Staging-Matrizen bewegten insgesamt etwa 7,87 GB
geprüfte Payload je Rang; die normale Matrix weitere etwa 1,97 GB je Rang.
Die Testausgaben bestätigen `tcp_data_calls=0`. MRs, FD-/Bereichsproben,
GPU-Ausrichtung und fremde PDs verwenden das unverändert weiterverwendete
HIP-Testprogramm aus der vorherigen Qualifikation.

Frühere Versuche sind als solche erhalten: v1 scheiterte an belegten NHI-Ringen.
Unter Kernel-Lockdown war eine schreibbare Debugfs-Messdatei gesperrt; die
Messung verwendet deshalb einen rootgeschützten Modulparameter und eine reine
Debugfs-Leseansicht. In v3 blockierten ausgebliebene TX-Completions den kleinen
Kontrollpool. V4 ergänzt Fortschrittsprüfung und Wartezeit auf Poolfreigabe.
Die gesamte Matrix wurde damit erneut bestanden. Dabei gab es zwei kurze
TX-Pool-Wartefälle auf fuzzy, einen auf misty und eine STATUS-Wiederherstellung
auf fuzzy; keine fehlerhaften Kontrollframes oder RX-Ereignispool-Überläufe.
Jeder Host meldet 4.346 aktiv gesendete Batch-DONEs für diese Matrix.

Builds mit `W=1` bestanden auf beiden Fedora-Hosts, Kernel
`7.2.8-100.fc43.x86_64`. Keine Compilerwarnung in den neuen Kontrolldateien;
der vollständige Build enthält die bestehende Warnung zur Größe des
IB-Gerätenamens sowie den BTF-/pahole-Hinweis. Die gemeinsame Protokollprobe
bestand lokal mit `-Wall -Wextra -Werror`, AddressSanitizer und UBSan.
Shell-Syntax und Whitespace-Prüfung bestehen. Das HIP-Testbinary ist unverändert
und hat SHA-256 `870bed5e2742d0eb2c7b8501c1cfcacfeccabb4f3aa38df9b9019d641f966baf`.

Im gesamten Kernelprotokoll gibt es keine WARN-/BUG-Stacks, GPU-Resets oder
IOMMU-Faults. Die frühe Ringallokation und einige Tunnelaufbau-/Abbauversuche
auf misty meldeten Fehler bzw. Konfigurations-Timeouts; sie sind im Protokoll
erhalten und dürfen nicht als fehlerfreie Setup-Qualifikation bezeichnet werden.
Die abschließenden Datenprüfungen bestanden. Kernel-Taint bleibt auf beiden
Hosts 4.608. Hot-Unplug, kontrollierte Verlustinjektion und dauerhafte Serving-
Last bleiben weitere Qualifikationsaufgaben.

## Ergebnis und nächste Richtung

Die Vermutung, der synchrone XDomain-Transport allein erkläre einen großen Teil
der Lücke, hat sich im isolierten seriellen Test nicht bestätigt. Weitere
Kontrollringe zu reservieren kostet auf dieser Hardware mehr Datenleistung,
als der neue Kanal an Latenz einspart.

Der nächste sinnvolle Steuerpfadversuch ist ein asynchroner Abschluss über den
bestehenden Konfigurationskanal, der alle Datenrails erhält, zusammen mit
weniger Workerwechseln. Für große WRs bleibt das gebündelte Einreihen von
Deskriptoren im NHI-Treiber ein stärkerer Hebel. Modellleistung und
Speicherbandbreitenentlastung brauchen anschließend einen Gufo-Adapter und
einen Vergleich unter echter Modelllast.

Die öffentlichen eigenen Protokolle, Quell-/Binary-Prüfsummen und redigierten
Wiederherstellungsnachweise liegen in
[fuzzy](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-06-verbs-control-ring/fuzzy/public-results.sha256) und
[misty](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-06-verbs-control-ring/misty/public-results.sha256).
Die privaten Gufo-Inspect-Daten und Signierschlüssel sind darin nicht enthalten.
