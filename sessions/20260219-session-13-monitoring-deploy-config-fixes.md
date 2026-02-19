# Session 13: Monitoring Stack Deploy + Config-Korrekturen
**Datum:** 2026-02-19
**Agent:** Code-Agent (Forschungsprojekt)
**Branch:** development
**Vorheriger Commit:** `36206c9` (Session 12 final)
**Neuer Commit:** `227f71d` (config fixes + pushgateway endpoints)

---

## Zusammenfassung

Infrastruktur-Session: (1) Longhorn Engine Image Fix fuer MariaDB-2, (2) Prometheus
Monitoring Stack Deployment via server-side apply, (3) Pushgateway separat deployed,
(4) Config-Korrekturen fuer korrekte Service-Endpunkte, (5) DB-User-Verifizierung.

---

## 1. Longhorn Engine Image Fix (ERLEDIGT)

### Problem
- `mariadb-2` stuck in ContainerCreating seit 10h
- Root Cause: Longhorn Engine Image `longhornio/longhorn-engine:v1.7.2` Status = `deploying`
- Engine Image Pod auf `talos-5x2-s49` war `Terminating` (stale)

### Loesung
```bash
kubectl delete pod engine-image-ei-51cc7b9c-btnn2 -n longhorn-system --force --grace-period=0
```
- Neuer Pod `tdnvg` in 10s Running
- Engine Image Status wechselte auf `deployed`
- MariaDB-2 startete sofort, MariaDB-3 folgte
- **Ergebnis:** MariaDB 4/4 Running, ClickHouse 4/4 Running, PostgreSQL 4/4 Running

---

## 2. Prometheus Monitoring Stack Deployment

### Voraussetzungen (aus Session 68)
- 10/10 Prometheus CRDs installiert (Schema-Stripping Workaround)
- Helm Repo `prometheus-community` vorhanden
- K8s API Load: 4 (vorher 12+ = Blocker)

### Helm Install Versuch
```bash
helm install kube-prometheus-stack ... --skip-crds
```
**FEHLGESCHLAGEN:** `http2: client connection lost` — auch mit `--skip-crds`.
Die K8s API verliert http2-Verbindungen bei Bulk-Resource-Erstellung.

### Workaround: Template + Server-Side Apply
```bash
helm template ... > /tmp/prom-rendered.yaml  # 5472 Zeilen, 533KB
kubectl apply --server-side --force-conflicts -f /tmp/prom-rendered.yaml
```
- Meiste Ressourcen erfolgreich applied
- Letzte ~7 Ressourcen scheiterten an http2 (gleicher Fehler)
- Zweiter Apply (mit korrigiertem values.yaml) ergaenzte fehlende Ressourcen

### PodSecurity Fix
- Monitoring Namespace brauchte `privileged` Label fuer Node Exporter
```bash
kubectl label namespace monitoring \
  pod-security.kubernetes.io/enforce=privileged \
  pod-security.kubernetes.io/audit=privileged \
  pod-security.kubernetes.io/warn=privileged --overwrite
```

### Pushgateway (separates Chart)
kube-prometheus-stack enthaelt KEINE Pushgateway Sub-Chart.
Separat deployed:
```bash
helm template pushgateway prometheus-community/prometheus-pushgateway -n monitoring ...
kubectl apply --server-side -f /tmp/pushgateway-rendered.yaml
```
- Service: `pushgateway-prometheus-pushgateway.monitoring.svc.cluster.local:9091`

### Image Pull Status (Session-Ende)
| Komponente | Status | Image |
|-----------|--------|-------|
| Operator | 1/1 Running | quay.io/prometheus-operator/... |
| kube-state-metrics | 1/1 Running | registry.k8s.io/kube-state-metrics/... |
| Pushgateway | 1/1 Running | quay.io/prometheus/pushgateway |
| Node Exporter | 3/4 Running | quay.io/prometheus/node-exporter |
| Prometheus | PodInitializing | quay.io/prometheus/prometheus:v3.9.1 |
| Alertmanager | Init | quay.io/prometheus/alertmanager:v0.31.1 |
| Grafana | ErrImagePull | docker.io/grafana/grafana:12.3.3 + quay.io/kiwigrid/k8s-sidecar |

