# GPU-RDMA / Gufo: Stand vom 4. Oktober 2026

Dieser Bericht dokumentiert den exklusiven ioctl-Pfad vor der Verbs-Integration.
Der anschließend entwickelte Implementierungsstand hinter normalen MRs und
Verbs-Operationen wird separat in [GPU_VERBS.md](../GPU_VERBS.md) geführt. Die
Messungen und offenen Qualifikationen hier beziehen sich auf den älteren Pfad.

## Ergebnis

Der angefragte Datenpfad funktioniert auf **fuzzy und misty**, verbunden durch
zwei USB4-Kabel mit insgesamt vier Donnerkeule-Rails:

```text
Gufo GPU-Projektion / MoE-Epilog
  → HIP-TX-Puffer, als DMA-BUF exportiert
  → NHI liest dieselben Seiten, Striping über vier Rails
  → NHI schreibt den HIP-RX-Puffer des Peers
  → GPU liest dieselben RX-Seiten und summiert die Teilergebnisse
```

Es gibt dabei keine CPU-Nutzdatenkopie und keine zusätzliche GPU-Stagingkopie.
CPU-Steuerung, GPU-Synchronisation und die eigentliche GPU-Summation bleiben
notwendig. Das ist ein **experimenteller, exklusiver Nachrichtentransport**;
es ist noch kein DMA-BUF-fähiger Verbs-Memory-Region-Pfad und kein Ersatz für
alle Zuverlässigkeits- und Adressierungsfunktionen von RC.

Der Q8-Modelltest stimmt bei 2048, 383/385 und 32768 Prompttokens pro Anfrage
zwischen beiden Rechnern und beiden Transporten in vollständigen Logit-Hashes,
Tokens und aktiven Anfragegruppen überein. Ein 64K-Prefill wurde noch nicht
abgeschlossen: Der Versuch scheiterte schon beim Modellladen an langsamen
lokalen NVMe-Lesezugriffen auf fuzzy.

## Implementierter Stand

### Donnerkeule

- `include/tbv_gpu_direct.h`: versionierte ioctl-Schnittstelle, Topologie,
  Import, RX-Vorbereitung, TX-Start, Completion-Wait und Abort.
- `kernel/gpu_direct.c`: dedizierte HIP-DMA-BUFs importieren, an die tatsächlichen
  NHI-Geräte der Rails binden und deren DMA-Adressen in Deskriptoren verwenden.
  Keine CPU-Abbildung der Nutzdaten. Import-, Bereichs- und Completion-Prüfungen.
- Exklusives `/dev/tbv_gpu_direct`, `CAP_SYS_RAWIO`, maximal eine Sitzung.
  `gpu_direct=1` erfordert `register_verbs=0` und native Transportkonfiguration.
- Maximal 512 4-KiB-Frames pro Rail und Batch, somit 8 MiB bei vier Rails.
  Größere Nachrichten werden in mehrere Batches zerlegt.
- Ein nachweislich ausgebliebener letzter Completion-Interrupt wird durch eine
  begrenzte erneute Einplanung desselben NHI-Completion-Workers behandelt.
  Erfolg erfordert echte Deskriptor-Completions; keine fingierte Bestätigung.
- Abbruch und Schließen drainieren DMA vor dem Freigeben der Abbildungen.
  Nach einer Sitzung mit Nutzdaten müssen beide Testmodule neu geladen werden;
  ein generationengesicherter Wiederverbindungsmechanismus fehlt noch.

### Gufo

- `--tp-transport donnerkeule` wählt den experimentellen Adapter. **`verbs`
  bleibt Standard.** Der vorhandene authentifizierte TP-Steuerkanal bleibt nötig.
- Drei 32-MiB-Fenster pro Richtung in dedizierten HIP-Allokationen von je 96 MiB.
  TX verwendet cachebares `hipMalloc`, RX derzeit ungecachten HIP-Speicher.
- Der Executor reserviert das Sendefenster vor der letzten Projektion bzw. dem
  MoE-Epilog. Der GPU-Kernel schreibt direkt dorthin. Ein System-Release-Marker
  liegt vor dem Bereitschaftsflag; der GPU-Konsument wartet auf das Empfangsflag.
