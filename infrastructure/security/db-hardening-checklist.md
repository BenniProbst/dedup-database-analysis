# Database Security Hardening Checklist
**Zweck:** Sicherstellen, dass der dedup-lab User NUR auf Lab-Schemas zugreifen kann.
**Kundendaten:** PostgreSQL (GitLab), Redis (GitLab Cache), Kafka (Prod Topics)

---

## Reihenfolge (nach Downtime-Impact sortiert)

### Phase 0: Voraussetzungen (0 Downtime)
- [ ] Samba AD User `dedup-lab` existiert und ist aktiv
- [ ] kubectl Zugriff via `ssh root@192.168.178.44`
- [ ] Backup aller DB-Configs VOR Änderungen

### Phase A: PostgreSQL absichern (~30s Downtime)
- [ ] `CREATE SCHEMA dedup_lab` auf postgres DB
- [ ] `GRANT USAGE, CREATE ON SCHEMA dedup_lab TO "dedup-lab"`
- [ ] `REVOKE CONNECT ON DATABASE gitlabhq_production FROM "dedup-lab"`
- [ ] `REVOKE CONNECT ON DATABASE praefect FROM "dedup-lab"`
- [ ] `REVOKE ALL ON SCHEMA public FROM "dedup-lab"`
- [ ] pg_hba.conf: dedup-lab NUR auf dedup_lab DB beschränken
- [ ] `SELECT pg_reload_conf();`
- [ ] Verifizieren: dedup-lab kann NICHT auf gitlabhq_production zugreifen
- [ ] Verifizieren: GitLab funktioniert weiterhin normal

### Phase B: Redis absichern (~30s Downtime)
- [ ] Redis ACL: `dedup-lab` User mit `~dedup:*` Key-Restriction
- [ ] Redis ACL: `default` User mit Passwort versehen
- [ ] GitLab Redis Secret aktualisieren
- [ ] GitLab Redis Pods rollout restart
- [ ] Verifizieren: GitLab Sessions funktionieren
- [ ] Verifizieren: dedup-lab kann NUR dedup:* Keys verwenden

### Phase C: Kafka Topic-ACLs (0 Downtime)
- [ ] Strimzi KafkaUser CRD für dedup-lab erstellen
- [ ] ACL: NUR dedup-lab-* Topic Prefix erlaubt
- [ ] Verifizieren: dedup-lab kann NICHT auf Prod-Topics zugreifen

### Phase D: MinIO Bucket Policy (0 Downtime)
- [ ] IAM Policy: NUR dedup-lab-* Buckets erlaubt
- [ ] Policy an dedup-lab User binden
- [ ] Verifizieren: dedup-lab kann NICHT auf Prod-Buckets zugreifen

### Phase E: CockroachDB (bereits sicher)
- [ ] TLS Mode aktiv (verifiziert)
- [ ] Client-Zertifikat für dedup-lab erstellen
- [ ] GRANT nur auf dedup_lab Database

### Phase F: NetworkPolicies (0 Downtime)
- [ ] databases Namespace: Experiment-Pods nur auf DB-Ports
- [ ] redis Namespace: Nur autorisierte Pods
- [ ] kafka Namespace: Erweitern (bereits teilweise vorhanden)
- [ ] minio Namespace: Nur autorisierte Pods
- [ ] Verifizieren: Alle bestehenden Services funktionieren noch

### Phase G: MariaDB + ClickHouse (neu, 0 Downtime)
- [ ] MariaDB deployen mit Lab-User im Init-SQL
- [ ] ClickHouse deployen mit Default-User (kein Kundendaten-Risiko)
- [ ] Beide nur über Lab-Schemas erreichbar

---

## Verifizierungs-Tests

```bash
# 1. dedup-lab kann auf dedup_lab Schema zugreifen
kubectl exec -n databases postgres-ha-0 -- \
  psql -U dedup-lab -d postgres -c 'CREATE TABLE dedup_lab.test(id int); DROP TABLE dedup_lab.test;'

# 2. dedup-lab kann NICHT auf GitLab DB zugreifen
kubectl exec -n databases postgres-ha-0 -- \
  psql -U dedup-lab -d gitlabhq_production -c 'SELECT 1'
# ERWARTET: FATAL: permission denied

# 3. Redis dedup-lab kann NUR dedup:* Keys
kubectl exec -n redis redis-cluster-0 -- \
  redis-cli -u dedup-lab SET dedup:test hello  # ERWARTET: OK
kubectl exec -n redis redis-cluster-0 -- \
  redis-cli -u dedup-lab SET gitlab:test hello  # ERWARTET: NOPERM

# 4. Kafka dedup-lab kann NUR dedup-lab-* Topics
# (via Strimzi KafkaUser ACL enforcement)
```

---

## Rollback-Plan

Bei Problemen nach Security-Änderungen:

```bash
# PostgreSQL: pg_hba.conf zurücksetzen
kubectl exec -n databases postgres-ha-0 -- psql -U postgres -c "SELECT pg_reload_conf();"

# Redis: ACL zurücksetzen
kubectl exec -n redis redis-cluster-0 -- redis-cli ACL SETUSER default on nopass ~* +@all

# NetworkPolicies: Löschen (erlaubt wieder alles)
kubectl delete networkpolicy -n databases --all
```
