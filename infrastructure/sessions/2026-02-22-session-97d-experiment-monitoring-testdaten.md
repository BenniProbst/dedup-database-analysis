# Session 97d: Experiment-Monitoring + Testdaten-Vorbereitung
**Datum:** 2026-02-22 (Abend)
**Agent:** Infrastruktur (system32 Kontext)
**Projekt:** dedup-database-analysis (GitLab Project 280)
**Vorgaenger:** Session 97c (Bug-Fixes + Pipeline #6087 Rerun)

## Zusammenfassung

Pipeline #6087 laeuft erfolgreich: experiment:postgresql SUCCESS (216min/3.6h), experiment:cockroachdb RUNNING.
Bug-Fixes aus S97c verifiziert (bulk_insert OK, Kafka Backoff OK, NFS Volume OK).
Testdaten-Lueckenanalyse durchgefuehrt: 3 NAS-Archive auf K8s NFS entpackt, doku.tex Anforderungen analysiert.

---

## 1. Pipeline #6087 Monitoring

### experiment:postgresql — SUCCESS (12.975s / 216min)
- **Bug 1 Fix VERIFIZIERT:** `Bulk insert complete: 500 rows, 262127814 bytes` — Row Count > 0!
- **Bug 3 Fix VERIFIZIERT:** `Kafka still failing (3203 suppressed)` statt tausender Error-Zeilen — Backoff funktioniert korrekt
- **Bug 2 Fix VERIFIZIERT:** NFS Volume unter `/datasets/real-world` erreichbar (K8s Volume)
- **Kafka-Fehler:** `Broker: Topic authorization failed` (ACL fehlt, kein Code-Bug, Backoff unterdrueckt korrekt)
- **Longhorn Metriken:** Volume `pvc-5006db3b-e14c-4daf-9ccb-b9f9968c724a` korrekt gemessen

### Payload-Typ Ergebnisse (Run 1-3 × 8 Typen × 3 Grades × 4 Ops)

| Payload-Typ | Rows/Test | Bytes/Test | Status |
|-------------|-----------|------------|--------|
| random_binary | 500 | 262.144.000 | OK |
| structured_json | 500 | 262.134.908 | OK |
| text_document | 500 | 262.145.865 | OK |
| uuid_keys | 500 | 262.145.000 | OK |
| jsonb_documents | 500 | 262.127.814 | OK |
| bank_transactions | 0 | 0 | NFS-Daten fehlen |
| text_corpus | 0 | 0 | NFS-Daten fehlen |
| numeric_dataset | 0 | 0 | NFS-Daten fehlen |

**5 von 8 Payload-Typen liefern gueltige Ergebnisse, 3 sind leer (NAS-Daten nicht auf NFS kopiert).**

### Runner-Stall-Bug
- Runner-4 wurde nach experiment:postgresql neugestartet (Vorsichtsmassnahme)
- CockroachDB wurde AUTOMATISCH aufgenommen (Stall trat hier NICHT auf)
- Moeglicher Grund: Pipeline-Kaskade mit `needs:` triggert Jobs anders als manuelles Warten

### Pipeline-Status bei Session-Ende
| Job | Status | Dauer |
|-----|--------|-------|
| experiment:build | SUCCESS | 117s |
| experiment:preflight | SUCCESS | 21s |
| experiment:postgresql | **SUCCESS** | **12.975s (216min)** |
| experiment:cockroachdb | RUNNING | ~23min (laeuft) |
| experiment:mariadb | CREATED | — |
| experiment:clickhouse | CREATED | — |
| experiment:redis | CREATED | — |
| experiment:kafka | CREATED | — |
| experiment:minio | CREATED | — |
| experiment:upload-results | CREATED | — |

---

## 2. Testdaten-Lueckenanalyse (doku.tex vs. Code)

### doku.tex Anforderungen (Kapitel 5.3 "Data sets and payload types")

Die Forschungsarbeit definiert folgende Payload-Typen:

**Synthetisch (im Code generiert):**
- Random binary, Structured JSON, Text/VARCHAR, UUID/GUID, JSON/JSONB

**NAS-Daten (Cluster_NFS vorhanden, NFS-Volume fehlte):**
- Bank-Transaktionsdaten (`bankdataset.xlsx.zip`, 32 MB)
- Million Post Corpus (`million_post_corpus.tar.bz2`, 105 MB)
- Synthetische Zufallszahlen (`random-numbers.zip`, 271 MB)

**NAS-Daten (vorhanden, nicht im Code implementiert):**
- GitHub Archive Events (`gharchive/`, 53.472 .json.gz Dateien, ~17 GB/500 Dateien)
- GitHub BigQuery Repos (`github-repos-bigquery/`, 8 Unterordner)

**Zu downloaden (nicht vorhanden):**
- NASA Imagery (Hubble Ultra Deep Field .tif, NASA Image Library)
- Blender Video (Big Buck Bunny, Sintel, Tears of Steel)
- Gutenberg Text (Pride and Prejudice, Moby-Dick etc.)

### Luecken-Matrix

| Payload-Typ | doku.tex | Code | NAS | K8s NFS | Status |
|-------------|----------|------|-----|---------|--------|
| random_binary | Ja | Ja | — | — | **OK (synthetisch)** |
| structured_json | Ja | Ja | — | — | **OK (synthetisch)** |
| text_document | Ja | Ja | — | — | **OK (synthetisch)** |
| uuid_keys | Ja | Ja | — | — | **OK (synthetisch)** |
| jsonb_documents | Ja | Ja | — | — | **OK (synthetisch)** |
| bank_transactions | Ja | Ja | Ja | **JETZT JA** | **GEFIXT** |
| text_corpus (Million Post) | Ja | Ja | Ja | **JETZT JA** | **GEFIXT** |
| numeric_dataset | Ja | Ja | Ja | **JETZT JA** | **GEFIXT** |
| github_events | Ja | NEIN | Ja (53k) | NEIN | **TODO: Code + Sample** |
| nasa_image | Ja | NEIN | NEIN | NEIN | **TODO: Download + Code** |
| blender_video | Ja | (ausgeschl.) | NEIN | NEIN | **TODO: Download + Code** |
| gutenberg_text | Ja | NEIN | NEIN | NEIN | **TODO: Download + Code** |

---

## 3. Testdaten auf K8s NFS kopiert

### Root Cause: NFS-Volume war leer
Die NAS→NFS Kopie aus Session 74d wurde nie abgeschlossen. Das Volume `/export/real-world/` auf dem NFS-Ganesha Pod war leer.

### NAS-Erreichbarkeit
- NAS (192.168.178.31) ist von pve1 NICHT erreichbar (100% Paketverlust, VLAN-Routing)
- NAS ist von Windows-PC erreichbar (VLAN 1, 2ms Ping)
- **Transferpfad:** Windows → SCP → pve1 → NFS Mount

### NFS-Mount Loesung
- **NFS von pve1 NICHT ueber VLAN 110 (10.0.110.184) erreichbar** (Timeout)
- **NFS von pve1 ueber VLAN 120 (10.0.120.184) ERREICHBAR!**
- Mount: `mount -t nfs4 -o timeo=50,retrans=3 10.0.120.184:/ /tmp/experiment-nfs`
- **MERKEN:** NFS-Ganesha auf talos-say-ls6 ist ueber VLAN 120 IP erreichbar, NICHT ueber VLAN 110!

### Kopierte und entpackte Dateien

| Datensatz | Archiv | Entpackt | Format | Groesse |
|-----------|--------|----------|--------|---------|
| bankdataset | bankdataset.xlsx.zip | bankdataset.xlsx | Excel .xlsx | 32 MB |
| million_post | million_post_corpus.tar.bz2 | corpus.sqlite3 | SQLite3 | 340 MB |
| random_numbers | random-numbers.zip | random_numbers.csv | CSV | 561 MB |

### NFS Verzeichnisstruktur
```
/export/real-world/
  archives/             # Originalarchive (Backup auf NFS)
    bankdataset.xlsx.zip
    million_post_corpus.tar.bz2
    random-numbers.zip
  bankdataset/           # Entpackt: bankdataset.xlsx (32 MB)
  million_post/          # Entpackt: million_post_corpus/corpus.sqlite3 (340 MB)
  random_numbers/        # Entpackt: random_numbers.csv (561 MB)
  gharchive/             # LEER (Stichproben TODO)
  checkpoints/           # Fuer Experiment-Checkpoints
```

### Cluster_NFS Backup-Struktur angelegt
```
\\BENJAMINHAUPT\Cluster_NFS\experiment-datasets\
  bankdataset/           # bankdataset.xlsx.zip
  million_post/          # million_post_corpus.tar.bz2
  random_numbers/        # random-numbers.zip
  gharchive-sample/      # LEER (TODO)
  nasa_images/           # LEER (TODO: Download)
  gutenberg_text/        # LEER (TODO: Download)
  blender_video/         # LEER (TODO: Download)
```

---

## 4. Data-Copier Helper Pod

### Problem: kubectl cp braucht tar
- NFS-Provisioner Image hat kein `tar` → `kubectl cp` scheitert
- **Loesung:** Alpine Helper Pod `data-copier` mit PVC Mount erstellt

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: data-copier
  namespace: dedup-experiment
spec:
  nodeName: talos-say-ls6
  tolerations:
  - key: experiment
    operator: Equal
    value: dedicated
    effect: NoSchedule
  containers:
  - name: copier
    image: alpine:3.19
    command: ["sleep", "infinity"]
    volumeMounts:
    - name: nfs-data
      mountPath: /nfs
  volumes:
  - name: nfs-data
    persistentVolumeClaim:
      claimName: experiment-data
```

- Tools installiert: `unzip`, `bzip2`, `tar`
- Pod laeuft auf talos-say-ls6 (Experiment-Node)
- **WARNUNG:** K8s API war intermittierend instabil (Timeouts bei kubectl exec/cp waehrend Experiment laeuft)
- **Workaround:** NFS direkt von pve1 ueber VLAN 120 gemountet statt kubectl-Operationen

---

## 5. gharchive Stichprobe (ABGEBROCHEN)

- 500 GH Archive Dateien = ~17 GB (zu gross!)
- Jede Datei ist ein stuendlicher GitHub Events Dump (.json.gz)
- **TODO naechste Session:** Weniger Dateien waehlen (24 Stueck = 1 Tag)
- **User-Direktive:** "Gezielt einige Aspekte auswaehlen, nicht alles kopieren"
- **User-Direktive:** "Datenmengen ueber 1GB/5GB/10GB auflockern"

---

## 6. C++ Data Generator Analyse

### Root Cause der 0-Byte-Dateien

**Datei:** `src/cpp/experiment/dataset_generator.cpp`

Die 3 NAS-abhaengigen Payload-Typen laden Daten via `load_local_directory_file()`:
```cpp
std::vector<char> DatasetGenerator::load_bank_transactions() {
    return load_local_directory_file("bankdataset");
}
std::vector<char> DatasetGenerator::load_text_corpus() {
    return load_local_directory_file("million_post");
}
std::vector<char> DatasetGenerator::load_numeric_dataset() {
    return load_local_directory_file("random_numbers");
}
```

Wenn das Verzeichnis nicht existiert → leerer Vector → 0-Byte-Dateien.

**ABER:** Die Daten sind jetzt auf dem NFS! Allerdings muss der `real_world_dir` Pfad stimmen (`/datasets/real-world` im Pod). Die Unterordner muessen `bankdataset/`, `million_post/`, `random_numbers/` heissen und Dateien enthalten.

**Problem:** Der Code erwartet Verzeichnisse mit VIELEN kleinen Dateien, nicht eine einzelne grosse Datei (Excel/CSV/SQLite). Der DatasetGenerator:
- Liest ALLE Dateien im Verzeichnis
- Waehlt per Index eine zufaellige Datei
- Gibt den Dateiinhalt als Payload zurueck

**Das heisst:** Die Excel/CSV/SQLite Dateien muessen erst in viele kleine Einzeldateien aufgeteilt werden (z.B. 500 Dateien pro Grade), damit der Generator sie als Payloads verwenden kann!

### Naechste Schritte fuer Datenvorbereitung
1. **bankdataset.xlsx:** Excel in 500 CSV/JSON-Chunks aufteilen (Python im data-copier Pod)
2. **corpus.sqlite3:** SQL-Queries → 500 Text-Dateien extrahieren
3. **random_numbers.csv:** CSV in 500 Abschnitte splitten
4. **gharchive:** 24 .json.gz Dateien auswaehlen, jeweils in ~500 JSON-Einzelobjekte aufteilen

---

## 7. User-Direktiven (NEU)

| # | Direktive | Quelle |
|---|-----------|--------|
| D1 | Cluster_NFS NUR als Speicher, NICHT zum Entpacken (zu langsam) | User S97d |
| D2 | Dateioperationen NUR auf K8s Experiment-NFS | User S97d |
| D3 | pve1 nur als Transit, Dateien dort nicht dauerhaft lagern | User S97d |
| D4 | Alle Testdaten auch auf Cluster_NFS sichern (Backup) | User S97d |
| D5 | Datenmengen-Anforderungen auflockern (nicht alles 1GB/5GB/10GB) | User S97d |
| D6 | Gezielt Aspekte auswaehlen, nicht alles kopieren | User S97d |
| D7 | Fehlende Daten (NASA etc.) aus Web downloaden → Cluster_NFS → K8s NFS | User S97d |
| D8 | gharchive und github-repos-bigquery UNERSETZBAR (800 EUR) | User S97d |
| D9 | Gesamte Testdaten spaeter fuer comdare-db Tests | User S97d |
| D10 | IMMER merge statt reset bei Git-Konflikten | User S97c |

---

## 8. Technische Erkenntnisse

### NFS-Routing
- NFS-Ganesha auf talos-say-ls6 hat hostNetwork IP 10.0.110.184 (VLAN 110)
- Von pve1 (10.0.10.201) ist VLAN 110 IP NICHT erreichbar (Mount Timeout)
- **VLAN 120 IP (10.0.120.184) FUNKTIONIERT!** ← MERKEN!
- `mount -t nfs4 -o timeo=50,retrans=3 10.0.120.184:/ /tmp/experiment-nfs`

### K8s API Stabilitaet unter Last
- Waehrend Experiment-Jobs laufen: kubectl exec/cp Timeouts haeufig
- `i/o timeout` auf `10.0.15.201:59408->10.0.15.250:6443` (VLAN 15 API)
- Kurze Befehle (mkdir, ls) funktionieren meistens
- Lange Dateitransfers (kubectl cp 32MB+) scheitern
- **Workaround:** NFS direkt mounten statt kubectl cp

### Datenformat-Problem
- C++ DatasetGenerator erwartet Verzeichnisse mit VIELEN kleinen Dateien
- NAS-Daten sind EINZELNE grosse Dateien (Excel 32MB, SQLite 340MB, CSV 561MB)
- **Loesung:** Aufteilen in 500+ Einzeldateien pro Payload-Typ im data-copier Pod

---

## 9. Offene Tasks (Naechste Session)

### Prio 1: Pipeline #6087 weiter ueberwachen
- experiment:cockroachdb laeuft (~23min, erwartete Dauer aehnlich PG)
- Kaskade: CRDB → MariaDB → CH → Redis → Kafka → MinIO
- Runner-4 Restart falls Stall-Bug auftritt
- Upload-Results Job pruefen

### Prio 2: NAS-Daten aufbereiten
1. bankdataset.xlsx → 500 JSON-Chunks (Python im data-copier Pod)
2. corpus.sqlite3 → 500 Text-Dateien extrahieren
3. random_numbers.csv → 500 CSV-Abschnitte
4. gharchive → 24 Dateien auswaehlen, in JSON-Objekte splitten

### Prio 3: Fehlende Testdaten downloaden
1. NASA Imagery: Hubble Ultra Deep Field + NASA Image Library Samples
2. Gutenberg Text: Pride and Prejudice (1342), Moby-Dick (2701), etc.
3. Blender Video: Big Buck Bunny (kleines Format, OOM-sicher)
4. Alles → Cluster_NFS → K8s NFS

### Prio 4: C++ Code erweitern
1. github_events Loader implementieren (gharchive .json.gz Dateien)
2. nasa_image Loader implementieren (NASA .tif/.jpg Dateien)
3. gutenberg_text Loader implementieren (.txt Dateien)
4. PAYLOAD_TYPES in CI um neue Typen erweitern

### Prio 5: Infrastruktur (nach Experiment)
1. PostgreSQL Active/Active NGINX+MetalLB
2. Samba AD LDAP Auth fuer alle 7 DBs
3. Streaming Replication einrichten
4. Runner-Stall-Bug permanent fixen

---

## 10. Aktive Ressourcen

| Ressource | Status | Details |
|-----------|--------|---------|
| Pipeline #6087 | RUNNING | CockroachDB bei ~23min |
| data-copier Pod | RUNNING | dedup-experiment NS, talos-say-ls6 |
| NFS-Server Pod | RUNNING | 10.0.110.184/10.0.120.184 |
| NFS Mount pve1 | AKTIV | /tmp/experiment-nfs (VLAN 120) |
| Monitor-Script pve1 | RUNNING | /tmp/experiment-monitor.sh (PID 3884212) |
| Experiment-Node | talos-say-ls6 | Taint experiment=dedicated:NoSchedule |
| HTTP Server pve1 | RUNNING | Port 8899 (kann gestoppt werden) |

### Aufzuraeumen (naechste Session)
- pve1 /tmp/*.zip, /tmp/*.tar.bz2 → loeschen nach NFS-Kopie bestaetigt
- HTTP Server auf pve1 stoppen: `kill $(lsof -ti:8899)`
- data-copier Pod ggf. loeschen wenn nicht mehr gebraucht
- NFS Unmount: `umount /tmp/experiment-nfs`
