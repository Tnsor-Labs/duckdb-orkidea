#!/usr/bin/env bash
# Removes the lab container, its connection details, and the files the lab
# wrote. Safe to run more than once.
set -euo pipefail

lab="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
container="${ORKIDEA_LAB_CONTAINER:-orkidea-lab}"

if docker container inspect "$container" >/dev/null 2>&1; then
    docker rm -f "$container" >/dev/null
    echo "Removed container $container."
fi
rm -rf "$lab/.lab.env" "$lab/tns" "$lab/out"
echo "Removed lab/.lab.env, lab/tns, and lab/out."
