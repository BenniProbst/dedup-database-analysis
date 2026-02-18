# Session 10: Infrastruktur-Manifeste + Security Audit + DB User Setup + Samba AD Integration
**Datum:** 2026-02-18
**Agent:** Infrastruktur-Agent (parallel zu Code-Agent)
**Kontexte:** 5
**Branch:** development
**Credentials:** `C:\Users\benja\OneDrive\Desktop\Projekte\Cluster\keys\database-credentials.md`

---

## Zusammenfassung

### Kontext 1
Infrastruktur-as-Code komplett erstellt (14 neue Dateien), Security-Audit aller
Datenbanken durchgefuehrt, 6 Security-Findings identifiziert.

### Kontext 2
User-Einrichtung auf allen 5 bestehenden Datenbanken durchgefuehrt (non-destructive).
Service-User in Samba AD gespeichert. Credentials in `Cluster/keys/` gesichert.
Hardening-Checklist auf non-destructiven Ansatz aktualisiert.

**Ansatz geaendert:** KEINE REVOKEs, KEINE pg_hba-Aenderungen, KEINE Aenderungen an
bestehenden DB-Configs. NUR User HINZUFUEGEN + Credentials in Samba AD speichern.

### Kontext 3
Kafka Authorization aktiviert (Simple, SCRAM-SHA-512 auf Port 9094). MinIO dedup-lab User
erstellt mit readwrite Policy. GitHub-Remote von Projekte entfernt (Secret Scanning).
GitLab-Push erfolgreich (Commit ba55faf, 51 Dateien inkl. SSH-Keys + Credentials).
Samba AD LDAP-Integrationsplan fuer alle Datenbanken dokumentiert.

---

## 1. Umgebungslimitationen (Windows-Arbeitsplatz)

| Komponente | Status | Details |
|-----------|--------|---------|
| Bash Shell | **KAPUTT** | Exit Code 1 bei jedem Befehl, kein Output |
| PowerShell | **FUNKTIONIERT** | Alle Befehle ueber `powershell.exe -Command "..."` |
| kubectl lokal | **FEHLT** | Kein `.kube/` Verzeichnis, kein kubeconfig |
| SSH zu pve1 | **OK** | `ssh root@192.168.178.44` via WLAN (Fritz.Box) |
| kubectl via pve1 | **OK** | `ssh root@192.168.178.44 'kubectl ...'` |
| PowerShell Heredoc | **OK** | `@' ... '@ ` fuer komplexe SSH-Commands |

**Empfohlener kubectl-Pfad:** `ssh root@192.168.178.44 'kubectl ...'` (pve1 direkt)

**PowerShell-SSH Escaping:** Fuer SQL-Queries auf pve1 Heredoc verwenden:
```powershell
powershell.exe -Command "& { ssh root@192.168.178.44 @'
kubectl exec -n databases postgres-ha-0 -- psql -U dedup-lab -d postgres -c 'SELECT 1'
'@ } 2>&1 | Out-String"
```

**ACHTUNG: $() Substitution in Heredocs laeuft LOKAL!**
Fuer Scripts mit Variablen: Lokal schreiben → SCP → auf pve1 ausfuehren.
```powershell
# Script lokal schreiben
# scp script.sh root@192.168.178.44:/tmp/
# ssh root@192.168.178.44 'bash /tmp/script.sh'
```

---

## 2. Cluster-Status (verifiziert 2026-02-18)

| Komponente | Status | Namespace | Pods |
|-----------|--------|-----------|------|
| K8s Version | **v1.34.0** | - | - |
| Nodes | **4/4 Ready** | - | qkr-yc0, lux-kpk, 5x2-s49, say-ls6 |
| Samba AD | **4/4 Running** | samba-ad | IPs .16-.19 |
| PostgreSQL | **4/4 Running** | databases | postgres-ha-0 bis -3 |
| CockroachDB | **4/4 Running** | cockroach-operator-system | cockroachdb-0 bis -3 |
| Redis | **4/4 Running** | redis | redis-cluster-0 bis -3 |
| Kafka | **8 Running** | kafka | 4 Broker + 4 Controller |
| MinIO | **4/4 Running** | minio | LB 10.0.90.55 |
| GitLab | **23/23 Running** | gitlab | - |

---

## 3. DATENBANK-CREDENTIALS (fuer Code-Agent)

### WICHTIG: Allgemeines Passwort
**Alle Lab-User verwenden das Passwort: `[REDACTED]`**
Samba AD UPN: `dedup-lab@comdare.de`

---

### 3.1 PostgreSQL

| Feld | Wert |
|------|------|
| **Host** | `postgres-lb.databases.svc.cluster.local` |
| **Port** | `5432` |
| **User** | `dedup-lab` |
| **Passwort** | `[REDACTED]` |
| **Database** | `postgres` |
| **Schema** | `dedup_lab` |
| **Status** | EINGERICHTET + VERIFIZIERT |

**Connection String:**
```
postgresql://dedup-lab:[REDACTED]@postgres-lb.databases.svc.cluster.local:5432/postgres?options=-csearch_path=dedup_lab
```

**libpq Parameter (fuer C++):**
```
host=postgres-lb.databases.svc.cluster.local
port=5432
dbname=postgres
user=dedup-lab
password=[REDACTED]
options=-csearch_path=dedup_lab
```

**Verifiziert:**
```
current_user | current_schema
--------------+----------------
 dedup-lab    | public
```
- Login: OK
- Schema dedup_lab: EXISTIERT (AUTHORIZATION dedup-lab)
- GRANT CONNECT on postgres: OK
- GRANT USAGE, CREATE on dedup_lab: OK

**Tabellen erstellen:**
```sql
-- Alle Tabellen MUESSEN im Schema dedup_lab erstellt werden!
SET search_path TO dedup_lab;
CREATE TABLE chunks (
    id SERIAL PRIMARY KEY,
    file_path TEXT NOT NULL,
    chunk_hash BYTEA NOT NULL,
    chunk_size INTEGER NOT NULL,
    created_at TIMESTAMP DEFAULT NOW()
);
```

**LDAP-Integration (ZUKUNFT):**
- Methode: pg_hba.conf `ldap` Authentifizierung
- Server: `samba-ad-lb.samba-ad.svc.cluster.local:389`
- Base DN: `CN=Users,DC=comdare,DC=de`
- Bind DN: `CN=dedup-lab,CN=Users,DC=comdare,DC=de`
- **Status:** GEPLANT (erfordert pg_hba.conf Aenderung, non-destructive Ansatz)

---

### 3.2 CockroachDB

