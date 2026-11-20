# Verbs-DMA-BUF: Profiling und erste Optimierung, 5. Oktober 2026

Paralleles Posting an die beiden NHI-Geräte senkt die gemessene Latenz des
direkten Pfads bei 1 und 20 MiB um etwa **21 %**. Der optimierte Pfad ist in
diesem Prüfablauf weiterhin langsamer als der bisherige Verbs-Pfad. Weniger
Kopien allein haben die Lücke noch nicht geschlossen.

Alle abschließenden GPU-, Host-Staging- und normalen Verbs-Matrizen bestanden
auf fuzzy und misty. Die ursprünglichen Module und Gufo wurden bis
**21:50 CEST** wiederhergestellt: ursprüngliche Build-ID und Parameter,
vier bereite Rails, gleiche Gufo-Images und Konfiguration, HTTP 200 auf
`/ready`. Dies ist eine Transportdiagnose; eine Verbesserung unter Gufo-Modelllast
oder eine gemessene Entlastung der DRAM-Bandbreite ist damit nicht nachgewiesen.

## Stand und Änderungen

Ausgangspunkt ist die [erste Hardwarequalifikation](2026-10-05-verbs-dmabuf.md)
auf Branch `feat/write-striping`, mit Upstream `f1e5f48` über Merge `ca9eafd`.
Die vorhandenen RC-Verbs-Operationen, MR-/QP-Referenzen, Berechtigungsprüfungen,
32-Byte-GPU-Längen und GPU-System-Release bleiben der Speichervertrag.

- `dmabuf_index=1`: Einmaliger Index je MR und tatsächlichem NHI-Attachment
  statt wiederholter SG-Listensuche für jeden 4-KiB-Frame. Einträge bewahren
  die SG-Grenzen und prüfen weiterhin die volle gepolsterte RX-Spanne.
- `dmabuf_rx_eager=1`: RX-Reaping beim letzten DMA-Callback in einem separaten
  Worker. CQ und Platzierungsstatus werden unter der bisherigen RX-Sperre
  abgeschlossen. STATUS bleibt die Bestätigung für die Quelle.
- `dmabuf_parallel_post=1`: Batches ab 128 Frames werden nach NHI-Gerät
  gruppiert und auf einer ungebundenen Workqueue parallel eingereiht.
  Rails desselben NHIs bleiben sequenziell. Alle Posting-Worker werden vor
  Freigabe oder Fehler-Drain abgewartet.
- `dmabuf_profile=1`: Aufrufe, kumulative Nanosekunden und Maximum pro
  Treiberphase in Debugfs, einschließlich getrennten RNR-/Busy-Wartepausen.
  Standardmäßig ausgeschaltet; die drei Optimierungen sind im weiterhin
  opt-in geschalteten DMA-BUF-Verbs-Modus eingeschaltet.

Der Test ergänzt `PHASE`-Zeitabschnitte, Tiefe 1–3, gezielte Größen/Operationen,
eine dauerhaft vorbereitete Empfangsqueue und ein einstellbares Spinfenster.
Der Standardmodus des Treibers bleibt `dmabuf_verbs=0`.

## Vergleich innerhalb des direkten Pfads

Identisches Modul, Testprogramm und Parameter bis auf `dmabuf_parallel_post`:
`dmabuf_verbs=1 dmabuf_index=1 dmabuf_rx_eager=1 dmabuf_profile=1`.
Der Test verwendet WRITE_WITH_IMM, vier ständig vorgepostete Zero-SGE-WQEs,
vier Runden mit QP-/MR-Neuerzeugung und `--spin-us 0`.

Die Tabelle zeigt den Median der sechs Rundenmittel aus beiden Hosts und
Runden 1–3; Runde 0 mit erstem GPU-Kernelstart bleibt im Rohprotokoll erhalten.
Beide Ränge tauschen gleichzeitig Daten aus. Diese Mittel sind keine
Einzeltransfer-Perzentile und die beiden Hosts sind keine unabhängigen
Versuchswiederholungen. Werte in µs je Nachricht, einschließlich GPU-Erzeugung,
Vorlesen, Barriere und vollständiger Byte-/Wächterprüfung:

