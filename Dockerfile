FROM docker.m.daocloud.io/library/ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    g++ \
    libgtest-dev \
    python3 \
    python3-pip \
    tcl \
    && pip3 install --no-cache-dir redis \
    && rm -rf /var/lib/apt/lists/*

RUN cd /usr/src/gtest && cmake . && make && \
    cp lib/*.a /usr/lib/ 2>/dev/null; \
    cp *.a /usr/lib/ 2>/dev/null; \
    true

COPY deps/ /opt/deps/

RUN cd /opt/deps && tar xzf redis-6.0.16.tar.gz \
    && cd redis-6.0.16 && make -j$(nproc) \
    && make install \
    && cd / && rm -rf /opt/deps/redis-6.0.16

RUN cd /opt/deps && tar xzf redisbloom-2.6.25.tar.gz \
    && cd redisbloom-2.6.25 && make -j$(nproc) \
    && cp bin/*/redisbloom.so /opt/redisbloom.so \
    && cd / && rm -rf /opt/deps/redisbloom-2.6.25

WORKDIR /src
COPY . .

RUN cmake -B build && cmake --build build -j$(nproc)

CMD ["bash", "ci/run_tests.sh"]
