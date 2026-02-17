# Session 10: Infrastruktur-Manifeste + Security Audit
**Datum:** 2026-02-18
**Agent:** Infrastruktur-Agent (parallel zu Code-Agent)
**Kontext:** 1 (UNTERBROCHEN wegen Kontext-Ende)
**Branch:** development

---

## Zusammenfassung

Infrastruktur-as-Code komplett erstellt (14 neue Dateien), Security-Audit aller Datenbanken durchgeführt. **6 kritische Security-Findings** identifiziert, die VOR dem ersten Experiment behoben werden müssen. Kundendaten auf PostgreSQL (GitLab), Redis und Kafka sind aktuell UNZUREICHEND geschützt.

---

## 1. Umgebungslimitationen (Windows-Arbeitsplatz)

| Komponente | Status | Details |
|-----------|--------|---------|
| Bash Shell | **KAPUTT** | Exit Code 1 bei jedem Befehl, kein Output |
| PowerShell | **FUNKTIONIERT** | Alle Befehle über `powershell.exe -Command "..."` |
| kubectl lokal | **FEHLT** | Kein `.kube/` Verzeichnis, kein kubeconfig |
| talosctl lokal | **FEHLT** | Kein `.talos/` Verzeichnis |
| SSH zu OPNsense | **OK** | `ssh root@10.0.10.11` via VLAN 10 (Key: ~/.ssh/cluster) |
| SSH zu pve1 | **OK** | `ssh root@192.168.178.44` via WLAN (Fritz.Box) |
| kubectl via pve1 | **OK** | `ssh root@192.168.178.44 'kubectl ...'` |
| Netzwerk WLAN | 192.168.178.21 | Fritz.Box, Default GW |
| Netzwerk Ethernet | 10.0.10.100 | VLAN 10 MGMT, Route 10.0.0.0/8 via 10.0.10.1 |
| Ping OPNsense WAN | **TIMEOUT** | .81-.84 nicht erreichbar (pf blockiert ICMP?) |

**Empfohlener kubectl-Pfad:** `ssh root@192.168.178.44 'kubectl ...'` (pve1 direkt)

---

## 2. Cluster-Status (verifiziert via pve1)

| Komponente | Status | Details |
|-----------|--------|---------|
| K8s Version | **v1.34.0** | Upgrade von v1.32 (Session 66+) |
| Nodes | **4/4 Ready** | qkr-yc0, lux-kpk, 5x2-s49, say-ls6 |
| Samba AD | **4/4 Running** | IPs .16-.19 (Calico IPReservation) |
| PostgreSQL | **4/4 Running** | Namespace databases |
| CockroachDB | **4/4 Running** | Namespace cockroach-operator-system, TLS AKTIV |
| Redis | **4/4 Running** | Namespace redis |
| Kafka | **4B+4C Running** | Namespace kafka, Strimzi Operator |
| MinIO | **4/4 Running** | Namespace minio, LB 10.0.90.55 |
| GitLab | **23/23 Running** | Namespace gitlab |

---

## 3. Erstellte Infrastruktur-Dateien (14 Stück)

### Dockerfile + .dockerignore
```
Dockerfile              -- Multi-stage: gcc:14-bookworm → debian:bookworm-slim
.dockerignore           -- Build-Context Optimierung
```

### k8s/base/ (Foundation)
```
namespace.yaml          -- monitoring + databases Namespaces
rbac.yaml               -- ServiceAccount dedup-experiment + ClusterRole (Longhorn read)
configmap.yaml          -- Experiment-Config (7 DB-Systeme, Prometheus, Kafka, Git Export)
secrets.yaml            -- TEMPLATE mit Platzhaltern (REPLACE_ME)
```

### k8s/mariadb/
```
statefulset.yaml        -- MariaDB 11.7, 1 Replica, 50Gi longhorn-database
                           ConfigMap: InnoDB Buffer 512M, max_allowed_packet 64M
                           Init-SQL: CREATE DATABASE dedup_lab + GRANT
```

### k8s/clickhouse/
```
statefulset.yaml        -- ClickHouse 24.12, 1 Replica, 50Gi longhorn-database
                           MergeTree/LZ4, Prometheus :9363, HTTP :8123, Native :9000
                           ConfigMap: config.xml + users.xml
```

### k8s/monitoring/
```
values.yaml             -- kube-prometheus-stack Helm Values
                           Scrape: Longhorn :9500, Kafka JMX :9404, ClickHouse :9363, MinIO
                           Pushgateway für C++ Metrics Push
                           Grafana mit Dashboard Pre-Load
grafana-dashboard-configmap.yaml -- Dashboard ConfigMap Referenz
install.sh              -- Helm Install Script
```

