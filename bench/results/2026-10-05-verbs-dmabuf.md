# Verbs-DMA-BUF: erste Hardwarequalifikation, 5. Oktober 2026

Der erste Verbs-DMA-BUF-Stand hat auf **fuzzy und misty** die direkten
GPU-Transfers und Fehlerprüfungen bestanden. Die normalen Host-MRs bestanden
im bisherigen Modus und im neuen Staging-Modus. Am Ende dieser Testserie wurden
die gesicherten Module und Gufo-Instanzen vollständig wiederhergestellt.
Der Modus bleibt experimentell; insbesondere ist der direkte WRITE-Pfad noch
langsamer als gewöhnliche Verbs. Anschließende Optimierungen werden getrennt
von dieser Basisqualifikation gemessen.

## Stand und Umgebung

- Branch `feat/write-striping`, Upstream `f1e5f48` integriert durch Merge `ca9eafd`.
  Die Implementierung liegt als Arbeitsbaumänderung vor. Vor dem Merge wurden
  alle Änderungen mit Prüfsummen gesichert.
- Fedora, Kernel `7.2.8-100.fc43.x86_64`, ROCm 7.2.3, `gfx1151`.
  Beide Module wurden isoliert mit `W=1` gebaut und mit den jeweiligen
  Host-Schlüsseln signiert. BTF fehlt mangels `vmlinux`; die bestehende
  Warnung zum Namen `usb4_rdma%d` ist im Buildprotokoll erhalten.
- Zwei USB4-Verbindungen mit insgesamt vier Rails, `lanes=2`;
  `usb4_rdma0`, GID-Index 1. Management-IP dient nur dem Bootstrap.
- Direkter Modus: `dmabuf_verbs=1`, `gpu_direct=0`, `register_verbs=1`.
  Der Vergleich verwendet denselben Modulstand mit `dmabuf_verbs=0`.
- HIP-Test SHA-256:
  `7e6c1d62f04cc5bbf69b43493ca2099f3e75ad3cde30c89c7a55bf4229c8fe84`.
  Alle nachfolgend als final bezeichneten Läufe verwenden diesen Test.
  Der frühere normale Kontrolllauf hatte den SHA-256
  `72a45e42fd62df27a789291c5a3e7a97fd92261974a15bc51f718714bf3138bb`.
- Isoliertes Verzeichnis beider Hosts: `/home/sn/tbv/verbs-dmabuf-lab-20261004`.
  Installierte Module und Systembibliotheken wurden nicht ersetzt.

## Geprüfter Vertrag

Alle Läufe prüfen SEND, SEND_WITH_IMM, WRITE_WITH_IMM und WRITE, drei ausstehende
Fenster, CQ-Reihenfolge, IMM und Byte-Länge. Jeder Payload wird vollständig auf
der GPU geprüft, zusätzlich 64 Wächterbytes je Nachricht. RX wird vor dem DMA
bereits auf der GPU gelesen. Nur das 16-Byte-Prüfergebnis wird zum Host kopiert.
Die zwei Runden erzeugen MRs und QPs neu.

Die GPU-Matrix umfasst `0, 32, 64, 4064, 4096, 4128, 65536, 2097184,
1048576, 4194304, 33554368` Byte. GPU-WRs benötigen eine SGE, einen
seitenausgerichteten MR-Offset und eine Länge als Vielfaches von 32.
Die Host-Matrix umfasst auch `1, 4, 4095, 4097, 2097153` Byte.
Imports verwenden FD-Offset 4096 und von GPU-VAs unabhängige IOVAs.

| Lauf, auf beiden Hosts PASS | Optionen neben Gerät/GID | Geprüfte Nutzdaten je Host, beide Runden |
| --- | --- | ---: |
| Direkte GPU-MRs, alle WRs signaled, RNR | `--iterations 2 --rounds 2 --signal-all --rnr-delay-ms 100` | 1.966.672.896 Byte |
| Direkte GPU-MRs, unsignaled und falscher rkey | `--iterations 4 --rounds 2 --remote-error` | 3.933.345.792 Byte |
| Direkte GPU-MRs, Timinglauf | `--iterations 8 --rounds 2` | 7.866.691.584 Byte |
| Host-MRs im neuen Staging-Modus, Fehlerprobe | `--host-mr --iterations 8 --rounds 2 --remote-error` | 7.866.668.160 Byte |
| Host-MRs im bisherigen Modus, finaler Kontrolllauf | `--host-mr --iterations 8 --rounds 2` | 7.866.668.160 Byte |

