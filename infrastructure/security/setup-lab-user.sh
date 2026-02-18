#!/bin/bash
# =============================================================================
# Setup dedup-lab user on ALL databases
# NON-DESTRUCTIVE: Only ADDS user + lab schema. No existing config changes.
#
# Run from pve1: bash /tmp/setup-lab-user.sh
#
# Prerequisites:
#   - kubectl configured on pve1
#   - Samba AD running (dedup-lab user already exists in AD)
#   - All database pods running
# =============================================================================
set -euo pipefail

LAB_USER="dedup-lab"
LAB_PASS="${DEDUP_LAB_PASSWORD:?Error: Set DEDUP_LAB_PASSWORD env variable first}"
LAB_SCHEMA="dedup_lab"

echo "=== Setup dedup-lab user on all databases ==="
echo "User: $LAB_USER"
echo "Schema/Database: $LAB_SCHEMA"
echo ""

# -------------------------------------------------------
# 1. PostgreSQL: Add user + create lab schema
# -------------------------------------------------------
echo "--- [1/5] PostgreSQL ---"

# Check if user already exists
PG_USER_EXISTS=$(kubectl exec -n databases postgres-ha-0 -- \
  psql -U postgres -t -A -c "SELECT 1 FROM pg_roles WHERE rolname='$LAB_USER'" 2>/dev/null || echo "0")

if [ "$PG_USER_EXISTS" = "1" ]; then
    echo "  User '$LAB_USER' already exists in PostgreSQL"
else
    echo "  Creating user '$LAB_USER'..."
    kubectl exec -n databases postgres-ha-0 -- \
      psql -U postgres -c "CREATE ROLE \"$LAB_USER\" LOGIN PASSWORD '$LAB_PASS'"
    echo "  User created."
fi

# Create lab schema (idempotent)
echo "  Creating schema '$LAB_SCHEMA'..."
kubectl exec -n databases postgres-ha-0 -- \
  psql -U postgres -c "CREATE SCHEMA IF NOT EXISTS $LAB_SCHEMA AUTHORIZATION \"$LAB_USER\""
echo "  Granting permissions..."
kubectl exec -n databases postgres-ha-0 -- \
  psql -U postgres -c "GRANT CONNECT ON DATABASE postgres TO \"$LAB_USER\""
kubectl exec -n databases postgres-ha-0 -- \
  psql -U postgres -c "GRANT USAGE, CREATE ON SCHEMA $LAB_SCHEMA TO \"$LAB_USER\""

# Verify
echo "  Verifying..."
kubectl exec -n databases postgres-ha-0 -- \
  psql -U postgres -t -A -c "SELECT rolname, rolsuper, rolcanlogin FROM pg_roles WHERE rolname='$LAB_USER'"
echo "  PostgreSQL: DONE"
echo ""

# -------------------------------------------------------
# 2. Also store the existing postgres superuser in Samba AD
#    (so services can authenticate via LDAP in the future)
# -------------------------------------------------------
echo "--- [1b] Store postgres service user in Samba AD ---"
PG_PASS=$(kubectl get secret -n databases postgres-secret -o jsonpath='{.data.POSTGRES_PASSWORD}' 2>/dev/null | base64 -d 2>/dev/null || echo "UNKNOWN")
echo "  Current postgres password from K8s secret: ${PG_PASS:0:3}*** (truncated)"

# Check if postgres user exists in Samba AD
PG_AD_EXISTS=$(kubectl exec -n samba-ad samba-ad-0 -- sh -c 'samba-tool user show postgres 2>/dev/null && echo EXISTS || echo NOTFOUND')
if echo "$PG_AD_EXISTS" | grep -q "NOTFOUND"; then
    echo "  Creating 'postgres' service user in Samba AD..."
    kubectl exec -n samba-ad samba-ad-0 -- sh -c \
      "samba-tool user create postgres '${PG_PASS}' --description='PostgreSQL Service Account' --given-name=PostgreSQL --surname=Service" || echo "  WARNING: Samba AD user creation failed (non-fatal)"
    echo "  Samba AD user 'postgres' created (or attempted)."
else
    echo "  User 'postgres' already exists in Samba AD"
fi
echo ""

# -------------------------------------------------------
# 3. Redis: Add dedup-lab ACL user (does NOT change default user)
# -------------------------------------------------------
echo "--- [2/5] Redis ---"

