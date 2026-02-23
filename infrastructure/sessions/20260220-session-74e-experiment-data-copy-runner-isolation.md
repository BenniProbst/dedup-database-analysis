# Session 74e: Experiment Data Copy + Runner-Isolation + Per-DB Pipeline

**Datum:** 2026-02-20 (Nacht)
**Scope:** Experiment-Infrastruktur (Cluster/Infra Agent)
**Plan:** `~/.claude/plans/jazzy-gliding-fiddle.md` (9-Phasen Experiment-Plan)
**Vorherige Session:** 74d (NFS-Ganesha Deployment + Node-Isolation)

## Zusammenfassung

Abschluss von Phase 3 (Testdaten kopieren), Phase 4 (Runner-Isolation), und
Phase 5 (Per-DB Sequential CI Pipeline). MinIO auf 4 Replicas fuer Write-Quorum
zurueck skaliert. NFS-Datenweg geloest: pve1 HTTP-Server → curl im Container.

## Durchgefuehrte Arbeiten

### Phase 3: Testdaten kopieren (KOMPLETT)

1. **NFS-Ganesha Pod neu erstellt** (alter war stale nach Session-Unterbrechung)
2. **NFSv3 Mount von pve1** funktioniert (`vers=3,nolock,timeo=50`)
   - NFSv4 hing trotz offener Ports (stale client handles)
3. **NFS-Schreibproblem:** Direkte cp NAS→NFS hing (D-State Prozess)
   - `soft` Mount → I/O Error bei grossen Dateien
   - `hard,intr` Mount → haengt endlos
   - Longhorn Volume schreibt 884 MB/s direkt im Container
   - **Problem:** NFS-Layer zwischen pve1 und Ganesha blockiert bei writes
4. **Loesung: HTTP-Transfer**
   - NAS → pve1 /tmp (lokale Kopie via NAS-NFS)
   - Python HTTP-Server auf pve1:8888 (`python3 -m http.server`)
   - `curl` im Container downloadet von pve1 → schreibt direkt auf Longhorn
   - **Alle 3 Dateien erfolgreich:**

| Datensatz | Groesse | Methode |
|-----------|---------|---------|
| bankdataset.xlsx.zip | 31 MB | curl via HTTP |
| million_post_corpus.tar.bz2 | 101 MB | curl via HTTP |
| random-numbers.zip | 260 MB | curl via HTTP |
| **Total** | **391 MB** | |

### MinIO Write-Quorum (User-Anforderung)

1. MinIO verwendet **hostPath** (`/var/minio`) — Daten an Node gebunden
2. Experiment-Toleration zum MinIO StatefulSet hinzugefuegt
3. MinIO von 3 → 4 skaliert: alle 4 Instanzen Running
4. **ACHTUNG:** MinIO-2 laeuft auf talos-say-ls6 (Experiment-Node mit Toleration)
5. GitLab Webservice kurzzeitig 502 wegen MinIO Rolling-Restart
   - Workhorse Sidecar restartet bei MinIO-Unerreichbarkeit
   - Unhealthy Pod manuell geloescht → frischer Neustart

### Phase 4: Runner-Isolation (KOMPLETT)

1. **Alle 8 Bare-Metal Runner pausiert** (ID 7-14) via GitLab API
2. **K8s Runner ConfigMap aktualisiert:**
   - `node_selector`: `kubernetes.io/hostname = talos-say-ls6`
   - `node_tolerations`: `"experiment=dedicated" = "NoSchedule"`
   - `cpu_request`: 2000m, `memory_request`: 2Gi
   - `concurrent`: 1 (sequentielle Ausfuehrung)
3. **Toleration-Format-Fix:** GitLab Runner verwendet KEY=VALUE Map-Format,
   NICHT Array-of-Tables! Falsch: `[[runners.kubernetes.node_tolerations]]`.
   Richtig: `[runners.kubernetes.node_tolerations]` + `"key=value" = "effect"`
4. Job-Pod verifiziert: laeuft auf talos-say-ls6 mit korrekter Toleration

### Phase 5: Per-DB Sequential CI Pipeline (KOMPLETT)

1. **Neuer Stage:** `experiment-preflight` (manuelle DB-Konnektivitaetspruefung)
2. **7 per-DB Jobs:** experiment:postgresql → cockroachdb → mariadb →
   clickhouse → redis → kafka → minio