**Bekanntes Problem:** DNS/TLS Timeouts zu quay.io von Talos Nodes.
Images werden bei Retries langsam gezogen. Kein manueller Eingriff noetig.

---

## 3. Config-Korrekturen (Commit 227f71d)

### 3.1 Pushgateway Scrape Target
- **Vorher:** `kube-prometheus-stack-pushgateway.monitoring.svc...`
- **Nachher:** `pushgateway-prometheus-pushgateway.monitoring.svc...`
- Betrifft: `k8s/monitoring/values.yaml`, `config.example.json`, `configmap.yaml`

### 3.2 Service-Hostnamen
- MariaDB: `mariadb-lb.databases...` → `mariadb.databases...` (kein LB-Service)
- ClickHouse: `clickhouse-lb.databases...` → `clickhouse.databases...`
- Tatsaechliche K8s Services: `mariadb` und `clickhouse` (ohne `-lb`)

### 3.3 Grafana URL = Pushgateway
- `grafana.url` im Code wird fuer Pushgateway-Push verwendet (NICHT Grafana API)
- Vorher: leer oder Grafana HTTP
- Nachher: `http://pushgateway-prometheus-pushgateway....:9091`

### 3.4 Deployment-Status aktualisiert
- MariaDB/ClickHouse Kommentare: "TODO: deploy" → "Deployed as StatefulSet"
- Prometheus Kommentar: "TODO: deploy" → "deployed to monitoring namespace"

---

## 4. DB-User Verifizierung

### MariaDB
```sql
-- Users: dedup-lab (%), root (%), healthcheck (localhost), mariadb.sys (localhost)
-- Databases: dedup_lab, mysql, information_schema, performance_schema, sys
```
- `dedup-lab` User existiert mit `%` Host (ueberall erreichbar)
- `dedup_lab` Database automatisch erstellt (MARIADB_DATABASE env var)
- Root-Passwort: `83n]am!nP.` (via Secret `dedup-credentials`)

### ClickHouse
```sql
-- Users: dedup_lab, default
-- Databases: dedup_lab, default, system, information_schema
```
- `dedup_lab` User existiert
- `dedup_lab` Database automatisch erstellt

---

## 5. Gesamtstatus: Alle 7 Datenbanken

| # | System | Pods | User | Database | PVC | Status |
|---|--------|------|------|----------|-----|--------|
| 1 | PostgreSQL | 4/4 Running | dedup-lab | postgres/dedup_lab | 50Gi x4 | BEREIT |
| 2 | CockroachDB | 3/3 Running | dedup_lab | dedup_lab | 125Gi x3 | BEREIT |
| 3 | Redis | 3/3 Running | dedup-lab | N/A (dedup:*) | 50Gi x3 | BEREIT |
| 4 | Kafka | 3/3 Running | dedup-lab | N/A (dedup-lab-*) | 50Gi x3 | BEREIT |
| 5 | MinIO | 4/4 Running | dedup-lab-s3 | dedup-lab bucket | DirectDisk | BEREIT |
| 6 | MariaDB | 4/4 Running | dedup-lab | dedup_lab | 50Gi x4 | BEREIT (NEU) |
| 7 | ClickHouse | 4/4 Running | dedup_lab | dedup_lab | 50Gi x4 | BEREIT (NEU) |

---

## 6. Monitoring Stack Detail