### k8s/jobs/
```
experiment-job.yaml     -- K8s Job: 500 files × 3 grades, 6h timeout
cleanup-job.yaml        -- K8s Job: --cleanup-only, 10 min timeout
```

### k8s/
```
deploy-all.sh           -- Orchestrator: richtige Reihenfolge (NS → RBAC → Secrets → DBs → Helm)
```

### Geänderte Dateien
```
.gitlab-ci.yml          -- SYSTEMS += mariadb,clickhouse
                           PROMETHEUS_URL korrigiert auf kube-prometheus-stack-*
```

---

## 4. SECURITY AUDIT - KRITISCHE FINDINGS

### F1: KRITISCH - Redis hat KEINE Authentifizierung
```
Redis ACL: default user = nopass ~* &* +@all
requirepass = (leer)
```
- **Risiko:** JEDER Pod im Cluster kann ALLE Redis-Keys lesen/schreiben
- **Betroffen:** GitLab Redis Cache (Kundendaten: Sessions, CI Jobs, Caches)
- **Fix:** Redis ACL + requirepass setzen, GitLab-Pods mit Passwort konfigurieren
- **Downtime:** ~30 Sekunden (Redis Config Reload)

### F2: HOCH - PostgreSQL pg_hba.conf ist weit offen
```
host all all all scram-sha-256
```
- **Risiko:** dedup-lab User kann sich zu ALLEN Datenbanken verbinden
- **Betroffen:** `gitlabhq_production`, `praefect` (GitLab Kundendaten)
- **Fix:** pg_hba.conf einschränken:
  ```
  host dedup_lab  dedup-lab  all  scram-sha-256
  host all        gitlab     all  scram-sha-256
  ```
- **Downtime:** ~10 Sekunden (PostgreSQL pg_reload_conf())

### F3: HOCH - Keine NetworkPolicies auf Datenbank-Namespaces
- **Status:** Nur `kafka` und `calico-system` haben NetworkPolicies
- **Risiko:** Jeder Pod in jedem Namespace kann alle DB-Services erreichen
- **Fix:** NetworkPolicies pro Namespace erstellen (allow nur autorisierte Pods)
- **Downtime:** 0 (additiv, blockiert nichts wenn richtig konfiguriert)

### F4: MITTEL - Kein dediziertes dedup_lab Schema/Database auf PostgreSQL
- **Status:** dedup-lab Rolle existiert, aber kein Schema/Database
- **Fix:** `CREATE SCHEMA dedup_lab; GRANT ALL ON SCHEMA dedup_lab TO "dedup-lab"; REVOKE ALL ON SCHEMA public FROM "dedup-lab";`
- **Downtime:** 0

### F5: MITTEL - PostgreSQL Local Trust
```
local all all trust
```
- **Risiko:** Pods im gleichen Container haben Vollzugriff ohne Passwort
- **Fix:** `local all all scram-sha-256` (nach Passwort-Setup)
- **Downtime:** ~10 Sekunden

### F6: NIEDRIG - dedup-lab User hat sich nie eingeloggt
- **Status:** `logonCount: 0`, Account aktiv seit 2026-02-16
- **Aktion:** Nach DB-Setup ersten Login testen

---

## 5. Samba AD User Status

```
DN: CN=Dedup Lab,CN=Users,DC=comdare,DC=de
UPN: dedup-lab@comdare.de
Beschreibung: "Dedup Research Integration Test User - lab schema access ONLY"
Gruppe: Research Lab (einziges Mitglied)
Status: Enabled (userAccountControl: 512)
Erstellt: 2026-02-16 21:59:27 UTC
Letzter Login: NIE
```

---

## 6. Plan: DB-Sicherheit + Samba AD Migration

### Phase A: PostgreSQL absichern (Downtime ~30s)

```bash
# 1. dedup_lab Schema erstellen + Rechte einschränken
ssh root@192.168.178.44 "kubectl exec -n databases postgres-ha-0 -- psql -U postgres -c \"
  CREATE SCHEMA IF NOT EXISTS dedup_lab AUTHORIZATION \\\"dedup-lab\\\";
  GRANT CONNECT ON DATABASE postgres TO \\\"dedup-lab\\\";
  GRANT USAGE, CREATE ON SCHEMA dedup_lab TO \\\"dedup-lab\\\";
  REVOKE ALL ON SCHEMA public FROM \\\"dedup-lab\\\";
  REVOKE ALL ON DATABASE gitlabhq_production FROM \\\"dedup-lab\\\";
  REVOKE ALL ON DATABASE praefect FROM \\\"dedup-lab\\\";
\""

# 2. pg_hba.conf anpassen (KURZE Downtime)
# Ersetze: host all all all scram-sha-256
# Durch:   host dedup_lab  dedup-lab  0.0.0.0/0  scram-sha-256
#          host all         all        0.0.0.0/0  scram-sha-256
# Dann: SELECT pg_reload_conf();
```