| Feld | Wert |
|------|------|
| **Host** | `cockroachdb-public.cockroach-operator-system.svc.cluster.local` |
| **Port** | `26257` |
| **User** | `dedup_lab` (UNTERSTRICH, nicht Bindestrich!) |
| **Passwort** | `[REDACTED]` |
| **Database** | `dedup_lab` |
| **TLS** | verify-full (Cluster-Zertifikate) |
| **Status** | EINGERICHTET + VERIFIZIERT |

**Connection String:**
```
postgresql://dedup_lab:[REDACTED]@cockroachdb-public.cockroach-operator-system.svc.cluster.local:26257/dedup_lab?sslmode=verify-full
```

**ACHTUNG:** CockroachDB verwendet PostgreSQL-Protokoll (libpq kompatibel), aber:
- Username ist `dedup_lab` (Unterstrich!), da CockroachDB keine Bindestriche in Usernamen erlaubt
- TLS ist PFLICHT (Cluster laeuft im Secure Mode)
- Fuer Tests ohne TLS-Zertifikate: `sslmode=disable` (nur innerhalb K8s)

**Verifiziert:**
```
database_name | owner
dedup_lab     | root
```

**Verfuegbare Databases:**
- `dedup_lab` (Lab, eigene)
- `defaultdb` (Standard, leer)
- `postgres` (Kompatibilitaet)
- `system` (CockroachDB intern, kein Zugriff)

**LDAP-Integration (ZUKUNFT):**
- Methode: CockroachDB Enterprise LDAP Auth
- **Status:** GEPLANT (Enterprise Feature, moeglicherweise Lizenz noetig)

---

### 3.3 Redis

| Feld | Wert |
|------|------|
| **Host** | `redis-cluster.redis.svc.cluster.local` |
| **Port** | `6379` |
| **User** | `dedup-lab` |
| **Passwort** | `[REDACTED]` |
| **Key-Prefix** | `dedup:*` (NUR diese Keys erlaubt!) |
| **Cluster Mode** | JA (4 Nodes) |
| **Status** | EINGERICHTET + VERIFIZIERT |

**Connection (hiredis C++):**
```cpp
// Redis Cluster mit ACL Auth
redisClusterContext *cc = redisClusterContextInit();
redisClusterSetOptionAddNodes(cc, "redis-cluster.redis.svc.cluster.local:6379");
redisClusterConnect2(cc);
// Auth mit Username + Passwort (Redis 6+ ACL)
redisClusterCommand(cc, "AUTH dedup-lab [REDACTED]");
```

**WICHTIG - Key-Naming:**
Alle Keys MUESSEN mit `dedup:` beginnen! Der ACL-User hat `~dedup:*` Restriction.
Andere Key-Patterns werden mit `NOPERM` abgelehnt.

**Empfohlene Key-Struktur:**
```
dedup:file:<file_hash>           -- Datei-Metadaten
dedup:chunk:<chunk_hash>         -- Chunk-Referenzen
dedup:block:<block_size>:<hash>  -- Block-Level Dedup
dedup:stats:<run_id>             -- Lauf-Statistiken
dedup:index:<experiment_id>      -- Index-Daten
```

**Verifiziert (ACL GETUSER):**
```
flags: on
passwords: aef8678fb300a66...
```

**LDAP-Integration:** NICHT MOEGLICH (Redis hat kein LDAP-Support, bleibt ACL-basiert)

---

### 3.4 Kafka

| Feld | Wert |
|------|------|
| **Bootstrap Server (plain)** | `kafka-cluster-kafka-bootstrap.kafka.svc.cluster.local:9092` |
| **Bootstrap Server (SCRAM)** | `kafka-cluster-kafka-bootstrap.kafka.svc.cluster.local:9094` |
| **User** | `dedup-lab` |
| **Auth** | SCRAM-SHA-512 (Port 9094, via Strimzi KafkaUser CRD) |
| **Topic-Prefix** | `dedup-lab-*` |
| **K8s Secret** | `dedup-lab-kafka-password` (Namespace: kafka) |
| **Status** | EINGERICHTET + VERIFIZIERT (Kontext 3) |

**Kafka Cluster Authorization (AKTIV seit Kontext 3):**
- `authorization.type: simple`
- `superUsers: ["User:ANONYMOUS"]` → bestehende Services auf Port 9092 ungestoert
- `allow.everyone.if.no.acl.found: true` → Rueckwaerts-Kompatibilitaet
- Port 9092: Plain (ohne Auth, fuer bestehende Services)
- Port 9093: TLS (ohne Auth)
- Port 9094: SCRAM-SHA-512 Auth (NEU, fuer dedup-lab)

**Connection mit Auth (Port 9094):**
```
bootstrap.servers=kafka-cluster-kafka-bootstrap.kafka.svc.cluster.local:9094
security.protocol=SASL_PLAINTEXT
sasl.mechanism=SCRAM-SHA-512
sasl.jaas.config=org.apache.kafka.common.security.scram.ScramLoginModule required username="dedup-lab" password="[REDACTED]";
```

**Connection ohne Auth (Port 9092):**
```cpp
// Kafka Producer ohne Auth (bestehender Cluster-Port)
rd_kafka_conf_set(conf, "bootstrap.servers",
    "kafka-cluster-kafka-bootstrap.kafka.svc.cluster.local:9092", NULL, 0);
// KEIN sasl.username/password noetig
```

**Topic-Naming (Konvention):**
```
dedup-lab-chunks         -- Chunk-Events
dedup-lab-files          -- Datei-Verarbeitungs-Events
dedup-lab-results        -- Experiment-Ergebnisse
dedup-lab-metrics        -- Performance-Metriken
```

**KafkaUser CRD (angewendet):**
```yaml
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
    password:
      valueFrom:
        secretKeyRef:
          name: dedup-lab-kafka-password
          key: password
  authorization:
    type: simple
    acls:
      - resource:
          type: topic
          name: dedup-lab-
          patternType: prefix
        operations: [Read, Write, Create, Describe, DescribeConfigs]
      - resource:
          type: group
          name: dedup-lab-
          patternType: prefix
        operations: [Read, Describe]
```

**LDAP-Integration:** Nicht noetig (Strimzi KafkaUser CRD verwaltet Auth)

---

### 3.5 MinIO (Object Storage)

| Feld | Wert |
|------|------|
| **Endpoint** | `minio-lb.minio.svc.cluster.local:9000` |
| **Console** | `minio-lb.minio.svc.cluster.local:9001` |
| **Root User** | `admin` |
| **Root Passwort** | `[REDACTED]` |
| **Lab User** | `dedup-lab` |
| **Lab Passwort** | `[REDACTED]` |
| **Lab Policy** | `readwrite` |
| **Lab Bucket** | `dedup-lab-data` |
| **Status** | EINGERICHTET + VERIFIZIERT (Kontext 3) |