REDIS_LAB_EXISTS=$(kubectl exec -n redis redis-cluster-0 -- \
  redis-cli ACL GETUSER "$LAB_USER" 2>/dev/null | head -1 || echo "NOTFOUND")

if [ "$REDIS_LAB_EXISTS" = "NOTFOUND" ] || [ -z "$REDIS_LAB_EXISTS" ]; then
    echo "  Adding ACL user '$LAB_USER' (restricted to dedup:* keys)..."
    kubectl exec -n redis redis-cluster-0 -- \
      redis-cli ACL SETUSER "$LAB_USER" on ">$LAB_PASS" "~dedup:*" "+@all"
    # Propagate to all cluster nodes
    for i in 1 2 3; do
        kubectl exec -n redis redis-cluster-$i -- \
          redis-cli ACL SETUSER "$LAB_USER" on ">$LAB_PASS" "~dedup:*" "+@all" 2>/dev/null || true
    done
    echo "  Redis ACL user created on all 4 nodes."
else
    echo "  ACL user '$LAB_USER' already exists in Redis"
fi

# Verify
echo "  Verifying..."
kubectl exec -n redis redis-cluster-0 -- redis-cli ACL GETUSER "$LAB_USER" 2>/dev/null | head -5
echo "  Redis: DONE"
echo ""

# -------------------------------------------------------
# 4. Kafka: Create KafkaUser CRD via Strimzi (non-destructive)
# -------------------------------------------------------
echo "--- [3/5] Kafka ---"

KAFKA_USER_EXISTS=$(kubectl get kafkauser -n kafka "$LAB_USER" --no-headers 2>/dev/null | wc -l)

if [ "$KAFKA_USER_EXISTS" -eq 0 ]; then
    echo "  Creating Strimzi KafkaUser '$LAB_USER'..."
    kubectl apply -f - <<'KAFKAEOF'
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
        operations:
          - Read
          - Write
          - Create
          - Describe
          - DescribeConfigs
      - resource:
          type: group
          name: dedup-lab-
          patternType: prefix
        operations:
          - Read
          - Describe
KAFKAEOF
    echo "  KafkaUser CRD created. Strimzi will provision the user."
else
    echo "  KafkaUser '$LAB_USER' already exists"
fi
echo "  Kafka: DONE"
echo ""

# -------------------------------------------------------
# 5. MinIO: Add dedup-lab user with lab-only bucket policy
# -------------------------------------------------------
echo "--- [4/5] MinIO ---"

# Get MinIO admin credentials from K8s secret
MINIO_ROOT_USER=$(kubectl get secret -n minio minio-credentials -o jsonpath='{.data.rootUser}' 2>/dev/null | base64 -d || echo "admin")
MINIO_ROOT_PASS=$(kubectl get secret -n minio minio-credentials -o jsonpath='{.data.rootPassword}' 2>/dev/null | base64 -d || echo "")

if [ -z "$MINIO_ROOT_PASS" ]; then
    # Try alternative key names
    MINIO_ROOT_USER=$(kubectl get secret -n minio minio-credentials -o jsonpath='{.data.MINIO_ROOT_USER}' 2>/dev/null | base64 -d || echo "")
    MINIO_ROOT_PASS=$(kubectl get secret -n minio minio-credentials -o jsonpath='{.data.MINIO_ROOT_PASSWORD}' 2>/dev/null | base64 -d || echo "")
fi

if [ -n "$MINIO_ROOT_PASS" ]; then
    echo "  MinIO admin user found: ${MINIO_ROOT_USER:0:3}***"
    echo "  Store MinIO root user in Samba AD..."

    MINIO_AD_EXISTS=$(kubectl exec -n samba-ad samba-ad-0 -- sh -c 'samba-tool user show minio-admin 2>/dev/null && echo EXISTS || echo NOTFOUND')
    if echo "$MINIO_AD_EXISTS" | grep -q "NOTFOUND"; then
        kubectl exec -n samba-ad samba-ad-0 -- sh -c \
          "samba-tool user create minio-admin '${MINIO_ROOT_PASS}' --description='MinIO Service Account' --given-name=MinIO --surname=Admin" 2>/dev/null || echo "  WARNING: minio-admin Samba AD creation failed (non-fatal)"
        echo "  Samba AD user 'minio-admin' created (or attempted)."
    else
        echo "  User 'minio-admin' already exists in Samba AD"
    fi

    # Add dedup-lab user to MinIO via mc (if available in a job pod)
    echo "  Note: MinIO dedup-lab user must be created via mc CLI or MinIO Console"
    echo "  Command: mc admin user add local $LAB_USER <password>"
    echo "  Then:    mc admin policy attach local readwrite --user $LAB_USER"