| WR-Länge | Tiefe / Iterationen je Runde | Serielles Posting | Paralleles Posting | Änderung |
|---|---:|---:|---:|---:|
| 10.304 B, eine Qwen-Zeile + Header | 1 / 256 | 331,0 | 318,8 | kleine Batches bleiben sequenziell; kein kausaler Gewinn behauptet |
| 1 MiB | 3 / 32 | 600,2 | 472,4 | −21,3 % |
| 20 MiB + 64 B, 2.048 Qwen-Zeilen | 3 / 16 | 9.335,2 | 7.349,5 | −21,3 % |

Die sechs 20-MiB-Rundenmittel lagen seriell bei 9.334,6–9.355,6 µs und parallel
bei 7.233,7–7.549,1 µs. Ein längerer, wechselnd geordneter Vergleich unter
Dauerlast steht noch aus.

Protokolle und Zähler:
[fuzzy seriell](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-05-verbs-dmabuf-optimization/fuzzy/persistent-serial-20971584.log),
[fuzzy parallel](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-05-verbs-dmabuf-optimization/fuzzy/persistent-parallel-20971584.log),
[misty seriell](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-05-verbs-dmabuf-optimization/misty/persistent-serial-20971584.log),
[misty parallel](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-05-verbs-dmabuf-optimization/misty/persistent-parallel-20971584.log).
Die zugehörigen `*-before.txt` und `*-after.txt` enthalten die Profiling-Deltas.

## Was die Phasen zeigen

Bei 20 MiB gibt es je Host 192 Nutzdaten-WRs und zusätzlich 68 leere
Barriere-WRs. 644 Batches durchlaufen PREPARE. Die folgende Tabelle teilt die
Zählerdifferenzen auf fuzzy durch die 192 Nutzdaten-WRs; die geringe zusätzliche
Barrierearbeit ist enthalten. Zeiten in µs:

| Treiberphase | Seriell | Parallel |
|---|---:|---:|
| TX-Deskriptorbau | 120,8 | 117,9 |
| PREPARE einschließlich Arbeit beim Empfänger | 1.601,9 | 961,5 |
| TX-Deskriptoren einreihen | 2.850,5 | 1.520,3 |
| Warten auf lokale TX-DMA-Abschlüsse | 3.772,4 | 3.651,9 |
| STATUS-Anfragen | 250,2 | 90,2 |
| RX-Deskriptoren einreihen, beim Empfänger | 1.405,7 | 699,6 |

RX-Posting ist in der PREPARE-Zeit der anderen Seite enthalten; diese Zeile darf
nicht zusätzlich zu einer Gesamtdauer addiert werden. Job- und Queue-Zeiten sind
ebenfalls keine weiteren disjunkten Abschnitte. Misty zeigt dieselbe Richtung:
TX-Posting 2.359,9 → 1.605,6 µs, RX-Posting 1.371,1 → 724,2 µs.

Damit liegt der wesentliche gemessene Gewinn beim Einreihen der Deskriptoren.
Die reine SG-Adresssuche ist erheblich kleiner: In der früheren Index-A/B-Serie
auf fuzzy sank TX-Bau für die gesamte 20-MiB-Serie von 8,16 auf 6,37 ms, RX-Bau
von 8,00 auf 6,12 ms. Diese frühere Serie verwendet einen anderen Testablauf;
ihre Gesamtzeiten sind kein vergleichbarer Beleg für einen Index-Latenzgewinn.

Frühe Testversionen stellten Receive-WQEs pro Iteration bereit und schliefen
sofort nach einem leeren CQ-Poll. Insbesondere die zusätzliche
Eigentumsbarriere konnte dadurch Millisekunden RNR-Wartezeit erzeugen. Mit vier
persistent vorbereiteten WQEs gab es in den abschließenden seriellen und
parallelen Profiling-Serien auf beiden Hosts **keine RNR-Wartepause**. Seltene
STATUS-Wartepausen bleiben möglich. Der große Rückgang der frühen kleinen
Testlatenzen darf daher nicht als entsprechender Gufo-Geschwindigkeitsgewinn
gelesen werden: Gufo hatte seine Empfangsqueue bereits vorbereitet.

