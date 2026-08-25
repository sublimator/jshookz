# syntax=docker/dockerfile:1.7@sha256:a57df69d0ea827fb7266491f2813635de6f17269be881f696fbfdf2d83dda33e
ARG BASE_IMAGE
FROM ${BASE_IMAGE}

SHELL ["/bin/bash", "-euxo", "pipefail", "-c"]

COPY scripts/linux-product-gate.lock.env /opt/jshookz/linux-product-gate.lock.env

# Keep the container narrow: this image exists only to reproduce the Linux
# compiler, Conan, CMake and CTest path that exposed issue 0096.
RUN source /opt/jshookz/linux-product-gate.lock.env && \
    sed -i \
      's|http://ports.ubuntu.com/ubuntu-ports|http://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports|g' \
      /etc/apt/sources.list.d/ubuntu.sources && \
    apt-get -o Acquire::Retries=5 update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      git \
      libboost-dev \
      ninja-build \
      python3 \
      python3-pip \
      python3-venv && \
    python3 -m venv /opt/conan && \
    /opt/conan/bin/pip install --disable-pip-version-check --no-cache-dir \
      "conan==${CONAN_VERSION}" && \
    rm -rf /var/lib/apt/lists/*

ENV PATH="/opt/conan/bin:${PATH}" \
    CMAKE_BUILD_PARALLEL_LEVEL=2 \
    CMAKE_GENERATOR=Ninja \
    CC=gcc \
    CXX=g++ \
    PYTHONDONTWRITEBYTECODE=1

COPY scripts/linux-product-gate.sh /opt/jshookz/linux-product-gate.sh
RUN chmod 0755 /opt/jshookz/linux-product-gate.sh

ENTRYPOINT ["/opt/jshookz/linux-product-gate.sh"]