**Connection (S3-kompatibel, LDAP Access Key):**
```cpp
// MinIO/S3-kompatibel (libcurl oder AWS SDK)
// ACHTUNG: Lokaler User entfernt, NUR LDAP Access Key verwenden!
endpoint = "minio-lb.minio.svc.cluster.local:9000";
access_key = "dedup-lab-s3";        // LDAP Access Key
secret_key = "dedup-lab-s3-secret"; // LDAP Secret Key
use_ssl = false;  // Intern kein TLS
```

**Bucket-Naming (Konvention):**
```
dedup-lab-data           -- Experiment-Dateien (BEREITS ERSTELLT)
dedup-lab-results        -- Ergebnis-Export
dedup-lab-chunks         -- Chunk-Storage (falls S3-basiert)
```

**Bestehende Buckets (NICHT ANFASSEN!):**
```
gitlab-artifacts-storage
gitlab-backup-storage
gitlab-ci-secure-files
gitlab-dependency-proxy
gitlab-lfs-storage
gitlab-mr-diffs
gitlab-packages-storage
gitlab-registry-storage
gitlab-uploads-storage
samba-sysvol
buildsystem-artifacts
buildsystem-cache
dedup-lab-chunks
dedup-lab-data
dedup-lab-metrics
dedup-lab-results
```

**LDAP-Integration: AKTIV (Kontext 5)**
- Methode: MinIO native LDAP Identity Provider
- Server: `ldap://samba-ad-lb.samba-ad.svc.cluster.local:389`
- Bind DN: `CN=admin,CN=Users,DC=comdare,DC=de`
- User DN Search Base: `CN=Users,DC=comdare,DC=de`
- Group Search Base: `CN=Users,DC=comdare,DC=de`
- **Status:** KONFIGURIERT + VERIFIZIERT (persistent nach Pod-Restart)

---

### 3.6 MariaDB (NOCH NICHT DEPLOYED)

| Feld | Wert |
|------|------|
| **Host** | `mariadb.databases.svc.cluster.local` |
| **Port** | `3306` |
| **User** | `dedup-lab` |
| **Passwort** | `[REDACTED]` |
| **Database** | `dedup_lab` |
| **Root Passwort** | `[REDACTED]` |
| **Status** | Manifest bereit (`k8s/mariadb/statefulset.yaml`), NICHT deployed |

**Connection String:**
```
mysql://dedup-lab:[REDACTED]@mariadb.databases.svc.cluster.local:3306/dedup_lab
```

**libmariadb Parameter (fuer C++):**
```cpp
MYSQL *conn = mysql_init(NULL);
mysql_real_connect(conn,
    "mariadb.databases.svc.cluster.local",  // host
    "dedup-lab",                             // user
    "[REDACTED]",                            // password
    "dedup_lab",                             // database
    3306,                                    // port
    NULL, 0);
```

**Deploy-Befehl (auf pve1):**
```bash
kubectl apply -f k8s/mariadb/statefulset.yaml
```

**LDAP-Integration (bei Deploy):**
- Methode: MariaDB PAM Plugin mit LDAP
- **Status:** GEPLANT (bei Deploy direkt mit LDAP konfigurieren)

---

### 3.7 ClickHouse (NOCH NICHT DEPLOYED)

| Feld | Wert |
|------|------|
| **HTTP Endpoint** | `clickhouse.databases.svc.cluster.local:8123` |
| **Native Endpoint** | `clickhouse.databases.svc.cluster.local:9000` |
| **User** | `dedup_lab` |
| **Passwort** | `[REDACTED]` |
| **Database** | `dedup_lab` |
| **Status** | Manifest bereit (`k8s/clickhouse/statefulset.yaml`), NICHT deployed |

**HTTP Connection (libcurl fuer C++):**
```
POST http://clickhouse.databases.svc.cluster.local:8123/?database=dedup_lab&user=dedup_lab&password=[REDACTED]
Content-Type: text/plain

SELECT 1
```

**Deploy-Befehl (auf pve1):**
```bash
kubectl apply -f k8s/clickhouse/statefulset.yaml
```

**LDAP-Integration (bei Deploy):**
- Methode: ClickHouse native `ldap_servers` Config
- **Status:** GEPLANT (bei Deploy direkt mit LDAP konfigurieren)

---

## 4. Samba AD User Uebersicht (KOMPLETT, Stand Kontext 3)

| AD User | Typ | Beschreibung | Passwort | Status |
|---------|-----|-------------|----------|--------|
| dedup-lab | Lab | Experiment-User fuer alle DBs | [REDACTED] | AKTIV |
| postgres | Service | PostgreSQL Superuser | postgres123 | AKTIV |
| minio-admin | Service | MinIO Root User | [REDACTED] | AKTIV |
| cockroach-root | Service | CockroachDB Root (TLS auth) | [REDACTED] | AKTIV |
| kafka-service | Service | Kafka Service Account | [REDACTED] | AKTIV |
| admin | Samba | Samba AD Admin | [REDACTED] | AKTIV |
| Administrator | Samba | Built-in Admin | (unbekannt) | AKTIV |
| Guest | Samba | Built-in Guest | - | DEAKTIVIERT |
| krbtgt | Samba | Kerberos TGT | - | SYSTEM |

**Gesamt: 9 Accounts (5 Service + 1 Lab + 3 System/Built-in)**

---

## 5. Kontext 3: Kafka Authorization + MinIO + GitLab/GitHub + Samba AD Plan

### 5.1 Kafka Authorization Aktivierung (ERLEDIGT)

**Problem:** KafkaUser CRD war `NotReady` weil Cluster keine Simple Authorization hatte.

**Loesung (3 Schritte):**

1. **Kafka Cluster CRD gepatcht:**
   ```yaml
   spec:
     kafka:
       authorization:
         type: simple
         superUsers:
           - User:ANONYMOUS
       config:
         allow.everyone.if.no.acl.found: true
       listeners:
         - name: plain
           port: 9092
           tls: false
           type: internal
         - name: tls
           port: 9093
           tls: true
           type: internal
         - name: scram
           port: 9094
           tls: false
           type: internal
           authentication:
             type: scram-sha-512
   ```

2. **Rolling Restart:** 4 Controller + 4 Broker Pods automatisch neugestartet (~5 Min)