## Vergleich mit dem bisherigen Verbs-Pfad

Der abschließende Vergleich nutzt denselben finalen Test, vier vorbereitete
Empfangs-WQEs und `--spin-us 5000`, ohne Kernel-Profiling. Gufo spinnt ebenfalls
zunächst bis zu 5 ms; hier gilt das Fenster pro CQ-Warteaufruf, dort pro
Austausch. Die zusätzliche Verbs-Eigentumsbarriere und vollständige GPU-Prüfung
bleiben im Test erhalten. Median über beide Hosts und Runden 1–3:

| WR-Länge | Bisherige Host-MRs, µs | Direkte GPU-MRs, µs | Verbleibende Mehrzeit |
|---|---:|---:|---:|
| 10.304 B | 121,6 | 180,2 | +48,2 % |
| 1 MiB | 400,0 | 458,1 | +14,5 % |
| 20 MiB + 64 B | 6.018,0 | 7.314,8 | +21,5 % |

Die sechs Rundenmittel der letzten Größe lagen im bisherigen Pfad bei
5.884,3–6.020,9 µs und direkt bei 7.209,3–7.343,2 µs. In diesen gezielten
normalen WRITE_IMM-Reihen meldeten die Zähler keine Retransmits oder
Retry-Erschöpfung.

Bei 20 MiB auf fuzzy entfallen im direkten Prüfablauf ungefähr 6,46 ms auf
CQ-Warten; im bisherigen Pfad ungefähr 4,06 ms. Der bisherige Pfad verbringt
dafür mehr Zeit in GPU-Vorbereitung/Host-Kopie und RX-Kopie/Release. Diese
CPU-Zeitabschnitte enthalten auch HIP-Arbeit und sind keine isolierten
GPU-Kernel- oder DMA-Laufzeiten.

Der Host-MR-Kontrolltest kopiert GPU→Host vor TX und Host→GPU nach RX. Gufos
bestehender Adapter nutzt dagegen GPU-gemappten Hostspeicher und
Produzenten-/Konsumenten-Kernels. Diese Tabelle ist deshalb kein exakter
Vergleich seiner beiden möglichen Adapter. Der DMA-BUF-Verbs-Adapter in Gufo
wurde in dieser Runde weder integriert noch unter Modelllast gemessen.

Auch Prozess-CPU-Zeit ist kein Beleg für die gesamte CPU-Ersparnis: Die
20-MiB-Reihe meldet auf fuzzy 1,25 s user+system für direkt und 1,37 s für normal,
auf misty 1,25 bzw. 1,39 s. Kernel-Worker fehlen darin. DRAM-/UMC-Bandbreitenzähler
wurden nicht gemessen. Das Entfallen der Treiber-Nutzdatenkopien ist strukturell
belegt, ihr Nutzen für konkurrierende Modell-Kernels bleibt zu messen.

## Abschließende Qualifikation und erhaltene Fehler

Auf **beiden** Hosts bestanden:

- GPU-Matrix, alle vier Operationen, zwei Iterationen und zwei Runden,
  alle Sends signaled, erste Receive-Bereitschaft um 100 ms verzögert:
  1.966.672.896 geprüfte Nutzdatenbytes pro Rang.
- GPU-Matrix, vier Iterationen und zwei Runden, unsignaled Sends mit späterem
  signaled Abschluss, falscher rkey mit `REM_ACCESS_ERR` auch beim unsignaled
  Send und `WR_FLUSH_ERR` beim Receive:
  3.933.345.792 geprüfte Nutzdatenbytes pro Rang.
- Neuer Host-Staging-Pfad, zwei Iterationen/zwei Runden mit falschem rkey:
  1.966.667.040 geprüfte Nutzdatenbytes pro Rang, auch Längen 1, 4, 4095 und 4097.
- Bisheriger Host-MR-Pfad, zwei Iterationen/zwei Runden, alle Operationen:
  1.966.667.040 geprüfte Nutzdatenbytes pro Rang. Einzelne normale
  Retransmits/RNR in dieser vollständigen Matrix sind erhalten;
  keine Retry-Erschöpfung oder fehlerhafte Nutzdaten.