- TCP überträgt ausschließlich Topologie, Scope, Operationsnummer, Größe und
  Bereitschaft. Die Nutzdaten laufen über NHI-DMA.
- Die bestehende Prefill-Planung mit zwei Teilen unterstützt jetzt direkte
  Pufferreservierungen: A veröffentlichen, B berechnen, erst dann auf A warten
  und summieren. Zwei ausstehende Teilergebnisse passen in drei Fenster.
  Wiederverwendung erfolgt erst hinter dem vorherigen GPU-Lesezugriff im
  selben Stream. Unter 384 Tokens sowie beim Decode bleibt der unmittelbare Wait.
- Die leere synchrone Barriere erlaubt nach beobachteten Ladezeitunterschieden
  nun bis zu 180 Sekunden Empfangswartezeit. Danach werden 30 Sekunden
  wiederhergestellt. DMA- und Datenkontroll-Zeitlimits wurden nicht erweitert.
- Der Modelltest wurde um `--prefill-sweep N,N,...` ergänzt, damit mehrere
  Größen bei einmal geladenem Modell laufen können. Jede Größe erhält frische
  Anfragezustände und einen eigenen Scope. **Build vorhanden; ein vollständiger
  GPU-Lauf dieses neuen Mehrfachtests steht noch aus.**

## Messungen und ihre Grenzen

### Transportdiagnose

256 gemessene Austausche je Größe nach acht Aufwärmaustauschen, bidirektional.
Die Zeit umfasst GPU-Erzeugung, RX-Vorlesen, Transfer, GPU-Summation und komplette
GPU-Prüfung. Sie ist keine reine DMA-Zeit und keine Einweg-Netzwerklatenz.

| Nutzdaten pro Richtung | Verbs | Direkt | Verbs GB/s | Direkt GB/s |
| --- | ---: | ---: | ---: | ---: |
| 1 MiB | 355,8 µs | 375,0 µs | 2,947 | 2,796 |
| 4 MiB | 1181,8 µs | 1351,3 µs | 3,549 | 3,104 |
| 32 MiB − 64 B | 8643,2 µs | 12807,0 µs | 3,882 | 2,620 |

Bei 1 MiB benötigt der direkte Pfad rund **36 % mehr Prozess-CPU-Zeit** pro
Austausch; durchschnittlich etwa 2,57–2,58 statt 2,0 CPU-Kerne pro Prozess.
IRQ- und unabhängige Kernel-Worker-Zeit sind darin nicht enthalten.
**GPU-Auslastung und Energie wurden nicht gemessen.** Frühere kürzere Kohorten
zeigen abweichende genaue Zeiten; eine unveränderliche Prozentzahl wird nicht
behauptet. Der abschließende direkte Lauf prüft 12.548.983.392 Bytes pro Richtung
fehlerfrei. Absichtliche Größenabweichung: Abbruch vor TX, keine Nutzdaten,
kein hängenbleibender GPU-Wait.

Die frühere reine NHI-DMA-Messung erreicht etwa 4,1–4,2 GB/s je Richtung.
Dieser engere Zeitabschnitt ist nicht mit der Tabelle gleichzusetzen.

### Modell-Prefill

Jeweils zwei synthetische Prompts; die Tabelle nennt die Summe beider Prefills.
Modellladen liegt außerhalb der gemessenen Prefill-Zeit. Es handelt sich um
gezielte Diagnoseläufe, nicht um qualifizierte Produktions-/Serving-Benchmarks.

| Fall | Verbs | Direkt ohne Überlappung | Direkt mit Überlappung |
| --- | ---: | ---: | ---: |
| 2 × 2048 Tokens | 4,157 s, früherer Kontrolllauf | 4,973 s, frische Kontrolle | 3,941 s |
| 2 × 32768 Tokens | 59,407 s / 1103,2 Tokens/s | nicht gemessen | 57,308 s / 1143,6 Tokens/s |
| 2 × 65536 Tokens | nicht gemessen | nicht gemessen | Modellladen abgebrochen; keine Prefill-Messung |