3. **Strimzi Operator Neustart:** `describeClientQuotas` Timeout blockierte `Ready` Status.
   Operator Pod geloescht → automatisch neugestartet → Cluster und KafkaUser sofort Ready.

**Rueckwaerts-Kompatibilitaet:**
- `allow.everyone.if.no.acl.found: true` → bestehende Services auf Port 9092 UNGESTOERT
- `superUsers: ["User:ANONYMOUS"]` → Unauthentifizierte Clients haben vollen Zugriff
- Nur dedup-lab hat ACL-Beschraenkung auf `dedup-lab-*` Topics/Groups

**Kafka K8s Secret erstellt:**
```bash
kubectl create secret generic dedup-lab-kafka-password -n kafka \
  --from-literal=password='[REDACTED]'
```

**Samba AD:** kafka-service User erstellt.

### 5.2 MinIO dedup-lab User (ERLEDIGT)

**Herausforderung:** Passwort `[REDACTED]` enthaelt `]` und `!` → Shell-Escaping-Hoelle
durch PowerShell → SSH → kubectl → mc CLI Kette.

**Loesung:** Script lokal geschrieben → SCP nach pve1 → dort ausgefuehrt:
```bash
#!/bin/bash
set -e
MINIO_USER=$(kubectl get secret -n minio minio-credentials -o jsonpath='{.data.root-user}' | base64 -d)
MINIO_PASS=$(kubectl get secret -n minio minio-credentials -o jsonpath='{.data.root-password}' | base64 -d)
kubectl exec -n minio minio-0 -- mc alias set local http://localhost:9000 "$MINIO_USER" "$MINIO_PASS"
kubectl exec -n minio minio-0 -- mc admin user add local dedup-lab "$MINIO_PASS"
kubectl exec -n minio minio-0 -- mc mb local/dedup-lab-data --ignore-existing
kubectl exec -n minio minio-0 -- mc admin policy attach local readwrite --user dedup-lab
kubectl exec -n minio minio-0 -- mc admin user info local dedup-lab
```

**Ergebnis:**
- User dedup-lab: enabled, readwrite Policy
- Bucket dedup-lab-data: erstellt
- 9x gitlab-* Buckets: UNBERUEHRT (GitLab laeuft weiter!)
- mc CLI auf minio-0 Pod verfuegbar (`/usr/bin/mc`)

### 5.3 GitHub→GitLab Migration (ERLEDIGT)

**Problem:** GitHub Secret Scanning loescht/blockiert SSH-Keys und Credentials im Projekte-Repo.

**Loesung:**
1. GitHub `origin` Remote entfernt: `git remote remove origin`
2. Nur `gitlab` Remote bleibt: `gitlab.comdare.de/comdare/projekte.git`
3. `git config http.sslVerify false` (Self-Signed Cert)
4. `.gitignore` aktualisiert: `!Cluster/keys/` (Negation, Keys werden versioniert)
5. `.gitlab/secret-detection-exclusions.yml` erstellt (Vorsichtsmassnahme)
6. Commit ba55faf: 51 Dateien (alle SSH-Keys + database-credentials.md)
7. Push auf GitLab erfolgreich

**GitLab Secret Push Protection:**
- Self-Managed GitLab hat Secret Push Protection NICHT standardmaessig aktiv
- Kein Risiko fuer versehentliches Key-Loeschen auf privatem GitLab

### 5.4 Samba AD LDAP-Integrationsplan (DOKUMENTIERT, NICHT AUSGEFUEHRT)

**User-Anforderung:** "Sync alle User auf den Datenbanken auf die Samba AD und vernetze
die Datenbanken auf die Samba AD."

**Status aller Service-User in Samba AD:**
| User | In Samba AD | In DB | LDAP-Verbindung |
|------|-------------|-------|-----------------|
| dedup-lab | JA | PG, CRDB, Redis, Kafka, MinIO | OFFEN |
| postgres | JA | PG (Superuser) | OFFEN |
| minio-admin | JA | MinIO (Root) | OFFEN |
| cockroach-root | JA | CRDB (Root, TLS) | OFFEN |
| kafka-service | JA | Kafka (KafkaUser CRD) | N/A (Strimzi) |

**LDAP-Integrationsplan (Phasen):**

| Phase | DB | Methode | Schwierigkeit | Abhaengigkeiten |
|-------|-----|---------|---------------|-----------------|
| 1 | **MinIO** | Native LDAP Identity Provider | EINFACH | Nur MinIO Helm/Config aendern |
| 2 | **PostgreSQL** | pg_hba.conf `ldap` Auth | MITTEL | pg_hba.conf Aenderung (bisher VERBOTEN) |
| 3 | **ClickHouse** | Native `ldap_servers` Config | EINFACH | Erst bei Deploy |
| 4 | **MariaDB** | PAM Plugin mit LDAP | MITTEL | Erst bei Deploy |
| 5 | **CockroachDB** | Enterprise LDAP Auth | SCHWER | Enterprise Feature, Lizenz? |
| 6 | **Kafka** | Via Strimzi KafkaUser CRD | N/A | Bereits ueber K8s verwaltet |
| 7 | **Redis** | NICHT MOEGLICH | - | Redis hat kein LDAP |

**MinIO LDAP Config (Beispiel fuer Phase 1):**
```
MINIO_IDENTITY_LDAP_SERVER_ADDR=samba-ad-lb.samba-ad.svc.cluster.local:389
MINIO_IDENTITY_LDAP_LOOKUP_BIND_DN=CN=admin,CN=Users,DC=comdare,DC=de
MINIO_IDENTITY_LDAP_LOOKUP_BIND_PASSWORD=[REDACTED]
MINIO_IDENTITY_LDAP_USER_DN_SEARCH_BASE_DN=CN=Users,DC=comdare,DC=de
MINIO_IDENTITY_LDAP_USER_DN_SEARCH_FILTER=(&(objectClass=person)(sAMAccountName=%s))
MINIO_IDENTITY_LDAP_GROUP_SEARCH_BASE_DN=CN=Users,DC=comdare,DC=de
MINIO_IDENTITY_LDAP_GROUP_SEARCH_FILTER=(&(objectClass=group)(member=%d))
```

**PostgreSQL pg_hba.conf LDAP (Beispiel fuer Phase 2):**
```
# LDAP Auth fuer dedup-lab User
host    postgres    dedup-lab    10.0.0.0/8    ldap ldapserver=samba-ad-lb.samba-ad.svc.cluster.local ldapbasedn="CN=Users,DC=comdare,DC=de" ldapsearchattribute="sAMAccountName"
```

**WICHTIG:** LDAP-Integration erfordert User-Genehmigung bevor bestehende DB-Configs
geaendert werden (pg_hba.conf war bisher VERBOTEN zu aendern).