Jede Nachricht prüft alle Nutzdaten und 64 Wächterbytes, einschließlich
GPU-Vorlesen vor DMA. Ungültige Imports, gültiger Teilbereich, FD-Offset 4096,
fremde PDs, unausgerichtete SGEs und sub-32-Byte-GPU-Längen werden weiterhin
geprüft. Der TCP-Bootstrap ist nach 200 gesendeten/empfangenen Bytes je Runde
geschlossen; alle Daten-WRs und Barrieren nutzen Verbs (`tcp_data_calls=0`).

Die erste Version der persistenten Testqueue ordnete eine vorzeitig ankommende
nächste Barriere dem laufenden Nutzdaten-WR zu. Der Test brach mit
`RQ order/length/immediate mismatch` ab. Die Barriere wird jetzt separat
vorgemerkt; alle anschließenden Vier-Runden-Reihen und die Abschlussmatrizen
bestanden. Der fehlgeschlagene Versuch ist als
`persistent-rq-v5-failed-10304.*` auf beiden Hosts erhalten und nicht in den
abschließenden Vergleich eingerechnet.

Kernel-Builds mit `W=1` bestanden auf beiden Hosts; die fehlende BTF-/pahole-
Umgebung ist im Buildprotokoll vermerkt. HIP-Build mit `-Wall -Wextra -Werror`,
Protokollprobe mit ASan/UBSan und Shell-Syntaxprüfungen bestanden.
Seit Beginn des Fensters um 20:53 CEST enthalten die Kernelprotokolle keine
WARN-/BUG-Stacks, IOMMU-Fehler, GPU-Resets oder GPU-Timeouts. Die erwarteten
Meldungen zu nicht unterstütztem MAD-QP1 sind erhalten.

## Artefakte und Wiederherstellung

Das isolierte Verzeichnis ist auf beiden Hosts
`/home/sn/tbv/verbs-dmabuf-lab-20261004/optimization-20261005`.
Installierte Module und Systembibliotheken wurden nicht ersetzt. Der finale
Test hat SHA-256
`870bed5e2742d0eb2c7b8501c1cfcacfeccabb4f3aa38df9b9019d641f966baf`.

Die lokalen Verzeichnisse
[fuzzy](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-05-verbs-dmabuf-optimization/fuzzy/public-results.sha256) und
[misty](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-05-verbs-dmabuf-optimization/misty/public-results.sha256)
enthalten 161 bzw. 155 eigene Protokolle/Metadatendateien plus SHA-Manifest.
`final-artifacts.sha256` verknüpft Quelle, Protokoll, Test und signiertes Modul;
`historical-artifacts.sha256` hält die früheren Kandidaten fest.
Alle 1.680 RESULT-/PHASE-Datensätze sind in
[timing-samples.json](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-05-verbs-dmabuf-optimization/timing-samples.json)
erhalten. Private Gufo-Startargumente und Umgebungsvariablen wurden nicht in
dieses Ergebnispaket übernommen.

Wiederherstellung:
[fuzzy](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-05-verbs-dmabuf-optimization/fuzzy/restoration-check.txt),
[misty](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-05-verbs-dmabuf-optimization/misty/restoration-check.txt).
Die ursprüngliche Build-ID ist `e898287be93660f988b733c065ab41633a49793c`.
Der Neustarthelfer wurde für bereits auf Image-ID gesetzte Startargumente
korrigiert; Image, Kommando, Umgebung und Hostkonfiguration wurden anschließend
mit den vor dem Fenster gesicherten Werten verglichen. Podmans reine
Bind-Mount-Listenreihenfolge wurde normalisiert.

Die nächsten durch Messung begründeten Arbeiten sind weniger Ring-Enqueues und
Allokationen pro WR, vorbereitete RX-Batches mit Credits und Überlappung der
Batch-Vorbereitung mit laufender DMA. Ein schnellerer Steuerpfad ist besonders
für kleine WRs sinnvoll. Referenzen, GPU-Fensterfreigabe, genau einmalige CQs
und der Drain bei Fehlern müssen dabei erhalten bleiben. Die bisherigen Grenzen
bei Verlust, Hot-Unplug, GPU-Reset und parallelen QPs gelten weiterhin.
