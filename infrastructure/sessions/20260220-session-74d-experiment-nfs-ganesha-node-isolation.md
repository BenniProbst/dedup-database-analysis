# Session 74d: Experiment Node-Isolation + NFS-Ganesha Datenzugriff

**Datum:** 2026-02-20 (Abend)
**Scope:** Experiment-Infrastruktur (Cluster/Infra Agent)
**Plan:** `~/.claude/plans/jazzy-gliding-fiddle.md` (9-Phasen Experiment-Plan)
**Vorherige Session:** 74c (Experiment-Spezifikation + Node-Isolation-Plan)

## Zusammenfassung

Implementierung der Phasen 1-3 des Experiment-Plans: Node-Isolation von talos-say-ls6,
Service-Skalierung auf 3 Replicas, und Aufbau eines NFS-Ganesha Datenzugriffspunkts
auf der dedizierten Experiment-Node. Mehrere Problemloesungen bei kubectl WebSocket-
Timeouts, Longhorn Volume-Attach, und Ganesha-Konfiguration.

## Durchgefuehrte Arbeiten

### Phase 1: Node-Isolation (KOMPLETT)

1. **Services 4 -> 3 skaliert:**
   - PostgreSQL: `kubectl scale sts postgres-ha --replicas=3` -> OK
   - MinIO: `kubectl scale sts minio --replicas=3` -> OK
   - Samba-AD: `kubectl scale sts samba-ad --replicas=3` -> OK
   - Redis-GitLab: `kubectl scale sts redis-gitlab --replicas=3` -> OK
   - CockroachDB: Nicht noetig (keine Pods auf say-ls6, Operator-Bug bei nodes:3)

2. **Node-Isolation:**
   - `kubectl taint node talos-say-ls6 experiment=dedicated:NoSchedule`
   - `kubectl drain talos-say-ls6` (anfangs mit Cordon, spaeter korrigiert)
   - **WICHTIGE LEKTION:** Cordon (`node.kubernetes.io/unschedulable`) blockiert
     auch DaemonSets und Longhorn instance-manager. NUR Taint reicht!
   - Node nach Uncordon: `Ready` mit nur Experiment-Taint

3. **ClickHouse umgezogen:**
   - Hatte `nodeSelector: talos-say-ls6` -> entfernt via `kubectl patch sts`
   - Pod geloescht -> neu auf talos-qkr-yc0 gescheduled

4. **OPNsense-4 gerettet:**
   - User-Direktive: "OPNsense macht Routing, darf NICHT ausfallen!"
   - Tolerations hinzugefuegt fuer Experiment-Taint auf:
     - `deploy/nfs-opnsense-4` (NFS-Ganesha fuer VM-Image)
     - `vm/opnsense-4` (KubeVirt VM)
   - VM neu gestartet -> Running auf talos-say-ls6

5. **GitLab-Runner auf 3 skaliert:**
   - PodAntiAffinity verhinderte 4. Replica auf 3 Nodes
   - `kubectl scale deployment gitlab-runner --replicas=3`

### Phase 2: NFS-Ganesha auf Experiment-Node (KOMPLETT)

1. **Namespace `dedup-experiment`** erstellt mit PSA `privileged`
2. **Longhorn PVC `experiment-data`** (200 Gi, StorageClass `longhorn-experiment`, Replica 1)
3. **NFS-Ganesha Pod** auf talos-say-ls6:
   - Image: `registry.k8s.io/sig-storage/nfs-provisioner:v4.0.8` (hat Ganesha eingebaut)
   - **Ganesha direkt gestartet** (nicht den Provisioner!) mit Custom-Config
   - hostNetwork: true -> IP 10.0.110.184
   - nodeSelector: talos-say-ls6, Tolerations fuer Experiment-Taint
   - VFS FSAL exportiert `/export` als NFSv3+v4

4. **Konfigurationsprobleme geloest:**
   - `ganesha.nfsd` Pfad: `/usr/local/bin/ganesha.nfsd` (nicht `/usr/bin/`)
   - PID-Dir: `mkdir -p /usr/local/var/run/ganesha` noetig
   - FSAL Plugins: `Plugins_Dir = /usr/local/lib64/ganesha` (nicht lib/)
   - CLIENT-Block: Ganesha 4.0.8 erkennt `Clients` nicht im CLIENT-Block -> entfernt
   - Pseudo-Pfad: `Pseudo = /` statt `/data`

5. **NFS Mount von pve1 FUNKTIONIERT:**
   ```bash
   mount -t nfs4 10.0.110.184:/ /mnt/experiment-data
   # showmount: /export (everyone)
   # Read/Write Test: BESTANDEN
   ```

### Phase 3: Testdaten kopieren (IN PROGRESS)

- Verzeichnisstruktur auf NFS erstellt: `testdata/{bankdataset,million_post,random_numbers}`
- Kopiervorgang gestartet: `cp /mnt/nas-cluster-backup/bankdataset.xlsx.zip ...`
- **Abgebrochen** wegen Session-Ende (NAS I/O langsam)

### Longhorn Taint-Toleration