Die Überlappung reduziert den anfänglichen direkten Prefill im kleinen Test
um etwa 21 %. Bei 32K beträgt der Abstand zu Verbs in **einem Vergleich** etwa
3,5 % zugunsten des direkten Pfads. Die Wiederholbarkeit dieses kleinen
Vorteils ist noch offen. Die 32K-Prefills tauschen je Rang 3072 Teilergebnisse
zu je 20.971.520 Bytes aus, insgesamt 64.424.509.440 Bytes pro Richtung.
Die vollständigen Logit-/Token-Signaturen stimmen überein.

Die 2048-Token-Prompts stoppen nach 1 bzw. 3 erzeugten Tokens bei EOS; die großen
Tests verwenden nur acht Decode-Tokens als Fortsetzungsprüfung. **Keiner dieser
Läufe ist ein tg128-Benchmark.** MTP, Sampling, Bilder, Cache-Wiederherstellung,
Abbruch mit ausstehenden direkten Pufferreservierungen und höhere Serving-
Parallelität sind damit nicht qualifiziert.

## Einordnung der Kosten

Die fehlende Überlappung war ein nachgewiesener Beitrag zur ersten Prefill-
Regression: Ihre Ergänzung verbessert den Modelltest bei identischem Ergebnis.
Weitere plausible Kosten sind die GPU-System-Release-Marker, ungecachte
RX-Lesezugriffe, TCP-Bereitschaftsabstimmung, mehrere ioctls pro DMA-Batch und
serielle Batch-Abwicklung. Ihr jeweiliger Anteil wurde noch nicht profiliert.

Cachebares TX wurde beibehalten. Ein Versuch mit feinkohärentem RX-Speicher
bestand die Korrektheitsprüfung, zeigte aber keinen durchgängigen Zeitvorteil
und wurde verworfen. „Zero-Copy“ allein bedeutet weder weniger CPU-Verbrauch
noch automatisch weniger Wartezeit.

## Verbs-Kompatibilität und empfohlene weitere Architektur

Im normalen Modus bleibt die vorhandene libibverbs-Schnittstelle vorgesehen.
Der direkte Modus ist **nicht gleichzeitig mit Verbs verfügbar** und implementiert
noch **kein `ibv_reg_dmabuf_mr()`**. Die bisherigen Verbs-Hardwarekontrollen
verwendeten das gesicherte ältere Modul. Ein gesonderter Kompatibilitätslauf des
geänderten Moduls mit `gpu_direct=0, register_verbs=1` steht noch aus; er wurde
vor der Dokumentationsanforderung vorbereitet, aber nicht ausgeführt.

**Empfehlung:** Den dauerhaften Transport-Fastpath in Donnerkeule hinter die
Verbs-Schnittstelle bringen und den Gufo-Spezialtransport danach zurückbauen.
Der aktuelle Adapter bleibt bis dahin ein isolierter Referenz-/Testpfad.

Dazu sind mehr als ein Funktionsaufruf und eine DMA-Adresse nötig:

1. DMA-BUF-Registrierung als Memory Region einschließlich Größen-/Offsetprüfung,
   Berechtigungen, DMA-Abbildung je NHI-Gerät und sicherer Lebensdauer umsetzen.
2. Direkte TX/RX-Deskriptoren mit QP-/WR-Zuordnung, Receive-Bereitschaft,
   Reihenfolge, Completion und dem bestehenden Zuverlässigkeitsprotokoll
   verbinden. Verhalten bei Fehlern und Wiederverbindung ausdrücklich prüfen.
3. GPU-Sichtbarkeit und Ownership zwischen HIP-Produzent, NHI und HIP-Konsument
   definieren; das Importieren eines DMA-BUF allein löst die Cachefrage nicht.
4. In Gufo nur die notwendigen Speicher-/Produzentenänderungen beibehalten:
   vorab reservierte registrierte Ausgabepuffer, direkte GPU-Ausgabe, passende
   Release/Acquire-Grenzen und sichere Pufferwiederverwendung. Diese Änderungen
   können transportneutral auch dem Verbs-Adapter dienen.
5. Mit gleichem geladenem Modell, gleicher Aufwärmfolge und frischen Sessions
   32K/64K wiederholen; danach Produktions-pp2048/tg128 und gezielte Profile.