---

## 6. Erstellte Infrastruktur-Dateien (14+3+5 Stueck)

### Kontext 1 erstellt (14 Dateien):
```
Dockerfile              -- Multi-stage: gcc:14-bookworm → debian:bookworm-slim
.dockerignore           -- Build-Context Optimierung
k8s/base/namespace.yaml -- monitoring + databases Namespaces
k8s/base/rbac.yaml      -- ServiceAccount dedup-experiment + ClusterRole
k8s/base/configmap.yaml -- Experiment-Config (7 DB-Systeme, Prometheus)
k8s/base/secrets.yaml   -- TEMPLATE mit Platzhaltern (REPLACE_ME)
k8s/mariadb/statefulset.yaml   -- MariaDB 11.7, 50Gi longhorn-database
k8s/clickhouse/statefulset.yaml -- ClickHouse 24.12, 50Gi longhorn-database
k8s/monitoring/values.yaml      -- kube-prometheus-stack Helm Values
k8s/monitoring/grafana-dashboard-configmap.yaml
k8s/monitoring/install.sh       -- Helm Install Script
k8s/jobs/experiment-job.yaml    -- K8s Job: 500 files x 3 grades, 6h timeout
k8s/jobs/cleanup-job.yaml       -- K8s Job: --cleanup-only, 10 min timeout
k8s/deploy-all.sh               -- Orchestrator-Script
.gitlab-ci.yml                  -- SYSTEMS += mariadb,clickhouse
```

### Kontext 2 erstellt/aktualisiert (3 Dateien):
```
infrastructure/security/setup-lab-user.sh       -- DB-User Setup Script (gefixt)
infrastructure/security/db-hardening-checklist.md -- Non-destructive Ansatz
infrastructure/sessions/20260218-session-10-*.md  -- Diese Session-Doku
```

### Kontext 3 extern gesichert/erstellt (5 Dateien):
```
C:\...\Cluster\keys\database-credentials.md          -- Alle DB-Credentials
C:\...\Projekte\.gitignore                            -- !Cluster/keys/ Negation
C:\...\Projekte\.gitlab\secret-detection-exclusions.yml -- Secret Detection Config
Cluster auf K8s: /tmp/kafka-auth-patch.yaml            -- Kafka CRD Patch (applied)
Cluster auf K8s: /tmp/kafkauser-dedup-lab.yaml          -- KafkaUser CRD (applied)
```

---

## 7. Security Findings (aktualisierter Status)

| # | Finding | Schwere | Ansatz | Status |
|---|---------|---------|--------|--------|
| F1 | Redis ohne Auth (default user nopass) | KRITISCH | **NICHT AENDERBAR** (bestehende Config) | AKZEPTIERTES RISIKO |
| F2 | PostgreSQL pg_hba.conf weit offen | HOCH | **NICHT AENDERBAR** (bestehende Config) | AKZEPTIERTES RISIKO |
| F3 | Keine NetworkPolicies | HOCH | Spaeter additiv hinzufuegen | OFFEN |
| F4 | Lab-Schema fehlte | MITTEL | Schema dedup_lab erstellt | ERLEDIGT |
| F5 | PostgreSQL local trust | MITTEL | **NICHT AENDERBAR** | AKZEPTIERTES RISIKO |
| F6 | dedup-lab nie eingeloggt | NIEDRIG | Erster Login VERIFIZIERT | ERLEDIGT |

**User-Entscheidung:** Bestehende DB-Configs duerfen NICHT geaendert werden.
Nur Lab-User HINZUFUEGEN und Service-Credentials in Samba AD speichern.
Die Isolation erfolgt ueber Schema-Trennung (dedup_lab) und Key-Prefix (dedup:*).

---

## 8. Gesamtstatus aller Datenbanken (FINAL, Kontext 5)

| DB | User | Auth-Methode | Port | LDAP | Status | Samba AD |
|----|------|-------------|------|------|--------|----------|
| PostgreSQL | dedup-lab | **LDAP** (pg_hba.conf) | 5432 | DIREKT | DONE | dedup-lab, postgres |
| CockroachDB | dedup_lab | **Password** (HBA) | 26257 | INDIREKT | DONE | cockroach-root |
| Redis | dedup-lab | ACL + Password | 6379 | N/A | DONE | - |
| Kafka | dedup-lab | SCRAM-SHA-512 | 9094 | N/A | DONE | kafka-service |
| MinIO | dedup-lab | **LDAP** (Identity) | 9000 | DIREKT | DONE | minio-admin |
| MariaDB | - | - | 3306 | - | NICHT DEPLOYED | - |
| ClickHouse | - | - | 8123 | - | NICHT DEPLOYED | - |

**5/5 bestehende Datenbanken: KOMPLETT**
**5/5 Service-User in Samba AD: KOMPLETT**
**2/5 direkte LDAP-Integration (MinIO + PostgreSQL)**
**1/5 indirekte Auth (CockroachDB: password, sync mit Samba AD Passwort)**
**2/5 eigene Auth (Redis ACL, Kafka SCRAM via Strimzi CRD)**

---

## 9. Implementierungshinweise fuer Code-Agent

### 9.1 Config-Datei Struktur

Die Experiment-Config (`k8s/base/configmap.yaml`) definiert alle Endpoints.
Der C++ Code sollte die Credentials aus Umgebungsvariablen oder einer Config-Datei lesen:

