FROM docker.m.daocloud.io/library/ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    g++ \
    libgtest-dev \
    redis-server \
    tcl \
    && rm -rf /var/lib/apt/lists/*

RUN cd /usr/src/gtest && cmake . && make && \
    cp lib/*.a /usr/lib/ 2>/dev/null; \
    cp *.a /usr/lib/ 2>/dev/null; \
    true

WORKDIR /src
COPY . .

RUN cmake -B build && cmake --build build -j$(nproc)

CMD ["bash", "ci/run_tests.sh"]