### Phase B: Redis absichern (Downtime ~30s)

```bash
# 1. Redis ACL setzen
# Default User mit Passwort versehen
# dedup-lab User NUR auf dedup:* Keys beschränken
kubectl exec -n redis redis-cluster-0 -- redis-cli ACL SETUSER dedup-lab on '>PASSWORT' '~dedup:*' '+@all'
kubectl exec -n redis redis-cluster-0 -- redis-cli ACL SETUSER default on '>REDIS_PASSWORT' '~*' '+@all'

# 2. GitLab Redis-Pods mit neuem Passwort konfigurieren
# Secret gitlab-redis aktualisieren, Pods rollout restart

# 3. requirepass setzen
kubectl exec -n redis redis-cluster-0 -- redis-cli CONFIG SET requirepass 'REDIS_PASSWORT'
```

### Phase C: Kafka Topic-ACLs (Downtime 0)

```bash
# dedup-lab darf NUR dedup-lab-* Topics verwenden
# Strimzi KafkaUser CRD erstellen
kubectl apply -f - <<EOF
apiVersion: kafka.strimzi.io/v1beta2
kind: KafkaUser
metadata:
  name: dedup-lab
  namespace: kafka
  labels:
    strimzi.io/cluster: kafka-cluster
spec:
  authentication:
    type: scram-sha-512
  authorization:
    type: simple
    acls:
      - resource:
          type: topic
          name: dedup-lab-
          patternType: prefix
        operations: [Read, Write, Create, Describe]
      - resource:
          type: group
          name: dedup-lab-
          patternType: prefix
        operations: [Read]
EOF
```

### Phase D: MinIO Bucket Policy (Downtime 0)

```bash
# dedup-lab User darf NUR dedup-lab-* Buckets verwenden
# Policy erstellen über mc CLI
mc admin policy create minio dedup-lab-policy /dev/stdin <<EOF
{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Action": ["s3:*"],
    "Resource": ["arn:aws:s3:::dedup-lab-*", "arn:aws:s3:::dedup-lab-*/*"]
  }]
}
EOF
mc admin policy attach minio dedup-lab-policy --user dedup-lab
```

### Phase E: CockroachDB (bereits sicher - TLS)
- Secure Mode aktiv, Client-Zertifikate erforderlich
- Eigenes Zertifikat für dedup-lab User erstellen

### Phase F: NetworkPolicies (Downtime 0)

```yaml
# Experiment-Pods dürfen NUR DB-Services auf definierten Ports erreichen
# Kein Zugriff auf GitLab, Samba AD Admin, oder andere Namespaces
apiVersion: networking.k8s.io/v1
kind: NetworkPolicy
metadata:
  name: dedup-experiment-egress
  namespace: databases
spec:
  podSelector:
    matchLabels:
      app.kubernetes.io/name: dedup-experiment
  policyTypes: [Egress]
  egress:
    - to:
      - podSelector:
          matchLabels: {}  # Nur innerhalb databases Namespace
      ports:
        - port: 5432   # PostgreSQL
        - port: 3306   # MariaDB
        - port: 8123   # ClickHouse HTTP
    - to:
      - namespaceSelector:
          matchLabels:
            kubernetes.io/metadata.name: redis
      ports:
        - port: 6379
    - to:
      - namespaceSelector:
          matchLabels:
            kubernetes.io/metadata.name: kafka
      ports:
        - port: 9092
    - to:
      - namespaceSelector:
          matchLabels:
            kubernetes.io/metadata.name: minio
      ports:
        - port: 9000
    - to:
      - namespaceSelector:
          matchLabels:
            kubernetes.io/metadata.name: monitoring
      ports:
        - port: 9090  # Prometheus
        - port: 9091  # Pushgateway
```

---

## 7. Reihenfolge nächste Session

### MUSS (Security, VOR erstem Experiment)
1. **PostgreSQL pg_hba + REVOKE** (~30s Downtime) - F2+F4+F5
2. **Redis ACL + Passwort** (~30s Downtime, GitLab Secret updaten) - F1
3. **Kafka KafkaUser CRD** (0 Downtime) - Phase C
4. **MinIO Bucket Policy** (0 Downtime) - Phase D
5. **NetworkPolicies erstellen** (0 Downtime) - F3
6. **k8s/base/secrets.yaml mit echten Passwörtern füllen**

