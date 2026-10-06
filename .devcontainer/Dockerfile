FROM mcr.microsoft.com/devcontainers/base:resolute

# hadolint ignore=DL3008
ARG USERNAME=vscode

# hadolint ignore=DL3008
RUN \
    # Ubuntu 26.04 (resolute) ships everything WiVRn needs stock, unlike 24.04:
    # - Vulkan >= 1.4.304 from the archive (CI builds server/dashboard on
    #   ubuntu-26.04 with plain libvulkan-dev, so no LunarG repo is needed)
    # - KF6/Qt6 from the archive (so no KDE neon repo is needed)
    # Package sets are kept in sync with .github/workflows/Build.yml:
    # server deps with SERVER_DEPS, dashboard deps with the 'dashboard' matrix entry.
    apt-get update && \
    apt-get install -y --no-install-recommends \
    file clang lldb lld httpie openjdk-21-jre-headless build-essential unzip curl ca-certificates \
    pkg-config python3 ccache glslang-tools \
    # Python modules for tools/gen_flatpak_manifest.py (via apt, not pip, so the
    # image stays reproducible without --break-system-packages workarounds)
    python3-toml python3-aiohttp \
    # Flatpak packaging: produces the wivrn-server.flatpak test bundle.
    # NOTE flatpak-builder needs user namespaces (bubblewrap); the devcontainer
    # runs privileged for this (see runArgs in devcontainer.json).
    flatpak flatpak-builder \
    # Vulkan (stock, >= 1.4.304) + shader tools + a software ICD for headless runs
    libvulkan-dev vulkan-tools spirv-tools mesa-vulkan-drivers \
    # Boost components required by CMakeLists.txt (locale, iostreams;
    # url is header-only and only requested for the client build)
    libboost-dev libboost-locale-dev libboost-iostreams-dev libsystemd-dev \
    # Server build deps, kept in sync with SERVER_DEPS in .github/workflows/Build.yml
    libx264-dev libavcodec-dev libavutil-dev libbsd-dev \
    libavahi-client-dev libavahi-glib-dev libeigen3-dev nlohmann-json3-dev \
    librsvg2-dev libdrm-dev libglib2.0-dev-bin libpipewire-0.3-dev \
    libcli11-dev libnotify-dev libarchive-dev libudev-dev \
    libwayland-dev libxrandr-dev \
    # Dashboard build deps, kept in sync with the 'dashboard' job in .github/workflows/Build.yml
    gettext extra-cmake-modules qt6-base-dev qt6-declarative-dev \
    libkirigami-dev libkf6i18n-dev libkf6coreaddons-dev \
    libkf6qqc2desktopstyle-dev libkf6iconthemes-dev qcoro-qt6-dev \
    qml6-module-org-kde-kirigami qml6-module-org-kde-kirigamiaddons-formcard && \
    rm -rf /var/lib/apt/lists/* && \
    # Make command history file
    mkdir /cmdhistory && touch /cmdhistory/.zsh_history && \
    # Set up .zshrc
    cat <<EOF >> /home/vscode/.zshrc
export PROMPT_COMMAND='history -a' # Saves history immediately after command
export HISTFILE=/cmdhistory/.zsh_history # Save history to file on a persistent volume
eval "\$(mise activate zsh)" # Enables Mise in shells
export EDITOR=nano  # Preference for Yazi to use nano
EOF