### Kubernetes Ressourcen (erstellt)
- Namespace: `monitoring` (privileged PodSecurity)
- Prometheus CRDs: 10/10
- Prometheus CR: v3.9.1, 1 Replica
- Alertmanager CR: v0.31.1, 1 Replica
- ServiceMonitors: 7 (grafana, kube-state-metrics, node-exporter, alertmanager, kubelet, operator, prometheus)
- PrometheusRules: 14 (vordefinierte Alert-Regeln)
- DaemonSet: node-exporter (4 Pods)
- Deployments: operator, kube-state-metrics, grafana, pushgateway
- Services: 6 (alertmanager, grafana, kube-state-metrics, operator, prometheus, node-exporter) + pushgateway
- Grafana Dashboard ConfigMap: `dedup-grafana-dashboard`
- Experiment ConfigMap: `dedup-experiment-config` (in databases namespace)

### Scrape Configs
| Job | Target | Port |
|-----|--------|------|
| longhorn | longhorn-manager pods | 9500 |
| kafka-jmx | Strimzi Kafka pods | 9404 |
| clickhouse | clickhouse.databases.svc | 9363 |
| minio | minio-lb.minio.svc | 9000 |
| pushgateway | pushgateway-prometheus-pushgateway.monitoring.svc | 9091 |

---

## 7. Commits

```
227f71d fix: correct pushgateway endpoints and service hostnames for deployed infrastructure
```

---

## 8. Task-Status

| Task | Status | Details |
|------|--------|---------|
| #64 | IN_PROGRESS | Prometheus stack deployed, Images noch am Pullen |
| #72 | IN_PROGRESS | Server-side apply statt Helm install |
| #73 | PENDING | Gitaly Scale (nicht Forschungsprojekt-Scope) |
| #75 | DONE | Longhorn engine image fix → MariaDB 4/4 |

---

## 9. CONTEXT RECOVERY (fuer naechste Session)

### Image Pulls abwarten
Die Monitoring-Pods pullen noch Images von quay.io (DNS/TLS Timeouts).
Pruefen mit:
```bash
kubectl get pods -n monitoring --no-headers
```
Erwartung: Alle Pods werden nach mehreren Retries Running sein.

### Experiment-Framework
KOMPLETT implementiert (Session 12). 864 Konfigurationen:
9 Payload-Typen x 8 Systeme x 3 Dup-Grades x 4 Stages

### Naechste Schritte (priorisiert)
1. **Monitoring pruefen** — Prometheus/Grafana/Alertmanager Running?
2. **Erster Dry-Run** — Verbindungstests zu allen 7 DBs
3. **Erster Experimentlauf** — Pipeline 3 (manual trigger)
4. **doku.tex Phase 2** — Abschnitte fuer DuckDB, Cassandra, MongoDB
5. **P1 Features** — DB-Varianten (TOAST, RMT, TTL, compaction)

### Kritische Dateipfade
| Was | Pfad |
|-----|------|
| Session 13 | `sessions/20260219-session-13-monitoring-deploy-config-fixes.md` |
| C++ Source | `src/cpp/` (34 Dateien, ~8.036 LOC) |
| K8s Manifeste | `k8s/` (monitoring/values.yaml, base/configmap.yaml) |
| Grafana Dashboard | `docs/grafana/dedup-experiment-dashboard.json` |
| Helm Values | `k8s/monitoring/values.yaml` |

### Git-Status
```
Branch:       development
Letzter Commit: f785311 (docs: session 13)
Remote:       GitLab (1316 skipped) + GitHub (both synced)
Pipeline:     Skipped (nur Config-Aenderungen, keine C++/LaTeX)
```

---

## 10. Experiment-DB Shutdown (User-Anweisung: Leistung fuer andere Aufgaben)

### Analyse: Produktion vs. Experiment

| Namespace | Memory Req | Produktion? | Entscheidung |
|-----------|-----------|-------------|--------------|
| gitlab | 11.454 Mi | JA (GitLab) | Laufen lassen |
| kafka | 12.672 Mi | 0 Consumer | TODO: User entscheidet |
| databases (PG) | 4.096 Mi | JA (GitLab DB) | Laufen lassen |
| databases (MariaDB) | 2.048 Mi | NUR Experiment | **Heruntergefahren** |
| databases (ClickHouse) | 2.048 Mi | NUR Experiment | **Heruntergefahren** |
| cockroach | 8.224 Mi | Geteilt/Produktion | Laufen lassen |
| redis (redis ns) | 2.048 Mi | NUR Experiment | **Heruntergefahren** |
| minio | 2.048 Mi | JA (GitLab) | Laufen lassen |
| monitoring | 448 Mi | NUR Experiment | **Heruntergefahren** |

