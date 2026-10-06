#!/usr/bin/env bash
set -eou pipefail

echo "Fixing permissions on mounted volumes & folders ..."
sudo chown -R "$(whoami)" /mnt/mise-data /home/vscode/.local /cmdhistory # These directories needs to be owned to be editable without root access

echo "Trusting mise config and pre-warming toolchain ..."
mise trust mise.toml
mise install --yes

echo "Allowing git to read the bind-mounted workspace ..."
# The workspace bind mount can be owned by another uid than the container user,
# which makes git refuse to read the repository ("detected dubious ownership").
# CMake runs git at build time to stamp the version, so allow it.
git config --global --add safe.directory '*'

echo "Post-create setup finished."