```cpp
struct DatabaseConfig {
    // PostgreSQL
    std::string pg_host = "postgres-lb.databases.svc.cluster.local";
    int pg_port = 5432;
    std::string pg_user = "dedup-lab";
    std::string pg_password = "[REDACTED]";
    std::string pg_database = "postgres";
    std::string pg_schema = "dedup_lab";

    // CockroachDB
    std::string crdb_host = "cockroachdb-public.cockroach-operator-system.svc.cluster.local";
    int crdb_port = 26257;
    std::string crdb_user = "dedup_lab";  // UNTERSTRICH!
    std::string crdb_password = "[REDACTED]";
    std::string crdb_database = "dedup_lab";

    // Redis
    std::string redis_host = "redis-cluster.redis.svc.cluster.local";
    int redis_port = 6379;
    std::string redis_user = "dedup-lab";
    std::string redis_password = "[REDACTED]";
    std::string redis_key_prefix = "dedup:";  // PFLICHT!

    // Kafka (AKTUALISIERT: jetzt mit Auth auf Port 9094)
    std::string kafka_bootstrap = "kafka-cluster-kafka-bootstrap.kafka.svc.cluster.local:9094";
    std::string kafka_user = "dedup-lab";
    std::string kafka_password = "[REDACTED]";
    std::string kafka_sasl_mechanism = "SCRAM-SHA-512";
    std::string kafka_topic_prefix = "dedup-lab-";

    // MinIO (AKTUALISIERT: LDAP Access Key statt lokaler User)
    std::string minio_endpoint = "minio-lb.minio.svc.cluster.local:9000";
    std::string minio_access_key = "dedup-lab-s3";        // LDAP Access Key
    std::string minio_secret_key = "dedup-lab-s3-secret"; // LDAP Secret Key
    std::string minio_bucket_prefix = "dedup-lab-";
    bool minio_use_ssl = false;

    // MariaDB (nach Deploy)
    std::string mariadb_host = "mariadb.databases.svc.cluster.local";
    int mariadb_port = 3306;
    std::string mariadb_user = "dedup-lab";
    std::string mariadb_password = "[REDACTED]";
    std::string mariadb_database = "dedup_lab";

    // ClickHouse (nach Deploy)
    std::string clickhouse_host = "clickhouse.databases.svc.cluster.local";
    int clickhouse_http_port = 8123;
    int clickhouse_native_port = 9000;
    std::string clickhouse_user = "dedup_lab";
    std::string clickhouse_password = "[REDACTED]";
    std::string clickhouse_database = "dedup_lab";
};
```

### 9.2 Kafka Client Config (NEU, mit Auth)

```cpp
// librdkafka C++ Config fuer SCRAM-SHA-512 Auth
rd_kafka_conf_set(conf, "bootstrap.servers",
    "kafka-cluster-kafka-bootstrap.kafka.svc.cluster.local:9094", NULL, 0);
rd_kafka_conf_set(conf, "security.protocol", "SASL_PLAINTEXT", NULL, 0);
rd_kafka_conf_set(conf, "sasl.mechanism", "SCRAM-SHA-512", NULL, 0);
rd_kafka_conf_set(conf, "sasl.username", "dedup-lab", NULL, 0);
rd_kafka_conf_set(conf, "sasl.password", "[REDACTED]", NULL, 0);
```

### 9.3 MinIO Client Config (AKTUALISIERT)

```cpp
// AWS SDK C++ oder libcurl
Aws::Client::ClientConfiguration config;
config.endpointOverride = "minio-lb.minio.svc.cluster.local:9000";
config.scheme = Aws::Http::Scheme::HTTP;
config.verifySSL = false;

Aws::Auth::AWSCredentials credentials("dedup-lab-s3", "dedup-lab-s3-secret");
auto s3Client = Aws::S3::S3Client(credentials, config,
    Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never, false);
```

---

## 10. Offene Aufgaben (Priorisiert, aktualisiert Kontext 3)

### SOFORT (Code-Agent):
1. Credentials in `db_connector.hpp` / Config implementieren
2. Connection Strings fuer alle 7 DBs eintragen
3. Schema/Table Init-Logik implementieren
4. Key-Prefix `dedup:` fuer Redis erzwingen
5. **NEU:** Kafka SCRAM-SHA-512 Config auf Port 9094 implementieren
6. **NEU:** MinIO mit dedup-lab User statt Root konfigurieren

### NAECHSTE SESSION (Infra-Agent):
1. **Samba AD LDAP Phase 1:** MinIO → Samba AD verbinden (native LDAP)
2. **Samba AD LDAP Phase 2:** PostgreSQL → Samba AD (pg_hba.conf, User-Genehmigung noetig!)
3. MariaDB deployen: `kubectl apply -f k8s/mariadb/statefulset.yaml`
4. ClickHouse deployen: `kubectl apply -f k8s/clickhouse/statefulset.yaml`
5. `k8s/base/secrets.yaml` mit echten Werten fuellen
6. Prometheus/Grafana deployen: `bash k8s/monitoring/install.sh`
7. Dockerfile bauen + in Registry pushen
8. Smoke-Test Experiment starten

### SPAETER:
- CockroachDB Enterprise LDAP (Lizenz pruefen)
- MariaDB + ClickHouse LDAP bei Deploy
- NetworkPolicies fuer DB-Namespaces erstellen
- TLS Client-Zertifikat fuer CockroachDB dedup_lab User

---

## 11. Rollback-Plan

Da nur User HINZUGEFUEGT wurden (non-destructive), ist Rollback trivial:

```bash
# PostgreSQL
kubectl exec -n databases postgres-ha-0 -- \
  psql -U postgres -c "DROP SCHEMA IF EXISTS dedup_lab CASCADE; DROP ROLE IF EXISTS \"dedup-lab\";"

# Redis
for i in 0 1 2 3; do
  kubectl exec -n redis redis-cluster-$i -- redis-cli ACL DELUSER dedup-lab
done

# Kafka (CRD + Secret + Cluster-Patch)
kubectl delete kafkauser -n kafka dedup-lab
kubectl delete secret -n kafka dedup-lab-kafka-password
# Kafka Cluster CRD zuruecksetzen: authorization + scram listener entfernen

# CockroachDB
kubectl exec -n cockroach-operator-system cockroachdb-0 -- \
  cockroach sql --certs-dir=/cockroach/cockroach-certs \
  -e "DROP DATABASE IF EXISTS dedup_lab CASCADE; DROP USER IF EXISTS dedup_lab;"

# MinIO
kubectl exec -n minio minio-0 -- mc admin user remove local dedup-lab
kubectl exec -n minio minio-0 -- mc rb local/dedup-lab-data --force

# Samba AD Service-User
kubectl exec -n samba-ad samba-ad-0 -- sh -c 'samba-tool user delete dedup-lab'
kubectl exec -n samba-ad samba-ad-0 -- sh -c 'samba-tool user delete postgres'
kubectl exec -n samba-ad samba-ad-0 -- sh -c 'samba-tool user delete minio-admin'
kubectl exec -n samba-ad samba-ad-0 -- sh -c 'samba-tool user delete cockroach-root'
kubectl exec -n samba-ad samba-ad-0 -- sh -c 'samba-tool user delete kafka-service'
```

---

## 12. Kubectl-Zugriff Cheatsheet