### SOLL (Infra Deploy)
7. MariaDB deployen: `kubectl apply -f k8s/mariadb/statefulset.yaml`
8. ClickHouse deployen: `kubectl apply -f k8s/clickhouse/statefulset.yaml`
9. Prometheus/Grafana deployen: `bash k8s/monitoring/install.sh`
10. Dockerfile bauen + in Registry pushen

### KANN (Verifizierung)
11. dedup-lab Login auf jeder DB testen
12. Dry-Run Experiment starten
13. Ergebnisse verifizieren

---

## 8. Code-Agent Status (parallel)

Der Code-Agent hat in dieser Session folgende C++ Änderungen gemacht:
- `db_connector.hpp` - `per_file_latencies_ns` Vector hinzugefügt
- `data_loader.hpp` - Latenz-Statistiken (p50/p95/p99/min/max/mean)
- `data_loader.cpp` - Latenz-Propagation + JSON Export
- Alle Connectors (PG, CRDB, Redis, Kafka, MinIO, MariaDB, CH, comdare) - perfile_insert/delete Fixes

**NICHT COMMITTED** - Beide Agents haben uncommitted Changes auf `development`.

---

## 9. Git Status (uncommitted)

### Infrastruktur-Agent (dieses Dokument):
```
Neue Dateien:
  .dockerignore
  Dockerfile
  k8s/base/namespace.yaml
  k8s/base/rbac.yaml
  k8s/base/configmap.yaml
  k8s/base/secrets.yaml
  k8s/mariadb/statefulset.yaml
  k8s/clickhouse/statefulset.yaml
  k8s/monitoring/values.yaml
  k8s/monitoring/grafana-dashboard-configmap.yaml
  k8s/monitoring/install.sh
  k8s/jobs/experiment-job.yaml
  k8s/jobs/cleanup-job.yaml
  k8s/deploy-all.sh
  sessions/20260218-session-10-*

Geändert:
  .gitlab-ci.yml (SYSTEMS += mariadb,clickhouse, Prometheus URL fix)
```

### Code-Agent (parallel):
```
Geändert:
  src/cpp/connectors/db_connector.hpp
  src/cpp/connectors/postgres_connector.cpp
  src/cpp/connectors/redis_connector.cpp
  src/cpp/connectors/kafka_connector.cpp
  src/cpp/connectors/minio_connector.cpp
  src/cpp/connectors/mariadb_connector.cpp
  src/cpp/connectors/clickhouse_connector.cpp
  src/cpp/connectors/comdare_connector.cpp
  src/cpp/experiment/data_loader.hpp
  src/cpp/experiment/data_loader.cpp
```

---

## 10. Offene Risiken

| Risiko | Schwere | Mitigation |
|--------|---------|------------|
| Redis ohne Auth | KRITISCH | Phase B (nächste Session, 30s Downtime) |
| PG Lab-User kann auf GitLab DB | HOCH | Phase A (REVOKE, 10s Downtime) |
| Keine NetworkPolicies | HOCH | Phase F (0 Downtime, additiv) |
| secrets.yaml hat Platzhalter | MITTEL | Vor Deploy mit echten Werten füllen |
| Bash auf Windows kaputt | NIEDRIG | PowerShell workaround funktioniert |
| Kein lokales kubeconfig | NIEDRIG | SSH via pve1 (192.168.178.44) |

---

## 11. Kubectl-Zugriff Cheatsheet

```powershell
# Von Windows aus (PowerShell):
powershell.exe -Command "ssh root@192.168.178.44 'kubectl get pods -A'"

# Oder SSH Session öffnen:
ssh root@192.168.178.44
kubectl get pods -n databases
kubectl get pods -n redis
kubectl get pods -n kafka

# Samba AD User prüfen:
kubectl exec -n samba-ad samba-ad-0 -- sh -c 'samba-tool user show dedup-lab'

# PostgreSQL Zugriff:
kubectl exec -n databases postgres-ha-0 -- psql -U postgres -c '\du'
```

---

## Nächster Kontext: Session 10b

**Priorität:** Security Hardening (Phasen A-F) VOR allen anderen Aufgaben.
**Geschätzte Downtime:** ~60 Sekunden total (PostgreSQL 30s + Redis 30s).
**Parallel testbar:** JA, nach Security-Fix sofort Dry-Run möglich.
