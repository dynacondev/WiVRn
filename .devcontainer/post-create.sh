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

echo "Registering flathub remote for flatpak builds ..."
# Needed by flatpak-builder --install-deps-from=flathub when packaging
# wivrn-server.flatpak. Guarded so an offline first launch doesn't fail setup.
if ! flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo; then
	echo "WARNING: could not register flathub remote; flatpak builds will fail until it is added" >&2
fi

echo "Post-create setup finished."