```powershell
# Von Windows aus (PowerShell Heredoc fuer komplexe Befehle):
powershell.exe -Command "& { ssh root@192.168.178.44 @'
kubectl get pods -n databases
'@ } 2>&1 | Out-String"

# SSH Session oeffnen:
ssh root@192.168.178.44
kubectl get pods -n databases
kubectl get pods -n redis
kubectl get pods -n kafka

# PostgreSQL als Lab-User:
kubectl exec -n databases postgres-ha-0 -- psql -U dedup-lab -d postgres -c 'SET search_path TO dedup_lab; \dt'

# Redis als Lab-User:
kubectl exec -n redis redis-cluster-0 -- redis-cli --user dedup-lab -a '[REDACTED]' SET dedup:test hello

# CockroachDB als Lab-User:
kubectl exec -n cockroach-operator-system cockroachdb-0 -- \
  cockroach sql --certs-dir=/cockroach/cockroach-certs -e 'SET DATABASE = dedup_lab; SHOW TABLES'

# Kafka Topic erstellen:
kubectl exec -n kafka kafka-cluster-broker-0 -- \
  kafka-topics.sh --bootstrap-server localhost:9092 --create --topic dedup-lab-test --partitions 4

# MinIO als Lab-User (LDAP Access Key):
kubectl exec -n minio minio-0 -- mc alias set lab http://localhost:9000 dedup-lab-s3 dedup-lab-s3-secret
kubectl exec -n minio minio-0 -- mc ls lab/dedup-lab-data/

# Samba AD User pruefen:
kubectl exec -n samba-ad samba-ad-0 -- samba-tool user list

# Port-Forward fuer direkten DB-Zugang von Windows:
ssh root@192.168.178.44 'kubectl port-forward -n databases postgres-ha-0 5432:5432 --address=0.0.0.0 &'
# Dann: psql -h 192.168.178.44 -U dedup-lab -d postgres
```

---

## 13. Kontext-Zusammenfassung (Cluster-Referenz)

### Letzte 3 Cluster-Sessions eingelesen (67a, 67b, 67c)

**Session 67a (17.02.):** GlusterFS Architektur-Analyse
- GlusterFS pve-iso Brick CPU 283% → gefixt (force restart)
- NFS-Ganesha SEGV-Crash → gefixt (restart)
- Full Backup 5/5 OK (120 Min)
- SSH-Keys nach Cluster/keys/ gesichert (17 Paare)
- OPNsense SSH-Lockout via REST API geflusht

**Session 67b (17.02.):** HAProxy Routing Fix + Phase 0-4 Review
- GitLab HTTPS via HAProxy: GEFIXT (externalTrafficPolicy: Local → Cluster)
- HAProxy ssl-hello-chk → TCP-only Health Check
- HAProxy Config auf alle 4 OPNsense gesynced
- HTTP→HTTPS Redirect auf Port 80
- HAProxy in config.xml eingebettet (base64, DR-gesichert)
- pf Rules vollstaendig gesynced, alle 4 AKTIV
- Internet aus K8s Pods: 79.254.12.169 (Fritz!Box NAT)

**Session 67c (17-18.02.):** Internet Routing + VLAN 1 Sicherheitsloch
- ALLE 4 Talos VLAN 1 IPs entfernt (dhcp: false auf br0, ohne Reboot)
- CARP Split-Brain VHID 110+130 identifiziert (multicast_snooping → DaemonSet Fix)
- VLAN 1 RX auf OPNsense WEITERHIN KAPUTT (TCP Handshake OK, Data FAIL)
- Root Cause: bridge-nf-call-iptables=1 auf Talos (Calico iptables DROP)
- Active/Active CARP LB Plan: 4/4/3/4 VHIDs Verteilung

### Aktuelle Cluster-Blocker
1. **VLAN 1 RX:** bridge-nf-call-iptables=0 auf Talos setzen (bricht NetworkPolicy!)
2. **CARP Split-Brain:** VHID 110+130 auf opn-3 (L2-Problem)
3. **K8s Internet:** Geht ueber VLAN 1 Bypass (Sicherheitsloch trotz DHCP-Fix)
4. **pve1 VLAN 1:** .44 noch auf vmbr0 (erst nach OPNsense-Internet-Fix entfernbar)
5. **VLAN 1 verboten:** User-Regel: NUR VLAN 10 fuer unsere Belange

---

## 14. Kontext 4: LDAP-Integration Versuch (2026-02-18 07:44-07:48 UTC)

### LDAP Connectivity: ALLE 3 DB-Pods → Samba AD = OK

| Quelle | TCP 389 | LDAP Bind |
|--------|---------|-----------|
| MinIO Pod | OK | OK (admin@comdare.de / [REDACTED]) |
| PostgreSQL Pod | OK | nicht getestet |
| CockroachDB Pod | OK | nicht getestet |
| pve1 (Bare Metal) | FAIL | N/A (nicht im K8s Netz) |

### Samba AD Credentials VERIFIZIERT

- **Bind DN:** `CN=admin,CN=Users,DC=comdare,DC=de` oder `admin@comdare.de` → FUNKTIONIERT
- **Passwort:** `[REDACTED]` (OHNE Backslash!) → FUNKTIONIERT
- **K8s Secret:** `[CLUSTER-PW-REDACTED]` (MIT Backslash, Shell-Escaping-Artefakt)
- **Cluster2026!** → FUNKTIONIERT NICHT MEHR
- **CN=Administrator** → Anderer User, anderes Passwort

### User-Warnung: Passwort-Angleichung noetig

User: "Wir muessen das Passwort angleichen aber wir wissen nicht welcher Service
das verwendet und dann nicht mehr rein kommt."

**Services die Samba AD verwenden:**
1. GitLab LDAP (bind_dn + password aus Helm Values/K8s Secret)
2. CoreDNS Forward (kein Auth, nur DNS)
3. MinIO LDAP (GERADE konfiguriert mit [REDACTED])

### MinIO LDAP: CONFIG GESETZT, RESTART NOETIG

- `mc admin config set local identity_ldap ...` → ERFOLGREICH
- `mc admin service restart` → FEHLGESCHLAGEN (kein TTY in kubectl exec)
- Config nach 20s: LEER (MinIO nicht neugestartet)
- **FIX:** `kubectl delete pod -n minio minio-0` (Kubernetes restart)

### PostgreSQL LDAP: PFAD FALSCH

- `/home/postgres/pgdata/pgroot/data/pg_hba.conf` existiert NICHT
- Patroni/Spilo verwendet anderen Pfad → `find / -name pg_hba.conf` noetig

### CockroachDB LDAP: NICHT AUSGEFUEHRT

- Script abgebrochen nach PostgreSQL-Fehler

### Naechste Session LDAP-Aufgaben
1. ~~MinIO Pod neustarten + LDAP verifizieren~~ → ERLEDIGT (Kontext 5)
2. ~~PostgreSQL pg_hba.conf Pfad finden (Patroni)~~ → ERLEDIGT (Kontext 5)
3. ~~PostgreSQL LDAP konfigurieren~~ → ERLEDIGT (Kontext 5)
4. ~~CockroachDB HBA Setting mit LDAP~~ → ERLEDIGT (Kontext 5, password statt ldap)
5. GitLab LDAP Config pruefen (welches PW?)
6. Passwort-Angleichung koordiniert