Diese Verbs-Integration ist **noch nicht implementiert**. Die GPU-Funktionsprobe
rechtfertigt nicht, den experimentellen Adapter zum Standard zu machen.

## Arbeitsstände, Tests und offener Betriebszustand

- Donnerkeule: `/Users/user/Documents/thunderbolt-ibverbs`, Branch
  `feat/write-striping`, Basis `e37ecaf709a6d82171a97791dcf6e9ebc92abf69`.
- Gufo: `/Users/user/Documents/gufo`, Branch `feat/tp2-cache-prs`, Basis
  `c15a6234cfcd607a731eb666bd45298b83f0d298`. Der zuvor saubere lokale Stand
  `6ddc7cc2` wurde auf diesen bereits vorhandenen Testhost-Stand fast-forwarded;
  anschließend wurden die Aufgabenänderungen lokal angewendet.
- Änderungen sind **nicht committet und nicht gepusht**. Gufo wurde zunächst
  in `/tmp/gufo-donnerkeule-20261003` entwickelt/gebaut und dann lokal übernommen.
- Isoliertes Testverzeichnis auf beiden Hosts:
  `/home/sn/tbv/gpu-direct-lab-20261002`; Nix-Buildcontainer `nixbox` auf fuzzy.
  Die sonstigen Gufo-Quellverzeichnisse der Hosts wurden nicht überschrieben.
- Pinned Nix-Toolchain: GCC 15.3, HIP clang 22, ROCm 7.2.3, gfx1151.
  HIP-/Kernel-/Geometrie-Prüfungen und betroffene Gufo-Testtargets gebaut;
  `serve.cpp` als betroffenes Objekt kompiliert. Kein vollständiger neuer
  Produktions-Gufo-Build und keine neue Serverausrollung.
- Letzte Prüfung: **keine laufenden Testprozesse und keine offenen GPU-/Verbs-/
  Direkt-Geräte durch Tests**. Beide Hosts stehen auf `gpu_direct=Y`,
  `register_verbs=N`. Die ursprünglichen Gufo-Dienste wurden nicht neu gestartet.
- 64K-Ladeproblem: fuzzy zeigte wartende Modelllade-Threads in `iomap_dio_rw`,
  hohen aggregierten I/O-Druck und nur langsam steigende Lesezähler auf
  `/dev/nvme0n1p2` (`/opt`, ext4). NVMe-Zustand war `live`. Ursache und Behebung
  sind offen; ein Defekt oder eine thermische Ursache ist nicht nachgewiesen.
  Der eigene Test wurde beendet, nachdem der Peer die Startbarriere abbrach.
- Frühere Kernel-WARN-Ursache, physisches Abziehen, GPU-Reset und IOMMU-Fehler
  bleiben offen. Die damalige Einsicht in Kernelprotokolle wurde von der
  automatischen Freigabeprüfung abgelehnt; sie wurde nicht umgangen.

## Nachweise

- [Gufo-Messbericht und Reproduktionsdaten](../../bench/results/2026-10-03-gufo-direct.md)
- [Rohlogs und Prüfer](../../bench/results/2026-10-03-gufo-direct/)
- [Erster reiner GPU/NHI-Hardwaretest](../../bench/results/2026-10-02-gpu-direct.md)
- Direkte ABI und Betriebsregeln: `docs/GPU_DIRECT.md`, mit dem `gpu_direct`-Pfad entfernt
- Gufo: `docs/models/qwen3.8-flash-next/DONNERKEULE.md`, ergänzende Einträge in
  `BENCHMARKS.md`, `QUALITY.md` und `EXPERIMENTS.md`.

Reproduzierbare Prüfung der bisher abgeschlossenen Log-Kohorten:

```sh
python3 bench/results/2026-10-03-gufo-direct/verify.py
```

Vorbereiteter Mehrfachtest für den nächsten Versuch, noch nicht als erfolgreich
gewertet: auf beiden Rängen gleicher Kontext und gleiche Größenfolge, z. B.
`--context 66048 --width 2 --prefill-sweep 4096,32768,65536 --max-tokens 8`.
Der erste Fall dient als offengelegter Warm-up; Modell und Transport bleiben
zwischen den Fällen geladen, die Anfragezustände werden neu erzeugt.
