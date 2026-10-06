FROM mcr.microsoft.com/devcontainers/base:ubuntu@sha256:7ee7da33a68d997971660d91ecc8372e55a38a777c3c6bd6808daf91928052db

# hadolint ignore=DL3008,DL3064
ARG USERNAME=vscode

RUN \
    # WiVRn requires Vulkan >= 1.4.304 (CMakeLists.txt) but Ubuntu 24.04 only ships
    # 1.3.275, so pull headers/loader/tools from LunarG instead. CI gets this for free
    # by running on ubuntu-26.04 (.github/workflows/Build.yml).
    # The dashboard needs KF6, which is not in Ubuntu 24.04 at all, so pull Qt6/KF6
    # from KDE neon (built for noble).
    apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates curl && \
    curl -fsSL https://packages.lunarg.com/lunarg-signing-key-pub.asc -o /etc/apt/trusted.gpg.d/lunarg.asc && \
    . /etc/os-release && \
    curl -fsSL "https://packages.lunarg.com/vulkan/lunarg-vulkan-${VERSION_CODENAME}.list" \
        -o "/etc/apt/sources.list.d/lunarg-vulkan-${VERSION_CODENAME}.list" && \
    curl -fsSL https://archive.neon.kde.org/public.key -o /etc/apt/trusted.gpg.d/neon.asc && \
    echo "deb https://archive.neon.kde.org/user ${VERSION_CODENAME} main" \
        > "/etc/apt/sources.list.d/neon-${VERSION_CODENAME}.list" && \
    rm -rf /var/lib/apt/lists/*

# hadolint ignore=DL3008
RUN \
    # Toolchain: file for previews, clang/lldb/lld native arm64 (NOT via mise —
    # simplest versioned prebuilts), Java for jadx/apktool, httpie as apt fallback
    # (primary http client is mise github:httpie/cli), plus build essentials.
    # cmake/ninja/git/glslang/glib come from mise (mise.toml), everything else is apt.
    apt-get update && \
    apt-get install -y --no-install-recommends \
    file clang lldb lld httpie openjdk-21-jre-headless build-essential unzip curl ca-certificates \
    pkg-config python3 ccache glslang-tools \
    # Vulkan from LunarG (>= 1.4.304) + shader tools + a software ICD for headless runs
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
    # Dashboard build deps (Qt6/KF6/QCoro from KDE neon, Gettext for translations)
    gettext qt6-base-dev qt6-declarative-dev extra-cmake-modules \
    kf6-kirigami-dev libkf6i18n-dev libkf6coreaddons-dev \
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
