FROM docker.m.daocloud.io/library/ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    g++ \
    git \
    libgtest-dev \
    python3 \
    python3-pip \
    redis-server \
    tcl \
    && pip3 install --no-cache-dir redis \
    && rm -rf /var/lib/apt/lists/*

RUN cd /usr/src/gtest && cmake . && make && \
    cp lib/*.a /usr/lib/ 2>/dev/null; \
    cp *.a /usr/lib/ 2>/dev/null; \
    true

RUN git clone --branch v2.6.25 --depth 1 https://github.com/RedisBloom/RedisBloom.git /opt/RedisBloom \
    && cd /opt/RedisBloom \
    && git submodule update --init --recursive \
    && make -j$(nproc) \
    && cp bin/*/redisbloom.so /opt/redisbloom.so \
    && rm -rf /opt/RedisBloom

WORKDIR /src
COPY . .

RUN cmake -B build && cmake --build build -j$(nproc)

CMD ["bash", "ci/run_tests.sh"]