- **Longhorn Setting `taint-toleration` = `experiment=dedicated:NoSchedule`**
  gesetzt, damit instance-manager auf talos-say-ls6 erstellt werden kann
- Ohne diese Einstellung: Volume-Attach schlaegt fehl (kein instance-manager auf Node)
- **MUSS nach Experiment zurueckgesetzt werden!**

## Fehlgeschlagene Ansaetze (Lessons Learned)

1. **kubectl exec/cp tar-pipe:** WebSocket-Timeout nach ~30s bei Datentransfer
   (K8s API VLAN 15, TCP i/o timeout). Auch mit `--request-timeout=300s` nicht fixbar.
2. **kubectl cp direkt:** Selbes WebSocket-Problem
3. **hostNetwork Pod mit apk install:** Alpine kann keine Pakete laden
   (DNS/Internet-Probleme von Talos Node aus)
4. **Debian apt-get install nfs-ganesha:** Extrem langsam (>5 Min ohne Ergebnis)
5. **nfs-provisioner als NFS-Server:** Braucht RBAC (ServiceAccount Permissions),
   exportiert ohne Provisioner-Modus kein Volume

## Aktueller Zustand

### Cluster-Health
| Komponente | Status | Node |
|------------|--------|------|
| talos-say-ls6 | Ready, Taint: experiment=dedicated:NoSchedule | - |
| OPNsense 1-4 | Alle Running | qkr-yc0, lux-kpk, 5x2-s49, say-ls6 |
| PostgreSQL | 3/3 Running | 5x2-s49, lux-kpk, qkr-yc0 |
| MinIO | 3/3 Running | lux-kpk, qkr-yc0, 5x2-s49 |
| Redis-GitLab | 3/3 Running | lux-kpk(x2), qkr-yc0 |
| Samba-AD | 3/3 Running | qkr-yc0, lux-kpk, 5x2-s49 |
| ClickHouse | 1/1 Running | qkr-yc0 (umgezogen) |
| CockroachDB | 4/4 Running | Alle 4 Nodes (unveraendert) |
| GitLab | Alle Running (HTTP 302) | lux-kpk, 5x2-s49, qkr-yc0 |
| GitLab Runner | 3/3 Running | qkr-yc0, 5x2-s49, lux-kpk |
| NFS-Ganesha | 1/1 Running | say-ls6 (Experiment-Node) |
| Longhorn IMs | 4/4 Running | Alle 4 Nodes |
| Kafka Entity Op | CrashLoopBackOff | (bekannt, pre-existing) |

### NFS-Ganesha Konfiguration (FUNKTIONIEREND)
```
Image: registry.k8s.io/sig-storage/nfs-provisioner:v4.0.8
Binary: /usr/local/bin/ganesha.nfsd
Plugins: /usr/local/lib64/ganesha
Config: /tmp/ganesha.conf (inline generiert)
Export: /export (Pseudo = /, VFS FSAL, RW, No_Root_Squash)
Port: 2049 (hostNetwork, IP 10.0.110.184)
PVC: experiment-data (200 Gi, longhorn-experiment, Replica 1)
Mount von pve1: mount -t nfs4 10.0.110.184:/ /mnt/experiment-data
```

### Zu bereinigende Ressourcen
- Namespace `dedup-experiment` enthaelt nur nfs-server + PVC (sauber)
- Alter Staging-Pod in databases namespace -> geloescht
- database-experiment Namespace -> geloescht

## Naechste Schritte (Session 74e)

1. **Testdaten kopieren fortsetzen:**
   - NAS remounten auf pve1: `mount /mnt/nas-cluster-backup`
   - `cp /mnt/nas-cluster-backup/bankdataset.xlsx.zip /mnt/experiment-data/testdata/bankdataset/`
   - `cp /mnt/nas-cluster-backup/million_post_corpus.tar.bz2 /mnt/experiment-data/testdata/million_post/`
   - `cp /mnt/nas-cluster-backup/random-numbers.zip /mnt/experiment-data/testdata/random_numbers/`
   - gharchive: Selektiv kopieren (53k Files, I/O-Fehler bei manchen)
   - Entpacken im Container

2. **Phase 4: Runner-Isolation** (per Plan)
3. **Phase 5: CI Pipeline per-DB Jobs** (per Plan)
4. **Phase 7: Pre-Flight Checklist**
5. **Nach Experiment:** Longhorn taint-toleration zuruecksetzen!

## Kritische Regeln (bestaetigt/ergaenzt)

- **OPNsense DARF NICHT ausfallen** bei Node-Isolation! Tolerations hinzufuegen!
- **KEIN kubectl cordon** fuer Experiment-Isolation! Nur Taint reicht.
  Cordon blockiert Longhorn instance-manager und andere System-Pods.
- **Longhorn taint-toleration** MUSS gesetzt werden wenn Node getaintet wird,
  damit instance-manager erstellt werden kann.
- **NFS-Ganesha in nfs-provisioner Image:**
  - Binary: `/usr/local/bin/ganesha.nfsd`
  - Plugins: `/usr/local/lib64/ganesha`
  - KEIN CLIENT-Block in Ganesha 4.x Config
  - PID-Dir manuell erstellen
