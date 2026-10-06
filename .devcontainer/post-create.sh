#!/usr/bin/env bash
set -eou pipefail

echo "Fixing permissions on mounted volumes & folders ..."
sudo chown -R "$(whoami)" /mnt/mise-data /home/vscode/.local /cmdhistory # These directories needs to be owned to be editable without root access

echo "Trusting mise config and pre-warming toolchain ..."
mise trust mise.toml
mise install --yes

echo "Post-create setup finished."