Acht Import-Negativfälle, ein gültiger Teilbereich, unausgerichtete GPU-SGE,
Ein-Byte-GPU-WR und fremde PD wurden geprüft. Falscher rkey erzeugte auch für
einen unsignaled WR `REM_ACCESS_ERR`, anschließend `WR_FLUSH_ERR` für die offene
Receive-WQE. Die nächste Runde mit neuen QPs funktionierte auf denselben Rails.

TCP wurde vor den ersten WRs geschlossen: 152 Bootstrap-Bytes je Runde
(beide Richtungen), `tcp_data_calls=0` in sämtlichen finalen Läufen.
PREPARE und STATUS des neuen Modus laufen über USB4-XDomain.

Seit dem finalen Laden um **20:20:48 CEST** traten in den gespeicherten
Kernelprotokollen keine WARN/BUG-Stacks, GPU-Resets oder IOMMU-Fehler auf.
Die Meldungen zu fehlendem MAD-QP1/ib_srpt sind erhalten: Der neue Modus
unterstützt ausschließlich RC und stellt keinen QP1 bereit.

## Leistungsvergleich der Basisversion

Werte in **µs je Nachricht** für WRITE_WITH_IMM, Median der vier ausgegebenen
Mittelwerte (zwei Hosts × zwei Runden, je acht Iterationen bei Tiefe 3).
Die Hosts messen denselben bidirektionalen Austausch, keine vier unabhängigen
Versuche. Die drei Modi liefen nacheinander; die Messung ist eine Diagnose.

| Größe | Bisherige Verbs, Host-MR | Neuer Modus, Host-Staging | Neuer Modus, direktes GPU-MR |
| --- | ---: | ---: | ---: |
| 4 KiB | 144,2 | 1.433,1 | 1.404,0 |
| 64 KiB | 154,4 | 1.448,3 | 1.828,2 |
| 1 MiB | 512,5 | 1.688,1 | 1.697,6 |
| 4 MiB | 1.652,2 | 3.626,4 | 2.845,2 |
| 32 MiB minus 64 Byte | 10.883,7 | 24.564,7 | 15.231,4 |

Bei großen Transfers senkt der direkte Pfad die Kosten gegenüber Staging im
neuen Modus. Gegenüber bisherigen Verbs bleibt WRITE_WITH_IMM langsamer.
PREPARE/STATUS je Batch, Deskriptorbau und globale Serialisierung sind
konkrete Kandidaten für die folgende Phasenmessung; ihre Anteile sind durch
diese Gesamtdauer allein nicht bestimmt.

Gemessene Prozess-CPU-Zeit (User + System, Sekunden): bisheriger Modus
fuzzy 2,18 / misty 2,30; neuer Host-Staging-Modus 1,32 / 1,34; direkter
GPU-Modus 0,66 / 0,66. **Kernel-Arbeitsqueues sind darin nicht erfasst.**
Die Matrizen und Fehlerproben unterscheiden sich; diese Werte sind keine
vollständige CPU- oder Speicherbandbreitenbilanz. Wall-Time enthält auch das
Warten auf den Peerstart und ist für diesen Vergleich ungeeignet.

Die Zeiten enthalten GPU-Produktion, Vorlesen, Transfer, HIP-Kopien bei Host-MRs,
RMA-Fensterbarrieren und vollständige GPU-Prüfung. Sie sind keine reine DMA-Latenz
und kein Gufo-Benchmark. Der bisherige SEND-Pfad hatte in einzelnen Größen starke
Ausreißer und insgesamt 28 Retransmits auf fuzzy; deshalb wird aus seinen Zeiten
kein Leistungsgewinn abgeleitet. Samples aller Operationen stehen in
[timing-samples.json](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-05-verbs-dmabuf/timing-samples.json).