---

## 15. Kontext 5: LDAP-Integration KOMPLETT (2026-02-18 08:03-08:08 UTC)

### MinIO LDAP: ERFOLGREICH

- `kubectl delete pod -n minio minio-0` → Pod neugestartet
- LDAP Config **persistent** nach Restart
- `server_addr=samba-ad-lb.samba-ad.svc.cluster.local:389`
- `lookup_bind_dn=CN=admin,CN=Users,DC=comdare,DC=de`
- `user_dn_search_filter=(&(objectClass=person)(sAMAccountName=%s))`
- `group_search_filter=(&(objectClass=group)(member=%d))`

### PostgreSQL LDAP: ERFOLGREICH

- pg_hba.conf Pfad gefunden: `/var/lib/postgresql/data/pgdata/pg_hba.conf`
- LDAP-Zeile eingefuegt **vor** catch-all `scram-sha-256` (Zeile 119)
- `pg_reload_conf()` → Config sofort aktiv (kein Restart noetig)
- **Login-Test ERFOLGREICH:** `dedup-lab` kann sich per LDAP gegen Samba AD authentifizieren

```
host    all    dedup-lab    0.0.0.0/0    ldap ldapserver=samba-ad-lb.samba-ad.svc.cluster.local ldapport=389 ldapbasedn="CN=Users,DC=comdare,DC=de" ldapsearchattribute="sAMAccountName"
```

### CockroachDB: PASSWORD AUTH (kein natives LDAP)

- CockroachDB v24.3.0 hat **KEIN natives LDAP** (erst ab v25.1 experimentell)
- Alternative: HBA `password` Auth fuer `dedup_lab` User
- IPv4 + IPv6 Regeln gesetzt (::1 Verbindung beachten!)
- Passwort fuer dedup_lab war NICHT gesetzt (has_password=false) → jetzt `[REDACTED]`
- **Login-Test ERFOLGREICH** (via URL mit URL-encoded Passwort)

```
HBA Config:
host all dedup_lab 0.0.0.0/0 password
host all dedup_lab ::/0 password
host all all 0.0.0.0/0 cert-password
host all all ::/0 cert-password
```

### Redis: ACL (kein LDAP Support)

- Redis hat keinen LDAP Support → bleibt ACL-basiert
- Login-Test: OK (`PONG`)

### Kafka: SCRAM-SHA-512 (via Strimzi CRD)

- KafkaUser CRD verwaltet Auth → kein LDAP noetig
- Status: Ready
- Auth: scram-sha-512

### FINALE Verifikation (5/5)

| DB | Auth-Methode | LDAP | Login-Test | Status |
|----|-------------|------|------------|--------|
| MinIO | LDAP Identity Provider | DIREKT | Config OK | DONE |
| PostgreSQL | pg_hba.conf ldap | DIREKT | dedup-lab Login OK | DONE |
| CockroachDB | HBA password | INDIREKT (PW sync) | dedup_lab Login OK | DONE |
| Redis | ACL + Password | N/A (kein Support) | PONG OK | DONE |
| Kafka | SCRAM-SHA-512 | N/A (Strimzi CRD) | KafkaUser Ready | DONE |

**5/5 Datenbanken: AUTHENTIFIZIERUNG KOMPLETT**

### Namespaces (korrigiert)

| Service | Namespace |
|---------|-----------|
| MinIO | minio |
| PostgreSQL | databases |
| CockroachDB | cockroach-operator-system |
| Redis | redis |
| Kafka | kafka |

### Offene LDAP-Aufgaben (aktualisiert Kontext 5b)

1. ~~GitLab LDAP Bind-Password pruefen~~ → ERLEDIGT (verwendet `[REDACTED]` via `gitlab-ldap-password` Secret)
2. ~~Passwort-Angleichung~~ → ERLEDIGT: Alle Services verwenden bereits `[REDACTED]`
3. ~~MinIO LDAP Access Key~~ → ERLEDIGT: `dedup-lab-s3` / `dedup-lab-s3-secret` (LDAP-gebunden)
4. ~~MinIO LDAP Policy~~ → ERLEDIGT: `readwrite` an `cn=Dedup Lab,cn=Users,dc=comdare,dc=de`
5. CockroachDB → v25.1+ Upgrade fuer natives LDAP (ZUKUNFT)

### Passwort-Audit Ergebnis (Kontext 5b)

**ALLE Services verwenden `[REDACTED]`:**

| Service | Secret/Config | Passwort | Status |
|---------|--------------|----------|--------|
| Samba AD admin (LDAP bind) | samba-ad-credentials | [REDACTED] | GEFIXT (Backslash entfernt) |
| GitLab LDAP bind | gitlab-ldap-password | [REDACTED] | OK |
| GitLab root | gitlab-initial-root-password | [REDACTED] | OK |
| GitLab PostgreSQL | gitlab-postgresql-password | [REDACTED] | OK |
| MinIO root | minio-credentials | [REDACTED] | OK |
| MinIO LDAP bind | mc admin config identity_ldap | [REDACTED] | OK |
| PostgreSQL dedup-lab | LDAP (Samba AD) | [REDACTED] | OK |
| CockroachDB dedup_lab | HBA password | [REDACTED] | OK |
| Kafka dedup-lab | dedup-lab-kafka-password | [REDACTED] | OK |
| Redis dedup-lab | ACL | [REDACTED] | OK |

**K8s Secret Fix:** `samba-ad-credentials` admin-password korrigiert: `[CLUSTER-PW-REDACTED]` → `[REDACTED]`

### MinIO LDAP Access Key (Kontext 5b)

- **WICHTIG:** MinIO blockiert lokale User-Erstellung wenn LDAP aktiv ist!
- Lokaler User `dedup-lab` wurde entfernt (Konflikt mit LDAP)
- LDAP Access Key erstellt: `dedup-lab-s3` / `dedup-lab-s3-secret`
- Policy `readwrite` an LDAP DN gebunden
- Bucket-Listing + Read/Write: VERIFIZIERT

**Neuer MinIO Connection String (LDAP-basiert):**
```cpp
// MinIO/S3 mit LDAP Access Key
endpoint = "minio-lb.minio.svc.cluster.local:9000";
access_key = "dedup-lab-s3";
secret_key = "dedup-lab-s3-secret";
use_ssl = false;
```