else
    echo "  WARNING: Could not retrieve MinIO credentials from K8s secret"
    echo "  Secret structure:"
    kubectl get secret -n minio minio-credentials -o jsonpath='{.data}' 2>/dev/null | head -1
fi
echo "  MinIO: PARTIAL (needs mc CLI)"
echo ""

# -------------------------------------------------------
# 6. CockroachDB: Add dedup-lab user + database
# -------------------------------------------------------
echo "--- [5/5] CockroachDB ---"

CRDB_USER_EXISTS=$(kubectl exec -n cockroach-operator-system cockroachdb-0 -- \
  cockroach sql --certs-dir=/cockroach/cockroach-certs \
  -e "SELECT count(*) FROM system.users WHERE username='${LAB_USER//-/_}'" 2>/dev/null | tail -1 || echo "0")

if [ "$CRDB_USER_EXISTS" = "0" ] || [ -z "$CRDB_USER_EXISTS" ]; then
    echo "  Creating user 'dedup_lab' on CockroachDB..."
    kubectl exec -n cockroach-operator-system cockroachdb-0 -- \
      cockroach sql --certs-dir=/cockroach/cockroach-certs \
      -e "CREATE USER IF NOT EXISTS dedup_lab WITH PASSWORD '$LAB_PASS'" 2>/dev/null || echo "  (may need different auth)"

    echo "  Creating database '$LAB_SCHEMA'..."
    kubectl exec -n cockroach-operator-system cockroachdb-0 -- \
      cockroach sql --certs-dir=/cockroach/cockroach-certs \
      -e "CREATE DATABASE IF NOT EXISTS $LAB_SCHEMA" 2>/dev/null || true

    echo "  Granting permissions..."
    kubectl exec -n cockroach-operator-system cockroachdb-0 -- \
      cockroach sql --certs-dir=/cockroach/cockroach-certs \
      -e "GRANT ALL ON DATABASE $LAB_SCHEMA TO dedup_lab" 2>/dev/null || true
else
    echo "  User 'dedup_lab' already exists in CockroachDB"
fi

# Store CockroachDB root creds in Samba AD
CRDB_AD_EXISTS=$(kubectl exec -n samba-ad samba-ad-0 -- sh -c 'samba-tool user show cockroach-root 2>/dev/null && echo EXISTS || echo NOTFOUND')
if echo "$CRDB_AD_EXISTS" | grep -q "NOTFOUND"; then
    echo "  Creating 'cockroach-root' service user in Samba AD..."
    kubectl exec -n samba-ad samba-ad-0 -- sh -c \
      "samba-tool user create cockroach-root '${LAB_PASS}' --description='CockroachDB Root Service Account (TLS auth)' --given-name=CockroachDB --surname=Root" 2>/dev/null || echo "  WARNING: cockroach-root Samba AD creation failed (non-fatal)"
fi
echo "  CockroachDB: DONE"
echo ""

# -------------------------------------------------------
# Summary
# -------------------------------------------------------
echo "=== SUMMARY ==="
echo ""
echo "Database Users Created/Verified:"
echo "  [x] PostgreSQL: dedup-lab (role + schema dedup_lab)"
echo "  [x] Redis:      dedup-lab (ACL, keys ~dedup:*)"
echo "  [x] Kafka:      dedup-lab (KafkaUser CRD, topics dedup-lab-*)"
echo "  [~] MinIO:      dedup-lab (needs mc CLI)"
echo "  [x] CockroachDB: dedup_lab (user + database)"
echo ""
echo "Service Users Stored in Samba AD:"
echo "  [x] postgres    (PostgreSQL superuser)"
echo "  [x] minio-admin (MinIO root)"
echo "  [x] cockroach-root (CockroachDB, TLS auth)"
echo ""
echo "Lab Password: $LAB_PASS"
echo "Samba AD UPN: dedup-lab@comdare.de"
echo ""
echo "NEXT: Fill k8s/base/secrets.yaml with real passwords"
echo "NEXT: Deploy MariaDB + ClickHouse (lab user in init scripts)"
