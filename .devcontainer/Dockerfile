FROM mcr.microsoft.com/devcontainers/base:ubuntu@sha256:7ee7da33a68d997971660d91ecc8372e55a38a777c3c6bd6808daf91928052db

# hadolint ignore=DL3064
ARG USERNAME=vscode

RUN \
    # Toolchain: file for previews, clang/lldb/lld native arm64 (NOT via mise —
    # simplest versioned prebuilts), Java for jadx/apktool, httpie as apt fallback
    # (primary http client is mise github:httpie/cli), plus build essentials.
    apt-get update && \
    apt-get install -y --no-install-recommends file clang lldb lld httpie openjdk-21-jre-headless build-essential unzip curl ca-certificates && \
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