**Wichtige Erkenntnis:** GitLab hat EIGENEN Redis (`redis-gitlab-*` in `gitlab` Namespace).
Der `redis-cluster` im `redis` Namespace ist rein experimentell!

### Ausgefuehrte Befehle
```bash
# MariaDB, ClickHouse, Redis (experiment) → 0 Replicas
kubectl scale statefulset mariadb clickhouse -n databases --replicas=0
kubectl scale statefulset redis-cluster -n redis --replicas=0

# Monitoring komplett → 0 Replicas
kubectl scale deployment kube-prometheus-stack-grafana \
  kube-prometheus-stack-kube-state-metrics \
  kube-prometheus-stack-operator \
  pushgateway-prometheus-pushgateway -n monitoring --replicas=0
kubectl scale statefulset alertmanager-kube-prometheus-stack-alertmanager \
  prometheus-kube-prometheus-stack-prometheus -n monitoring --replicas=0
# Node Exporter DaemonSet: nodeSelector auf non-existing gesetzt
```

### Freigegebene Ressourcen
- **~6.6 Gi Memory Requests** sofort frei (MariaDB+ClickHouse+Redis+Monitoring)
- **~12 Gi Limits** zusaetzlich frei
- PVCs bleiben erhalten (alle Daten sicher)
- Kafka (12.4 Gi) offen — User-Entscheidung ausstehend

### Wiederherstellen
```bash
kubectl scale statefulset mariadb -n databases --replicas=4
kubectl scale statefulset clickhouse -n databases --replicas=4
kubectl scale statefulset redis-cluster -n redis --replicas=4
kubectl scale deployment kube-prometheus-stack-grafana \
  kube-prometheus-stack-kube-state-metrics \
  kube-prometheus-stack-operator \
  pushgateway-prometheus-pushgateway -n monitoring --replicas=1
kubectl scale statefulset alertmanager-kube-prometheus-stack-alertmanager \
  prometheus-kube-prometheus-stack-prometheus -n monitoring --replicas=1
kubectl patch daemonset kube-prometheus-stack-prometheus-node-exporter \
  -n monitoring --type=json \
  -p='[{"op":"remove","path":"/spec/template/spec/nodeSelector/non-existing"}]'
```

---

## 11. Session-Ende Zusammenfassung

### Commits (2)
```
227f71d fix: correct pushgateway endpoints and service hostnames
f785311 docs: session 13 — monitoring stack deploy, Longhorn fix, config corrections
```

### Erledigte Aufgaben
1. Longhorn Engine Image fix → MariaDB 4/4 Running
2. Prometheus Monitoring Stack deployed (server-side apply)
3. Pushgateway separat deployed
4. Config-Korrekturen (Pushgateway-Endpunkte, Service-Namen)
5. DB-User verifiziert (MariaDB + ClickHouse)
6. Experiment-DBs heruntergefahren (User-Anweisung: Leistung)

### Aktiver Cluster-Zustand (Session-Ende)
- PostgreSQL HA: 4/4 Running (GitLab Produktion)
- CockroachDB: 4/4 Running (Produktion geteilt)
- MinIO: 4/4 Running (GitLab Artifacts)
- Kafka: 8/8 Running (0 Consumer, TODO: herunterfahren?)
- GitLab Redis: 4/4 Running
- **MariaDB: 0/0 (SUSPENDED)**
- **ClickHouse: 0/0 (SUSPENDED)**
- **Redis experiment: 0/0 (SUSPENDED)**
- **Monitoring: 0/0 (SUSPENDED)**