## Behobene Fehler und Grenzen

Die ersten Hardwareversuche deckten diese Fehler auf; die ursprünglichen
Protokolle und Kernel-Stacks bleiben im Ergebnisverzeichnis erhalten:

- RDMA-Core-DMA-BUF-Import erhielt zuvor ein virtuelles DMA-Gerät: gültiger
  Import schlug fehl und `dma_buf_dynamic_attach` warnte. Der neue Modus
  registriert das tatsächliche NHI-Gerät; der bisherige Modus behält virtuelles DMA.
- 256 Byte XDomain-Payload ließ keinen Platz für die 4-Byte-CRC und löste eine
  Thunderbolt-Warnung aus. Das Format ist jetzt 252 Byte; die Protokollprobe
  prüft die Grenze explizit.
- Reclaim-Arbeitsqueues flushen normale Thunderbolt-Ring-Worker und erzeugten
  `check_flush_dependency`. Der neue Modus verwendet hierfür gewöhnliche Queues.
- Kurze Ein-Paket-Frames melden SOF=0; der Validator akzeptiert dies bis 256 Byte
  und prüft weiterhin exakte Länge, EOF und CRC.
- Ein-Byte-GPU-RX beschädigte Wächterbytes. Direkte Längen unterhalb der
  32-Byte-Ausrichtung werden deshalb vor dem DMA abgewiesen.
- Beim Reload müssen zunächst beide Module entladen sein, bevor eines geladen
  wird. Andernfalls kann eine neue Instanz die alte Peer-Generation lernen.
- Der erste Host-Kontrolllauf fehlte eine RMA-Fensterfreigabe nach dem GPU-Lesen.
  Der Test koordiniert Wiederverwendung jetzt durch bidirektionales Verbs-SEND.

RDMA_READ, GPU-Scatter/Gather und Nutzdaten-Retransmission sind noch nicht
implementiert. Datenfehler nach dem Arming stoppen beide Richtungen der
betroffenen Rails; erneutes Laden beider Module ist dann erforderlich.
Hot-Unplug, GPU-Reset, Verlustinjektion, parallele QPs und dauerhafte Serving-Last
sind noch nicht qualifiziert. Weitere Details: [GPU_VERBS.md](../../docs/GPU_VERBS.md).

## Wiederherstellung und Protokolle

Die ursprünglichen Module mit Build-ID
`e898287be93660f988b733c065ab41633a49793c` und alle gesicherten Parameter sind
auf beiden Hosts wiederhergestellt. Vier Rails sind bereit. Die vor diesem
Wartungsfenster vollständig gesicherten Q8-Gufo-Instanzen wurden mit ihrem
ursprünglichen Image und ihren exakten Startparametern neu gestartet:
`upbeat_tharp` auf fuzzy, `boring_pare` auf misty. Gufo meldete am 5. Oktober
um 20:40 CEST auf fuzzy `/ready` mit HTTP 200; misty hat beide Bootstrap-Verbindungen.
Die geschützten Startkonfigurationen bleiben auf den Hosts und sind nicht Teil
dieses Berichts. Nachweise:
[fuzzy](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-05-verbs-dmabuf/fuzzy/restoration-check.txt),
[misty](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-05-verbs-dmabuf/misty/restoration-check.txt).

Je Host liegen im Ergebnisverzeichnis:
`dmabuf-gpu-signaled.log`, `dmabuf-gpu-unsignaled-errors.log`,
`dmabuf-gpu-timed.log`, `dmabuf-host.log`, `normal-host-timed.log`, die zugehörigen
`.time`-Dateien, Transportzähler, Buildprotokolle und Kernelprotokolle.
`validation-kernel.log` erfasst die finalen DMA-BUF-Läufe,
`normal-timed-kernel.log` den abschließenden bisherigen Modus,
`window-kernel.log` zusätzlich die ersten fehlgeschlagenen Versuche.
Die ursprünglich angehaltene Testserie und der spätere Neustart ändern diese
Abgrenzung nicht.
