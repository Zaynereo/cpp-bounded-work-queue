FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-12 cmake ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-12 \
 && cmake --build build -j2 \
 && cd build && ctest --output-on-failure

CMD ["./build/demo", "2", "8", "16", "block"]
