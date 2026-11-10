#!/usr/bin/env bash
# Starts Oracle Database Free in a container for the lab, creates the lab
# user, and seeds its schema. Writes lab/.lab.env with the connection details;
# lab/orkidea-lab.sh reads it.
#
#   lab/setup.sh                 # port 1531
#   ORKIDEA_LAB_PORT=1600 lab/setup.sh
#
# Needs Docker (or Podman as `docker`) and about 2 GB of memory for Oracle.
# The image is pulled from Docker Hub on first use; it is not redistributed.
set -euo pipefail

lab="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
container="${ORKIDEA_LAB_CONTAINER:-orkidea-lab}"
image="${ORKIDEA_LAB_IMAGE:-gvenzl/oracle-free:23-slim-faststart}"
port="${ORKIDEA_LAB_PORT:-1531}"
user="orkidea_lab"

if docker container inspect "$container" >/dev/null 2>&1; then
    echo "Container $container already exists. Run lab/teardown.sh first to start over." >&2
    exit 1
fi

# Generated here and kept only in lab/.lab.env, which git ignores.
password="Lab$(openssl rand -hex 12)"
sys_password="Sys$(openssl rand -hex 12)"

echo "Starting Oracle Database Free ($image) on 127.0.0.1:$port ..."
docker run -d --name "$container" -p "127.0.0.1:$port:1521" \
    -e ORACLE_PASSWORD="$sys_password" \
    -e APP_USER="$user" -e APP_USER_PASSWORD="$password" \
    "$image" >/dev/null

echo "Waiting for the database to open (a minute or two on first start) ..."
for _ in $(seq 1 180); do
    if docker logs "$container" 2>&1 | grep -q "DATABASE IS READY TO USE"; then
        break
    fi
    if ! docker container inspect -f '{{.State.Running}}' "$container" | grep -q true; then
        docker logs "$container" | tail -20 >&2
        echo "The container stopped; see its log above." >&2
        exit 1
    fi
    sleep 2
done
docker logs "$container" 2>&1 | grep -q "DATABASE IS READY TO USE" || {
    echo "The database did not open in 6 minutes." >&2
    exit 1
}

echo "Granting what parallel scans and the lab need ..."
docker exec -i "$container" sqlplus -s / as sysdba >/dev/null <<SQL
WHENEVER SQLERROR EXIT FAILURE
ALTER SESSION SET CONTAINER = FREEPDB1;
GRANT CREATE SESSION, CREATE TABLE, CREATE VIEW, CREATE PROCEDURE, CREATE SEQUENCE TO $user;
ALTER USER $user QUOTA UNLIMITED ON USERS;
-- Parallel scans read one snapshot (AS OF SCN) and split tables by extent.
GRANT EXECUTE ON DBMS_FLASHBACK TO $user;
GRANT SELECT ON DBA_EXTENTS TO $user;
EXIT
SQL

echo "Seeding the schema (a million orders; about a minute) ..."
docker exec -i "$container" sqlplus -s "$user/$password@FREEPDB1" < "$lab/seed.sql"

# A tnsnames.ora for the TNS module. The comment line before the alias is on
# purpose: aliases after a comment were not found before driver 0.2.0.
mkdir -p "$lab/tns"
cat > "$lab/tns/tnsnames.ora" <<TNS
# Orkidea lab database
orkidea_lab =
  (DESCRIPTION =
    (ADDRESS = (PROTOCOL = TCP)(HOST = 127.0.0.1)(PORT = $port))
    (CONNECT_DATA = (SERVICE_NAME = FREEPDB1)))
TNS

umask 077
cat > "$lab/.lab.env" <<ENV
# Written by lab/setup.sh; removed by lab/teardown.sh.
export ORKIDEA_LAB_HOST=127.0.0.1
export ORKIDEA_LAB_PORT=$port
export ORKIDEA_LAB_SERVICE=FREEPDB1
export ORKIDEA_LAB_USER=$user
export ORKIDEA_LAB_PASSWORD=$password
export ORKIDEA_LAB_TNS_ADMIN=$lab/tns
ENV

echo
echo "Ready. Open DuckDB connected to the lab with:"
echo "    lab/orkidea-lab.sh"
