# Database Security Hardening Checklist
**Zweck:** dedup-lab User auf allen Datenbanken einrichten (NUR additiv).
**Kundendaten:** PostgreSQL (GitLab), Redis (GitLab Cache), Kafka (Prod Topics)
**Ansatz:** NON-DESTRUCTIVE - Keine bestehende Config wird geaendert!

---

## Regeln

1. **KEINE REVOKEs** - Bestehende Berechtigungen bleiben unangetastet
2. **KEINE pg_hba.conf Aenderungen** - Bestehende Auth-Config bleibt
3. **KEINE ACL-Aenderungen an bestehenden Usern** - Nur NEUE User hinzufuegen
4. **KEINE NetworkPolicy-Aenderungen** - Bestehende Policies bleiben
5. **Service-User in Samba AD speichern** - Fuer spaetere LDAP-Migration

---

## Reihenfolge (nach Downtime-Impact sortiert)

### Phase 0: Voraussetzungen (0 Downtime)
- [x] Samba AD User `dedup-lab` existiert und ist aktiv
- [x] kubectl Zugriff via `ssh root@192.168.178.44`
- [ ] Alle DB-Pods Running verifiziert

### Phase A: PostgreSQL - Lab User hinzufuegen (0 Downtime)
- [ ] `CREATE ROLE "dedup-lab" LOGIN PASSWORD '...'`
- [ ] `CREATE SCHEMA IF NOT EXISTS dedup_lab AUTHORIZATION "dedup-lab"`
- [ ] `GRANT CONNECT ON DATABASE postgres TO "dedup-lab"`
- [ ] `GRANT USAGE, CREATE ON SCHEMA dedup_lab TO "dedup-lab"`
- [ ] Postgres Service-User Passwort in Samba AD speichern
- [ ] Verifizieren: dedup-lab kann auf dedup_lab Schema zugreifen
- [ ] Verifizieren: GitLab funktioniert weiterhin normal

### Phase B: Redis - Lab ACL User hinzufuegen (0 Downtime)
- [ ] `ACL SETUSER dedup-lab on >password ~dedup:* +@all`
- [ ] Auf alle 4 Cluster-Nodes propagieren
- [ ] Verifizieren: dedup-lab kann dedup:* Keys setzen
- [ ] Verifizieren: Bestehende Services unberuehrt

### Phase C: Kafka - KafkaUser CRD erstellen (0 Downtime)
- [ ] Strimzi KafkaUser CRD fuer dedup-lab erstellen
- [ ] ACL: NUR dedup-lab-* Topic Prefix erlaubt
- [ ] Strimzi provisioniert User automatisch

### Phase D: MinIO - Lab User via mc CLI (0 Downtime)
- [ ] MinIO Root Credentials aus K8s Secret lesen
- [ ] MinIO Root User in Samba AD speichern
- [ ] dedup-lab User via mc CLI oder MinIO Console erstellen
- [ ] Bucket Policy: NUR dedup-lab-* Buckets

### Phase E: CockroachDB - Lab User hinzufuegen (0 Downtime)
- [ ] `CREATE USER IF NOT EXISTS dedup_lab WITH PASSWORD '...'`
- [ ] `CREATE DATABASE IF NOT EXISTS dedup_lab`
- [ ] `GRANT ALL ON DATABASE dedup_lab TO dedup_lab`
- [ ] CockroachDB Root User in Samba AD speichern

### Phase F: MariaDB + ClickHouse deployen (0 Downtime, neue DBs)
- [ ] MariaDB deployen mit Lab-User im Init-SQL
- [ ] ClickHouse deployen mit Default-User
- [ ] Beide nur ueber Lab-Schemas erreichbar

---

## Verifizierungs-Tests

```bash
# 1. dedup-lab kann auf dedup_lab Schema zugreifen
kubectl exec -n databases postgres-ha-0 -- \
  psql -U dedup-lab -d postgres -c 'CREATE TABLE dedup_lab.test(id int); DROP TABLE dedup_lab.test;'

# 2. Redis dedup-lab kann dedup:* Keys setzen
kubectl exec -n redis redis-cluster-0 -- \
  redis-cli --user dedup-lab --pass "$DEDUP_LAB_PASSWORD" SET dedup:test hello

# 3. Kafka dedup-lab Topics erstellen (via Strimzi KafkaUser ACL)
# Automatisch durch Strimzi enforced

# 4. CockroachDB dedup_lab kann auf dedup_lab DB zugreifen
kubectl exec -n cockroach-operator-system cockroachdb-0 -- \
  cockroach sql --certs-dir=/cockroach/cockroach-certs \
  -e "SET DATABASE = dedup_lab; CREATE TABLE test(id INT); DROP TABLE test;"
```

---

## Rollback-Plan

Da nur User HINZUGEFUEGT werden, ist Rollback trivial:

```bash
# PostgreSQL: Lab User entfernen
kubectl exec -n databases postgres-ha-0 -- \
  psql -U postgres -c "DROP SCHEMA IF EXISTS dedup_lab CASCADE; DROP ROLE IF EXISTS \"dedup-lab\";"

# Redis: Lab ACL User entfernen
kubectl exec -n redis redis-cluster-0 -- redis-cli ACL DELUSER dedup-lab

# Kafka: KafkaUser CRD loeschen (Strimzi raeumt auf)
kubectl delete kafkauser -n kafka dedup-lab

# CockroachDB: Lab User + DB entfernen
kubectl exec -n cockroach-operator-system cockroachdb-0 -- \
  cockroach sql --certs-dir=/cockroach/cockroach-certs \
  -e "DROP DATABASE IF EXISTS dedup_lab CASCADE; DROP USER IF EXISTS dedup_lab;"

# Samba AD: Service-User entfernen (optional)
kubectl exec -n samba-ad samba-ad-0 -- sh -c 'samba-tool user delete postgres'
kubectl exec -n samba-ad samba-ad-0 -- sh -c 'samba-tool user delete minio-admin'
kubectl exec -n samba-ad samba-ad-0 -- sh -c 'samba-tool user delete cockroach-root'
```

---

## Ausfuehrung

Script: `infrastructure/security/setup-lab-user.sh`

```bash
# Auf pve1 kopieren und ausfuehren:
scp setup-lab-user.sh root@192.168.178.44:/tmp/
ssh root@192.168.178.44 'bash /tmp/setup-lab-user.sh'
```
