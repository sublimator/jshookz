# syntax=docker/dockerfile:1.7
ARG BASE_IMAGE
FROM ${BASE_IMAGE}

SHELL ["/bin/bash", "-euxo", "pipefail", "-c"]

COPY scripts/linux-product-gate.lock.env /opt/jshookz/linux-product-gate.lock.env

# Ubuntu's base image has the archive signing keys but no CA bundle. Bootstrap
# ca-certificates from the signed, date-frozen snapshot with TLS peer checking
# disabled only for that first fetch; all later downloads verify TLS and an
# explicit SHA-256 where upstream publishes one.
RUN source /opt/jshookz/linux-product-gate.lock.env && \
    snapshot="https://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/" && \
    sed -i \
      -e "s|http://archive.ubuntu.com/ubuntu/|${snapshot}|g" \
      -e "s|http://security.ubuntu.com/ubuntu/|${snapshot}|g" \
      /etc/apt/sources.list.d/ubuntu.sources && \
    apt-get -o Acquire::Retries=5 -o Acquire::https::Verify-Peer=false update && \
    DEBIAN_FRONTEND=noninteractive apt-get \
      -o Acquire::Retries=5 -o Acquire::https::Verify-Peer=false \
      install -y --no-install-recommends ca-certificates && \
    apt-get -o Acquire::Retries=5 update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      curl \
      git \
      libboost-dev \
      ninja-build \
      python3 \
      python3-pip \
      python3-venv \
      xz-utils && \
    rm -rf /var/lib/apt/lists/*

RUN source /opt/jshookz/linux-product-gate.lock.env && \
    fetch() { \
      local url="$1" expected="$2" output="$3"; \
      curl --fail --location --retry 5 --retry-all-errors --output "$output" "$url"; \
      echo "$expected  $output" | sha256sum --check; \
    } && \
    fetch \
      "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz" \
      "$NODE_SHA256" /tmp/node.tar.xz && \
    mkdir -p /opt/node && \
    tar -xJf /tmp/node.tar.xz --strip-components=1 -C /opt/node && \
    fetch \
      "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VERSION}/wasi-sdk-${WASI_SDK_VERSION}.0-x86_64-linux.tar.gz" \
      "$WASI_SDK_SHA256" /tmp/wasi-sdk.tar.gz && \
    mkdir -p /opt/tools/wasi-sdk && \
    tar -xzf /tmp/wasi-sdk.tar.gz --strip-components=1 -C /opt/tools/wasi-sdk && \
    fetch \
      "https://github.com/WebAssembly/binaryen/releases/download/version_${BINARYEN_VERSION}/binaryen-version_${BINARYEN_VERSION}-x86_64-linux.tar.gz" \
      "$BINARYEN_SHA256" /tmp/binaryen.tar.gz && \
    mkdir -p /opt/tools/binaryen && \
    tar -xzf /tmp/binaryen.tar.gz --strip-components=1 -C /opt/tools/binaryen && \
    fetch \
      "https://github.com/bytecodealliance/wizer/releases/download/v${WIZER_VERSION}/wizer-v${WIZER_VERSION}-x86_64-linux.tar.xz" \
      "$WIZER_SHA256" /tmp/wizer.tar.xz && \
    mkdir -p /opt/tools/wizer && \
    tar -xJf /tmp/wizer.tar.xz --strip-components=1 -C /opt/tools/wizer && \
    python3 -m venv /opt/conan && \
    /opt/conan/bin/pip install --disable-pip-version-check --no-cache-dir \
      "conan==${CONAN_VERSION}" && \
    mkdir -p /opt/boost-headers && \
    ln -s /usr/include/boost /opt/boost-headers/boost && \
    rm -f /tmp/node.tar.xz /tmp/wasi-sdk.tar.gz \
      /tmp/binaryen.tar.gz /tmp/wizer.tar.xz

ENV PATH="/opt/conan/bin:/opt/node/bin:/opt/tools/binaryen/bin:/opt/tools/wizer:${PATH}" \
    WASI_SDK_PATH=/opt/tools/wasi-sdk \
    BINARYEN_HOME=/opt/tools/binaryen \
    WIZER=/opt/tools/wizer/wizer \
    BOOST_INCLUDE_DIR=/opt/boost-headers \
    CMAKE_BUILD_PARALLEL_LEVEL=2 \
    CMAKE_GENERATOR=Ninja \
    CC=gcc \
    CXX=g++ \
    PYTHONDONTWRITEBYTECODE=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1

COPY scripts/linux-product-gate.sh /opt/jshookz/linux-product-gate.sh
RUN chmod 0755 /opt/jshookz/linux-product-gate.sh && \
    node --version && conan --version && \
    cmake --version && gcc --version && \
    /opt/tools/wasi-sdk/bin/clang --version && \
    wasm-opt --version && wizer --version

ENTRYPOINT ["/opt/jshookz/linux-product-gate.sh"]
