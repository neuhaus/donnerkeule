# DMA-BUF hinter Verbs: experimenteller Implementierungsstand

Update vom 16. Oktober: Die [erneute Treiberprüfung und Posting-Optimierung](../bench/results/2026-10-16-verbs-parity.md)
bestätigt die vier RC-SEND/WRITE-Operationen mit GPU- und Host-MRs.
Abwechselndes Posting auf den Rails desselben NHI bringt die geprüften
1–32-MiB-Zyklen nahe an den normalen Host-Pfad. Kleine Nachrichten bleiben
langsamer; vollständige Latenzparität und Gufo-Integration sind damit nicht
qualifiziert. Für diese Messungen war `dmabuf_push_done=1` ausdrücklich gesetzt.
Die Standardwerte bleiben unverändert.

Nachtrag: [Vorbereitete SEND-Credits](../bench/results/2026-10-16-verbs-credits.md)
(`dmabuf_credits=1`, standardmäßig aus) entfernen PREPARE bei GPU-SENDs in
gepostete Receive-WQEs. Im Muster ohne Barriere sanken 32 B und 10.304 B auf
etwa 88–91 µs je geprüfter Nachricht; WRITE bleibt unverändert. Siehe
[Vorbereitete SEND-Credits](#vorbereitete-send-credits).

Der neue native Modus `dmabuf_verbs=1` importiert HIP-DMA-BUFs über
`ibv_reg_dmabuf_mr()` und verwendet ihre NHI-DMA-Adressen hinter normalen
`ibv_post_send()`, `ibv_post_recv()` und CQ-Completions. Die Nutzdaten werden
weder auf die CPU abgebildet noch in einen GPU-Zwischenpuffer kopiert.

Das ist ein separat einschaltbarer experimenteller Implementierungsstand.
Der erste Stand hat am 5. Oktober 2026 auf fuzzy und misty alle vier Operationen
mit direkten GPU-MRs bestanden, einschließlich Wächterbytes, unsignaled
Fehler-Completions, RNR und QP-/MR-Neuerzeugung. Normale Host-MRs bestanden sowohl
im bisherigen Modus als auch im neuen Staging-Pfad. Der Leistungsvergleich
zeigt noch höhere WRITE-Latenz als im bisherigen Verbs-Pfad. Ergebnisse,
behobene Fehler und Protokolle stehen in
[2026-10-05-verbs-dmabuf.md](../bench/results/2026-10-05-verbs-dmabuf.md).
Die anschließende [Optimierungsrunde](../bench/results/2026-10-05-verbs-dmabuf-optimization.md)
indiziert DMA-Adressen, räumt RX beim DMA-Abschluss auf und übergibt große
Deskriptorpakete parallel an unterschiedliche NHIs. Bei 1 und 20 MiB sank die
Latenz innerhalb des direkten Pfads um etwa 21 %. Die Lücke zum bisherigen
Verbs-Pfad bleibt bestehen; Gufo-Modellleistung wurde damit noch nicht gemessen.
Der anschließend gebaute [NHI-Steuerkanal-Prototyp](../bench/results/2026-10-06-verbs-control-ring.md)
hat die vollständige Matrix ebenfalls bestanden. Sein isolierter Kontroll-PING
ist nur wenig schneller als XDomain. Die Test-NHIs haben keine freien Ringe
neben zwei Datenrails; das Reservieren eines Kontrollrings je Verbindung
verschlechtert deshalb die großen Transfers. Der Prototyp bleibt separat
ausgeschaltet (`dmabuf_control_ring=0`).
Die Variante [aktiv gesendetes DONE auf XDomain](../bench/results/2026-10-07-xdomain-done.md)
benötigt keine zusätzlichen NHI-Ringe und bleibt separat ausgeschaltet
(`dmabuf_push_done=0`). Die [Hardwareprüfung vom 8. Oktober](../bench/results/2026-10-08-hardware.md)
bestand mit GPU- und Host-MRs, Remote-Fehlern, RNR und gezielt verlorenen DONEs.
Bei 10.304 B sank die gemessene Zykluslatenz um 16,6 %; große Transfers blieben
praktisch gleichauf. Nach erzwungenem Link-Abbau blieben Daten korrekt, aber
normale Interrupts und Latenz kehrten erst nach einem ACPI-NHI-Reset zurück.
Die Messungen vom 2.–4. Oktober in
[GPU_RDMA_STATUS_2026-10-04.md](GPU_RDMA_STATUS_2026-10-04.md) betreffen den
früheren exklusiven ioctl-Pfad und dürfen nicht als Messungen dieser Integration
gelesen werden. Der Standardmodus bleibt `dmabuf_verbs=0`.

## TCP und bisherige Versuche

Gufo hat den TCP-Header und die TCP-Bestätigung pro normalem Verbs-Austausch
bereits entfernt: Der Versuch „Header-free RDMA exchange“ in
`docs/models/qwen3.8-flash-next/EXPERIMENTS.md` verwendet WRITE_WITH_IMM und drei
Fenster. Dokumentiert sind 46 → 24 µs für eine Zeile sowie +2,5 % AR und
+4,3 % MTP. Das ist ein anderer Versuch als der direkte HIP-DMA-BUF-Adapter.

Der bisherige direkte Adapter stimmt Bereitschaft und Operationen über TCP ab.
Seine Transportdiagnose war trotz entfallener Nutzdatenkopien langsamer als
normales Verbs. Die erhaltene Prefill-Überlappung und die verworfene Variante
mit feinkohärentem RX sind ebenfalls dokumentiert; siehe die Statusdatei.
DMA-BUF-Import hinter Verbs wurde dort ausdrücklich als nächste Arbeit genannt.

Der neue Treibermodus benutzt standardmäßig für PREPARE, STATUS und ABORT den bestehenden
Thunderbolt-XDomain-Konfigurationskanal. Er benötigt kein TCP pro WR. Anwendungen
dürfen weiterhin einen anfänglichen TCP-Kanal für GIDs, QPNs, IOVAs und rkeys
verwenden. Der neue Test schließt diesen Kanal vor dem ersten WR.

Der Konfigurationskanal benötigt mindestens eine Vorbereitung und eine
Bestätigung pro Batch; zusätzlich sind STATUS-Abfragen möglich. Derzeit werden
WRs über eine globale geordnete Arbeitsqueue serialisiert. Damit ist weder
geringere Latenz noch eine Verbesserung des Gufo-Prefills nachgewiesen.
„Kein TCP“ und „keine Kopie“ ersetzen keine Messung dieser Kosten.

## Aktiv gesendetes DONE auf XDomain

`dmabuf_push_done=1` handelt die Fähigkeit `PUSH_DONE` über HELLO aus.
Unterstützen beide Peers sie und ist kein separater Kontrollring ausgehandelt,
bleiben PREPARE, seine Bereitschaftsantwort und ABORT auf dem vorhandenen
XDomain-Konfigurationskanal. Der Empfänger sendet dort jeden vollständig
platzierten Batch als DONE. Auf den Test-NHIs kann dieser Pfad weiterhin
`lanes=2` und damit insgesamt vier Datenrails verwenden. Er legt weder weitere
NHI-Ringe noch HopIDs oder einen periodischen Kontroll-Worker an.

Die Quelle registriert die vollständige Transaktion vor PREPARE. Eine früh
ankommende Abschlussmeldung bleibt gespeichert; die erfolgreiche Source-CQ
setzt weiterhin auch den lokalen TX-DMA-Abschluss voraus. Ein leerer WR
schließt direkt in der PREPARE-Antwort ab, ohne zusätzlichen DONE-Frame oder
STATUS-Roundtrip. Fehlt die Meldung nach 10 ms, bestätigt die bestehende
STATUS-Abfrage die gecachte Platzierung. Ein fehlgeschlagener Versand einer
DONE-Meldung verwirft diese Platzierungsinformation nicht und führt nicht zur
erneuten Übertragung der Nutzdaten.

DONE wird vor dem allgemeinen XDomain-Antwortmatcher konsumiert. Dieser prüft
Route und Protokoll-UUID, jedoch nicht unseren Token oder Opcode. Auch
verspätete oder fehlerhafte DONE-Nachrichten dürfen deshalb keine andere
PREPARE- oder HELLO-Anfrage beantworten. Nur vollständig validierte Meldungen
mit passender Boot-Generation, Domain, QPs, PSN, Rails und Batch wecken die
registrierte Transaktion. Normale Antworten bleiben beim bestehenden Matcher;
`peer->control_lock` serialisiert diese Anfragen weiterhin. Software-Referenzen
halten den RX-Abschlusstracker auch bei einem Verbindungsabbau auf einem anderen
Datenlink gültig; die Hardware-Ringe werden beim Peer-Abbau weiterhin sofort
gestoppt und freigegeben.

Der Modus bestand die genannten Hardwarefälle und bleibt standardmäßig aus.
Ein schneller automatischer Wiederanlauf nach Link-Abbau ist nicht qualifiziert.
Für einen isolierten Test setzt das Laborskript
`TBV_LAB_PUSH_DONE=1 TBV_LAB_CONTROL_RING=0 TBV_LAB_LANES=2`.
Debugfs zeigt `dmabuf_push_done`, je Peer `push_done: ... ready=1` sowie
`control_done_rx` und `control_notify_failed`. Die vorhandene Phasenmessung
trennt `done_wait` von tatsächlich notwendigen STATUS-Anfragen.
Die reine PING-Messung bleibt auf XDomain und sollte allein durch diesen
Modus keine kürzere Roundtrip-Zeit erhalten. Er spart Nachrichten pro Batch;
ein Nutzen für Gufo muss noch gemessen werden.

## Separater NHI-Steuerkanal

`dmabuf_control_ring=1` handelt die zusätzliche Fähigkeit über HELLO aus.
Unterstützen beide Peers sie, richtet XDomain einen eigenen Kontrolltunnel
über HopID-Austausch und Bereitschaftsbestätigung ein. Danach laufen PREPARE, die Bereitschaftsantwort, ABORT
und aktiv gesendetes DONE über eigene NHI-Ringe. Bei abgeschalteter Fähigkeit
auf einem Peer bleibt die Kommunikation auf dem bisherigen XDomain-Pfad.
Ein bereits begonnenes DMA wechselt den Transport nicht.

Der Kanal verwendet je physischer Verbindung einen TX- und RX-Ring mit 128
Einträgen, 32 gepoolte TX-Puffer, 64 dauerhaft gepostete RX-Puffer und 64
vorbereitete Ereignisplätze. Eine Nachricht enthält 256 Byte Host-Metadaten.
GPU-Nutzdaten bleiben auf ihren exklusiven Datenringen. RX-Callbacks kopieren
und posten ihren kleinen Host-Puffer erneut; ein separater Worker bereitet
den GPU-Empfang vor. Bis zu 16 Kontrolltransaktionen werden nach vollständigem
Token, Boot-Identitäten, Domain, QPs, PSN und Batch zugeordnet. Die globale
Serialisierung der Nutzdaten-WRs und der aktive einzelne RX-WR bestehen weiter.

Die Quelle registriert DONE schon vor PREPARE und behält eine früh eintreffende
Meldung bis zum lokalen TX-DMA-Abschluss. Der Empfänger meldet jeden vollständig
platzierten Batch aktiv. Erst danach darf die Quelle weitergehen. Die Verbs-CQ
beim Empfänger erscheint weiterhin einmal für den gesamten WR. Leere
Besitzübergabe-WRs werden unter derselben RX-Sperre sofort abgeschlossen.
Die bisherigen QP-, PSN-, PD-, rkey-, Bereichs- und Lebensdauerprüfungen gelten
auch hier.

Nach 10 ms ohne DONE fragt die Quelle den gecachten STATUS als Wiederherstellung
ab. Der normale Austausch benötigt diese Abfrage nicht. Der Prototyp prüft
zusätzlich jede Millisekunde den Ringfortschritt über die vorhandenen NHI-Worker
und wartet bei einem vorübergehend belegten TX-Pool auf dessen Freigabe.
Das behebt im Hardwaretest ausgebliebene Kontroll-TX-Completions, verursacht
aber zusätzliche CPU-Arbeit.

**Ringbudget auf fuzzy und misty:** Jeder NHI meldet drei Hops. Hop 0 gehört
dem Konfigurationskanal; zwei Datenrails belegen die übrigen Ringe. Für diesen
Prototyp deshalb auf beiden Hosts `lanes=1` verwenden: ein Datenring und ein
Kontrollring je NHI, insgesamt zwei Datenrails über beide Kabel. Ohne freien
Ring wird der angeforderte Kanal abgewiesen. Solange er nicht bereitsteht,
werden GPU-WRs mit `ENOTCONN` abgewiesen. Vor dem Test in Debugfs `peers` für jede
Verbindung `control_ring: ... ready=1` prüfen.

Die rootgeschützte Messfunktion benötigt keine GPU-Allokation. Sie führt
1–100.000 Kontroll-PINGs auf dem angegebenen Peer aus und liefert Mittelwert,
Minimum, Median, P95, P99 und Maximum in Nanosekunden. Ein Modulparameter
ermöglicht die Messung unter dem bestehenden Kernel-Lockdown:

```sh
echo '1 50000' | sudo tee /sys/module/thunderbolt_ibverbs/parameters/dmabuf_control_bench
sudo cat /sys/module/thunderbolt_ibverbs/parameters/dmabuf_control_bench
```

In Debugfs `peers` stehen Ringbereitschaft, Fehler, gesendete/empfangene
Kontrollframes, PREPARE, DONE, STATUS, unbeantwortete Zuordnungen und Pooldruck.
`control_tx_ns` und `control_rx_work_ns` sind verstrichene Bearbeitungszeiten,
keine isolierten CPU-Zyklen. Die separate Messung und der Vergleich bei
gleicher sowie voller Datenrail-Zahl stehen im Ergebnisbericht.

## Vorbereitete SEND-Credits

`dmabuf_credits=1` (Standard 0) entfernt bei GPU-SENDs den PREPARE-Roundtrip
vom kritischen Pfad. Voraussetzung sind `dmabuf_push_done=1`,
`dmabuf_control_ring=0` und die über HELLO ausgehandelte Fähigkeit
`DMABUF_CREDIT` auf beiden Peers; sonst bleibt der bisherige Ablauf aktiv.
Die Frames eines NHI-RX-Rings landen in der Reihenfolge der geposteten
Deskriptoren. Ein Ziel muss deshalb vor den Daten bekannt sein. Bei SEND
kennt es der Empfänger: Es ist die nächste Receive-WQE. Bei WRITE kennt es
nur der Sender; WRITE behält PREPARE.

Ablauf:

1. Nach einem erfolgreich platzierten GPU-SEND bzw. SEND_WITH_IMM merkt sich
   der Empfänger QP, Länge und Lanes. Steht die nächste WQE schon bereit oder
   wird sie später gepostet (`ibv_post_recv()`), prüft der Treiber sie wie
   bei PREPARE (QP, PD, lkey, Rechte, Bereich, 4-KiB-Ausrichtung) und armiert
   ihre GPU-Seiten für genau diese Länge und den erwarteten PSN. Die WQE
   bleibt gepostet. Anschließend sendet er CREDIT an den Sender.
2. Ein SEND mit gleichem Ziel-QP, PSN, gleicher Länge und gleichen Lanes
   verwendet den Credit. Der Sender meldet COMMIT einseitig mit den vollständigen
   WR-Metadaten und postet seine Frames sofort, ohne auf eine Antwort zu
   warten. Der Empfänger übernimmt die WQE erst bei COMMIT; Replay-Wasserzeichen,
   QP-, PSN- und Lane-Prüfungen gelten wie bei PREPARE. DONE und die
   STATUS-Wiederherstellung bleiben unverändert.
3. Kreuzt sich ein PREPARE mit einem noch unterwegs befindlichen Credit,
   übernimmt der Empfänger das Armieren für diesen PREPARE und nennt den
   Credit in READY. Der Sender verwirft ihn dann.
4. Ein Credit, den der nächste WR nicht verwenden kann (andere Länge oder
   Operation, mehrere Batches), wird vor dessen PREPARE freigegeben: Der
   Sender füllt genau die armierten Frames mit Nullen und meldet RELEASE. Ein
   nicht passender PREPARE erhält BUSY und löst ein CANCEL aus. QP-Zerstörung
   oder QP-Fehler beim Empfänger fordert die Freigabe ebenfalls an und wartet
   bis zu 500 ms. Ohne RELEASE werden die Rails wie bei jedem Abbruch nach dem
   Armieren stillgelegt.
5. Erhält ein PREPARE RNR, wartet der Sender bis zu 2 ms auf einen Credit
   statt 1–2 ms zu schlafen und erneut zu fragen. Das Posten der WQE beim
   Empfänger ist damit das Signal zum Senden.

6. Ist kein anderer TX-Auftrag eingereiht oder aktiv, nimmt bereits
   `ibv_post_send()` einen passenden Credit, sendet COMMIT und postet die
   Frames (`dmabuf_direct_post=1`, Standard). Der TX-Worker wartet danach nur
   noch auf DMA und DONE und schließt den WR ab. Das spart das Aufwecken des
   Workers vor den Daten. Eine Sperre umfasst Leerlaufprüfung, direktes
   Posten und Einreihen, sodass spätere WRs dahinter bleiben.

Credits gelten nur für einen Batch und höchstens `dmabuf_credit_max` Byte
(Standard 1 MiB). Es gibt höchstens einen armierten Credit je Empfänger, wie
es auch nur eine aktive RX-Transaktion gibt. Die Credit-Nummer liegt im bisher
reservierten Feld bei Byte 164; ältere Peers handeln die Fähigkeit nicht aus.

**Speichervertrag im Credit-Modus:** Solange eine GPU-Receive-WQE gepostet ist,
kann eine Freigabe ihre ersten `ALIGN(L, 4096)` Byte mit Nullen überschreiben,
wobei L die Länge des vorherigen SENDs auf diesem QP ist. Das geschieht nur,
wenn der folgende WR den Credit nicht verwendet. Bytes jenseits der Länge eines
später kürzer empfangenen SENDs sind dann nicht mehr die vor dem Posten
geschriebenen Werte. Wer Inhalte hinter der empfangenen Länge erwartet, darf
den Modus nicht verwenden.

Debugfs `summary` zeigt `dmabuf_credits`, `dmabuf_credit_max` sowie
`dmabuf_credit_granted`, `committed`, `adopted`, `released`, `canceled`,
`aborted` (Empfänger) und `used`, `returned`, `stale`, `direct` (Sender). `mismatch`
zählt unerwartete Credit-Meldungen; deren Lanes werden stillgelegt.
Profiling-Tracepoints: `credit_grant`, `credit_received`, `credit_commit`,
`credit_commit_rx`, `credit_release`, `credit_release_rx`, `rnr_credit_wait`,
`tx_direct`.

## Schnittstelle und Speichervertrag

- Native RC-QPs: SEND, SEND_WITH_IMM, RDMA_WRITE und RDMA_WRITE_WITH_IMM.
  RDMA_READ, UC, GSI, SRQ und GPU-Scatter/Gather sind in diesem Modus noch nicht
  unterstützt. Der Standardmodus behält seine bisherigen Operationen.
- MR-Import: gültiger DMA-BUF-FD, Offset, Länge und IOVA mit Seitenausrichtung;
  maximal 256 MiB pro Exportobjekt. Zulässige Rechte sind LOCAL_WRITE und
  REMOTE_WRITE; REMOTE_WRITE setzt LOCAL_WRITE voraus. Schlüssel und PD werden
  vor der Platzierung geprüft. Ein Import ist eine MR, kein exklusives Gerät.
- GPU-WR: eine zusammenhängende SGE, deren Offset in der MR ein Vielfaches von
  4096 ist. Direkte GPU-Längen müssen Vielfache von 32 Byte sein, einschließlich
  0; maximal 248 MiB. Ein-Byte-Transfers beschädigten im Hardwaretest Wächterbytes
  hinter der Nutzlast. Solche Längen werden deshalb vor dem DMA abgewiesen.
  Kurze letzte Frames mit 32-Byte-Ausrichtung sind erlaubt. RX benötigt auch
  für einen kurzen Rest eine vollständig
  hinterlegte 4-KiB-Seite. Eine GPU-SEND-Receive-WQE muss diese aufgerundete
  Spanne reservieren; ihre CQ-Completion nennt die logische Länge.
- Die RDMA-Core-Registrierung hält den Exporter fest. Zusätzliche statische
  Attachments werden an die tatsächlichen NHI-Geräte gebunden. Nur deren
  DMA-Adressen werden in Deskriptoren verwendet. Keine `vmap()`-Abbildung und
  keine CPU-Synchronisation der GPU-Nutzdaten.
- MR- und QP-Referenzen bleiben bis zum Ende der betreffenden DMA-Operation
  bestehen. Fehler und Abbruch drainieren Deskriptoren vor der Freigabe.
  Anwendungen müssen wie üblich MRs und Puffer bis zur Completion verfügbar
  halten und dürfen GPU-Konsumentenfenster erst nach deren Lesezugriffen erneut
  für DMA freigeben.
- Normale Host-MRs funktionieren im selben Modus über begrenztes kohärentes
  Staging. Dieser Teil kopiert Nutzdaten und ist kein direkter GPU-Pfad.

Die GPU-Sichtbarkeit bleibt eine Anwendungsaufgabe. Vor TX ist ein System-Release
des Produzenten nötig; nach RX muss der GPU-Lesezugriff korrekt geordnet sein.
Der Test übernimmt den erprobten timing-aktivierten `hipEventReleaseToSystem`
und ungecachten RX. Das ist die bisherige Acquire-Probe für diese Hardware und
HIP-Laufzeit, keine portable Garantie allein durch MR-Registrierung oder CQ.

## Platzierung, Reihenfolge und Fehler

Ein WR wird in Batches mit maximal 512 Frames je Rail zerlegt. Maximal vier
Rails passen in einen 252-Byte-Konfigurationsdatensatz plus 4 Byte CRC; bei vier
Rails enthält
ein Batch höchstens 8 MiB. Domains werden über physische UUIDs und HopIDs
zugeordnet, nicht über lokale Enumerationsnummern.

PREPARE überprüft QP, PSN, PD, rkey, Rechte und Bereich, verbraucht die passende
Receive-WQE genau einmal und armiert RX. Erst danach startet TX. Ein Token pro
WR und unveränderliche Metadaten machen wiederholte Kontrollanfragen idempotent.
Boot-Identitäten und ein Replay-Wasserzeichen verhindern alte PREPAREs nach
einem neuen WR. Ein geänderter Remote-Bootstand auf noch bestehenden Ringen
wird abgewiesen; beide Verbindungen müssen neu erzeugt werden.

Eine erfolgreiche SEND-Completion setzt lokale Deskriptor-Completions und
bestätigte Platzierung aller Batches beim Peer voraus. Die SQ-Reihenfolge und
signaled/unsignaled-Verarbeitung verwenden die bestehende Verbs-Queue. IMM und
Receive-CQ erscheinen einmal für den gesamten WR. RNR wird über PREPARE und die
vorhandenen QP-Retry-Einstellungen behandelt. Plain WRITE verbraucht keine
Receive-WQE; der Nutzer muss die Zielpufferlebensdauer selbst koordinieren.

**Die bisherige Wiederholung gerahmter Verbs-Nutzdaten ist noch nicht auf rohe
GPU-Frames übertragbar.** Fehlende, abgeschnittene oder fehlerhafte Frames werden
als Fehler abgeschlossen; Nutzdaten werden nicht blind erneut gesendet.
Ein letzter Marker je Rail erkennt verschobene Frames nach einem Verlust.
Bei einem Datenfehler oder Abbruch nach dem Arming werden beide Richtungen der
betroffenen Rails gestoppt. Beide Testmodule bzw. physischen Verbindungen müssen
danach neu geladen werden. Saubere, vollständig abgeschlossene WRs und saubere
QP-Wechsel verbrauchen die Rails dagegen nicht.

Weitere offene Qualifikation: physisches Hot-Unplug, GPU-Reset,
IOMMU-Fehler, parallele QPs und dauerhafte Serving-Last. Der Modus ist derzeit
für vertraute native Testpeers vorgesehen.

### Verlust und Wiederverbindung (Stand 18. Oktober)

Das Modulparameter-Werkzeug `dmabuf_fault` (nur Test, standardmäßig leer; kein
debugfs, weil Lockdown beschreibbare debugfs-Dateien sperrt) verwirft,
verzögert, dupliziert oder lässt scheitern die nächsten Meldungen eines Typs,
die dieser Host sendet:
`echo "commit delay ms=50 count=1 skip=20" > /sys/module/thunderbolt_ibverbs/parameters/dmabuf_fault`.
Typen: `credit cancel commit release done`; Aktionen: `drop fail delay dup`;
`clear` löscht alle Regeln, Lesen zeigt Regeln und Treffer. `peers` zeigt je
Rail `gpu_stopped=1` für stillgelegte Rails, und der Kernel meldet das Stilllegen.

Ohne Neuladen überstehen: verlorenes, verzögertes oder doppeltes DONE (STATUS),
verlorenes, verzögertes oder doppeltes CREDIT (PREPARE übernimmt, CANCEL trägt
alle Daten), verlorenes CANCEL (BUSY löst die Freigabe aus), RELEASE bis
300 ms, COMMIT bis mindestens 2 s Verzögerung (STATUS antwortet BUSY, solange
ein passender Credit auf sein COMMIT wartet), QP-Zerstörung, Prozess-Kill und
Neuladen des Peer-Moduls bei armiertem Credit. Stillgelegt werden die Rails
beider Seiten bei: verlorenem COMMIT, verlorenem RELEASE oder RELEASE nach über
500 ms, CREDIT und CANCEL gemeinsam verloren, fehlgeschlagenem Senden einer
CREDIT-, CANCEL-, COMMIT- oder RELEASE-Meldung, doppeltem COMMIT oder RELEASE,
Prozess-Kill während einer Übertragung und Abbau eines Links. Danach müssen
beide Module neu geladen werden; ein erneutes Binden der Dienste genügt nicht.
Offene WRs der Gegenseite können dabei 30 s und länger ohne Fehler-CQE
bleiben. Einzelheiten: `bench/results/2026-10-18-verbs-loss.md`.

## Build und Test

`tools/build-gpu-verbs.sh` baut `userspace/bench/tbv_gpu_verbs.hip` gegen eine
vorhandene HIP- und Verbs-Installation. Für getrennte SDKs sind `HIPCC`,
`HIP_PATH`, `HIP_DEVICE_LIB_PATH`, `VERBS_INCLUDE` und `VERBS_LIB` vorgesehen.
Die Protokollprobe `tools/ci/verbs-dmabuf-smoke.c` ist in den bestehenden
Nix-/Distributionsprüfungen enthalten.

In einem abgestimmten Wartungsfenster auf **beiden** Hosts den geladenen
Modulstand, Build-ID und Parameter sichern, Gerätebenutzer beenden und das
Modul zuerst auf **beiden** Hosts entladen. Erst danach die signierten Testmodule
laden. Ein gleichzeitiges Entladen/Laden je Host reicht nicht: ein neuer Host
kann noch die alte Peer-Identität lernen und anschließend die neue Generation
abweisen.

```sh
sudo insmod kernel/thunderbolt_ibverbs.ko \
  profile=linux_perf tbnet=prefer_rdma lanes=2 \
  bind_services=1 allocate_rings=1 start_rings=1 \
  negotiate_native=1 enable_tunnels=1 register_verbs=1 \
  dmabuf_verbs=1

# Rank 0, danach Rank 1; Management-IP einsetzen:
tbv_gpu_verbs --device usb4_rdma0 --gid-index 1 --iterations 8 --rounds 2 --remote-error
tbv_gpu_verbs --device usb4_rdma0 --host MANAGEMENT_IP \
  --gid-index 1 --iterations 8 --rounds 2 --remote-error
```

Der Test prüft alle vier Operationen, acht ungültige Imports, einen gültigen
Teilbereich, FD-Offset 4096, von GPU-VAs unabhängige IOVAs, fremde PDs,
unausgerichtete SGEs und GPU-Längen, drei ausstehende Fenster, unsignaled WRs mit
späterer
signaled Completion, QP-/MR-Neuerzeugung, Byte-Längen und IMM-Reihenfolge.
Nach GPU-Vorlesen vor RX wird jedes Nutzdatenbyte und ein 64-Byte-Wächter geprüft.
Nur ein 16-Byte-Prüfergebnis wird zum Host übertragen. `--remote-error` prüft
einen falschen rkey mit Fehler-CQ auch bei einem unsignaled WR; `--rnr-delay-ms 100`
verzögert die erste Receive-Bereitschaft auf Rank 0.

`--host-mr` prüft denselben Ablauf mit Host-MRs und HIP-Kopien und dient als
Kontrolle des alten Modus bzw. des neuen Staging-Pfads. Gemeldete Zeiten umfassen
GPU-Erzeugung, Vorlesen, Transfer und vollständige Prüfung bei Tiefe 3; sie sind
keine reine DMA-Zeit. `--bytes N --op WRITE_IMM` wählt eine einzelne Größe und
Operation aus; beide Peers müssen dieselbe Auswahl verwenden.

`--depth 1|2|3` wählt die Anzahl der ausstehenden Fenster. `--persistent-rq`
hält mit `--op WRITE_IMM` vier generische Zero-SGE-Receive-WQEs vor und ergänzt sie beim Abholen der
CQ, wie Gufos Verbs-Adapter. Mit `--op SEND` oder `--op SEND_IMM` sind die
Fenster selbst die Receive-WQEs: Nach der vollständigen Prüfung und dem
Vorwärmen des nächsten Durchlaufs wird jedes Fenster erneut gepostet. Dieses
Posten gibt das Fenster zurück; eine Barriere je Nachricht entfällt. Die
Option ist ohne Fehler- oder RNR-Verzögerungsproben zu verwenden. Die Nachricht wird weiterhin
an IMM, Länge, Reihenfolge, Nutzdaten und Wächterbytes geprüft. Eine vorzeitig
eingetroffene nächste Barriere wird bis zur nächsten Iteration vorgemerkt.
Die Barrieren halten die DMA-Platzierung weiterhin hinter der Freigabe des
GPU-Konsumentenfensters.

`--spin-us 5000` prüft einen CQ-Abschluss zunächst aktiv und schläft erst danach
50 µs zwischen leeren Abfragen. Gufo verwendet ebenfalls ein anfängliches
5-ms-Spinfenster, dort für den gesamten Austausch; der Test setzt es pro
CQ-Warteaufruf. Standard ist `--spin-us 0`. `--late-receives` reproduziert die
frühere späte Receive-Bereitstellung und kann nicht mit `--persistent-rq`
kombiniert werden.

Zusätzliche `PHASE`-Zeilen teilen die gemeldete Gesamtdauer in acht aufeinander
folgende CPU-Zeitabschnitte: frühes RQ-Posting, GPU-Aufträge einstellen,
TX-System-Release, Bereitschaft/Barriere, Daten-WRs einstellen, CQs abholen,
RX-Kopie/Release und vollständige GPU-Prüfung. Die Werte sind je Nachricht
normalisiert. `tx_release_us` enthält auch noch laufende Produzenten-Kernels;
`gpu_submit_us` enthält im Host-MR-Modus möglicherweise synchron ausgeführte
HIP-Kopierarbeit. Es sind keine GPU-Event-Laufzeiten einzelner Kernels.

## Gufo und weitere Optimierung

Detaillierte, standardmäßig deaktivierte WR-/Batch-/Rail-Tracepoints und
optionale GPU-Event-Messungen im Test sind in
[TRANSPORT_PROFILING.md](TRANSPORT_PROFILING.md) beschrieben. Der Capture-Runner
erfasst abgeschlossene Diagnosefälle in einer eigenen Tracefs-Instanz; der
Report liefert Verteilungen, eine Timeline und Verlust-/Fallback-Prüfungen.
`TBV_PROFILING=0` bzw. das Nix-Paket `thunderbolt-ibverbs-no-profiling` entfernt
die neue Detailinstrumentierung aus dem Modul.

Gufos Qwen3.8-Flash-Next-TP2-All-Reduce überträgt `rows * 2560 * sizeof(float)`.
Der bestehende Verbs-Adapter schreibt zusätzlich 64 Byte Header am Beginn eines
32-MiB-Fensters. Sowohl die gesamte WR-Länge als auch beliebige Zeilenzahlen
erfüllen damit die 32-Byte-Regel; die drei Fenster beginnen auf Seitengrenzen.
Der frühere direkte ioctl-Adapter rundet die Übertragung sogar auf 4096 Byte auf.
Die Längen begrenzen diesen Gufo-Pfad daher nicht. Sein Verbs-Adapter verwendet
noch Host-MRs; HIP-Allokation, DMA-BUF-Registrierung und GPU-gerechte Headerzugriffe
müssen dort zusätzlich integriert werden.

`dmabuf_profile=1` erfasst Aufrufzahl, gesamte Nanosekunden und Maximum je Phase
in der Debugfs-`summary`: Queue, Job, TX-/RX-Deskriptorbau, PREPARE, TX-Posting,
TX-Warten, STATUS, Freigabe, Verbs-Abschluss sowie getrennte PREPARE-, RNR- und
STATUS-Wartepausen sowie `done_wait` bei aktiv gesendetem DONE. Die Werte sind kumulativ seit dem
Laden des Moduls; Queue und Job bzw. RX-Reaping und RX-Freigabe sind überlappende
Messungen und dürfen nicht zu einer Gesamtdauer addiert werden. Profiling ist
standardmäßig ausgeschaltet.

`dmabuf_index=1` indiziert die DMA-Adressen einmal pro MR und tatsächlichem
NHI-Gerät. Die Einträge erhalten SG-Grenzen und die zulässige Länge jedes Frames.
`dmabuf_index=0` verwendet für A/B-Messungen die bisherige Listensuche. Die
MR-Referenzen halten sowohl Attachments als auch Index bis zum Descriptor-Drain.

`dmabuf_rx_eager=1` plant beim letzten RX-DMA-Callback einen separaten Worker,
der den Batch aufräumt und die Receive-CQ freigibt. Er arbeitet außerhalb des
Ring-Callbacks und unter derselben RX-Sperre wie STATUS. Die Quelle bestätigt
die Platzierung im bisherigen Steuerpfad über STATUS. Der Kontrollring sendet
DONE; auch der XDomain-Push-Modus meldet die Platzierung aktiv. Beide schließen
leere WRs bereits bei PREPARE ab; diese haben keinen DMA-Callback.
`dmabuf_rx_eager=0` erlaubt den Vergleich mit dem bisherigen STATUS-Reaping.

`dmabuf_parallel_post=1` übergibt Batches ab 128 Frames parallel an verschiedene
tatsächliche NHI-Geräte. Rails desselben NHIs bleiben innerhalb einer Gruppe
geordnet; der Worker wechselt nach jeweils höchstens 16 Deskriptoren zur
nächsten Rail, damit ihre DMA früher überlappt. Eine eigene ungebundene Workqueue ermöglicht die parallele Arbeit;
der Aufrufer wartet auf alle Gruppen, auch bei Fehlern, bevor Batch-Metadaten
freigegeben werden. Kleine Batches verwenden weiterhin das sequenzielle
Posting. `dmabuf_parallel_post=0` schaltet diesen Vergleichsmodus aus.
Index, eager RX und paralleles Posting sind im experimentellen Modus
standardmäßig eingeschaltet; Profiling ist ausgeschaltet.

Der NHI-Steuerkanal hat die vermutete große Einsparung gegenüber XDomain in
dieser seriellen Arbeitsweise nicht bestätigt. XDomain-DONE sparte im
Hardwarevergleich normale STATUS-Abfragen unter Erhalt aller Datenrails;
sein Latenzvorteil blieb auf kleine Transfers beschränkt. Der gestörte
Interruptzustand nach Link-Abbau benötigt eine eigene Untersuchung.
Weitere Möglichkeiten sind weniger Workerwechsel und das gebündelte Einreihen von
Deskriptoren. Vorbereitete RX-Fenster mit Credits und überlappende Batches
bleiben weitere Möglichkeiten. Die Phasenmessung zeigt bei großen WRs
insbesondere Kosten für TX-/RX-Posting und DMA; bei kleinen WRs fallen
Steuerung und Synchronisation ins Gewicht.

Auf den APUs kann weniger Kopierarbeit Speicherbandbreite für die Modell-Kernels
freigeben. Ein Nutzen für Gufo muss unter Modelllast mit Prefill-Zeit und Tokens
pro Sekunde geprüft werden; die Transportdiagnose allein misst ihn nicht.

Die Laborskripte `tools/lab-verbs-dmabuf-module.sh` und
`tools/lab-gpu-verbs-run.sh` bereiten reversible Modulwechsel und die vorhandene
Nix-Laufzeit im isolierten Testverzeichnis vor. Sie ersetzen kein abgestimmtes
Wartungsfenster und installieren weder Module noch Bibliotheken systemweit.