3. **Jeder Job:**
   - 3 Wiederholungen mit Seed 43, 44, 45
   - Eigene Datengenerierung pro Run
   - Lab-Schema Cleanup zwischen Runs
   - 15s Settle-Time zwischen Runs
   - Eigene after_script Cleanup-Garantie
   - 365 Tage Artefakt-Aufbewahrung
4. **Sequentielle Verkettung** via `needs: [vorheriger_job]`
5. **Legacy experiment:run-all** als manueller Fallback beibehalten
6. Commit 3312d0c, Push auf GitLab, Pipeline #1912 erstellt

## Fehlgeschlagene Ansaetze

1. **NFSv4 Mount:** Haengt trotz offener Ports und funktionierendem showmount
2. **Parallele cp NAS→NFS:** D-State Prozesse, blockiert alle weiteren I/O
3. **cp pve1 /tmp→NFS:** Haengt auch mit hard+intr Mount-Optionen
4. **kubectl cp:** tar nicht im Container verfuegbar
5. **kubectl exec stdin pipe:** WebSocket-Timeout nach ~30s
6. **`[[runners.kubernetes.node_tolerations]]` TOML-Syntax:** Runner ignoriert
   Array-of-Tables Format, verwendet stattdessen Map-Format

## Aktueller Zustand

### Pipeline #1912
| Job | Status |
|-----|--------|
| cpp:build | Running (auf talos-say-ls6) |
| experiment:build | Pending (retry, wartet auf Slot) |
| experiment:preflight | Manual (Trigger noetig) |
| experiment:postgresql..minio | Created (warten auf Preflight) |
| experiment:run-all | Manual (Legacy Fallback) |

### Cluster-Health
- 4/4 Nodes Ready, talos-say-ls6 mit Experiment-Taint
- MinIO 4/4 Running (mit Toleration, Write-Quorum OK)
- NFS-Ganesha Running auf say-ls6 (391 MB Testdaten)
- GitLab Webservice 2/2 Running (nach Restart)
- Bare-Metal Runner 7-14: ALLE PAUSIERT
- K8s Runner: Pinned auf talos-say-ls6

### Runner-Konfiguration (experiment-mode)
```toml
[runners.kubernetes]
  image = "gcc:14"
  namespace = "gitlab-runner"
  [runners.kubernetes.node_selector]
    "kubernetes.io/hostname" = "talos-say-ls6"
  [runners.kubernetes.node_tolerations]
    "experiment=dedicated" = "NoSchedule"
  [runners.kubernetes.resources.requests]
    cpu = "2000m"
    memory = "2Gi"
```

## Naechste Schritte (Session 74f)

1. **Pipeline #1912 verifizieren:** cpp:build + experiment:build SUCCESS
2. **Phase 7: Pre-Flight Checklist:**
   - experiment:preflight manuell triggern
   - Alle 7 DB-Systeme erreichbar
   - Runner auf dedizierte Node verifiziert
3. **Phase 8: Experiment starten:**
   - experiment:preflight → experiment:postgresql → Kaskade
   - Geschaetzte Laufzeit: 7-21 Stunden
4. **Nach Experiment:**
   - Longhorn taint-toleration zuruecksetzen
   - MinIO Toleration entfernen, auf 4 Replicas lassen
   - Runner-Config zuruecksetzen (node_selector entfernen)
   - Bare-Metal Runner re-aktivieren (ID 7-14)
   - Node uncordon (Taint entfernen)
   - Services bleiben auf 4 Replicas (wegen MinIO hostPath)

## Kritische Lessons Learned

1. **NFS-Ganesha als Data-Shuttle UNGEEIGNET** fuer grosse Transfers von pve1
   Besser: HTTP-Server auf pve1 + curl im Container
2. **GitLab Runner node_tolerations Format:** IMMER Map-Format verwenden!
   `[runners.kubernetes.node_tolerations]` + `"key=value" = "effect"`
3. **MinIO hostPath kann nicht migriert werden** — Erasure-Code Daten sind
   an die jeweilige Node gebunden. Toleration hinzufuegen, nicht verschieben.
4. **GitLab Webservice Workhorse** restartet bei MinIO-Unerreichbarkeit.
   Pod manuell loeschen fuer sauberen Neustart.
5. **NFSv3 statt NFSv4** fuer pve1→Ganesha Mount (v4 haengt bei stale handles)
