#!/usr/bin/env bash
# Runs part of the benchmark with network delay between the client and the
# lab database: for each delay, a sidecar container sharing the lab
# container's network adds it to every packet Oracle sends (tc netem), so
# each round trip takes that much longer. The delay is removed on exit.
#
#   ORACLE_IC=/path/to/instantclient_23_xx bench/network.sh [delays in ms...]
#
# Default delays: 0 1 5 20. Results go to bench/results-network-<ms>ms.json.
set -euo pipefail

bench="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python="${PYTHON:-$bench/../.venv/bin/python}"
container="${ORKIDEA_LAB_CONTAINER:-orkidea-lab}"
delays=("$@")
[[ ${#delays[@]} -gt 0 ]] || delays=(0 1 5 20)

engines="orkidea,orkidea-1-session,oracle,oracle-tuned,orkidea-adbc,python-oracledb-tuned"
queries="scan_narrow,filter_point,group_by,top_n,point_lookup,py_scan_narrow"

netem() {
    docker run --rm --net "container:$container" --cap-add NET_ADMIN alpine:3.20 \
        sh -c "apk add -q iproute2 >/dev/null && tc qdisc del dev eth0 root 2>/dev/null; $1"
}
trap 'netem true' EXIT

for delay in "${delays[@]}"; do
    if [[ "$delay" == 0 ]]; then
        netem true
    else
        netem "tc qdisc add dev eth0 root netem delay ${delay}ms"
    fi
    echo "== ${delay} ms per round trip"
    "$python" "$bench/run.py" --engines "$engines" --queries "$queries" \
        --runs 3 --point-lookups 20 --out "$bench/results-network-${delay}ms.json"
done
