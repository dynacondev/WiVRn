#!/usr/bin/env bash
set -eou pipefail

echo "Ensuring Mise is up to date"
sudo mise self-update --yes # Ensure mise is up to date
