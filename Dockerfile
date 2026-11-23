FROM debian:trixie-20261104-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang-format \
        curl \
        gcc \
        gdb \
        git \
        libc6-dev \
        make \
        python3 \
        strace \
        valgrind \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --gid 1000 dev \
    && useradd --uid 1000 --gid dev --create-home --shell /bin/bash dev \
    && mkdir /workspace \
    && chown dev:dev /workspace

ENV LANG=C.UTF-8
WORKDIR /workspace
USER dev
CMD ["/bin/bash"]
