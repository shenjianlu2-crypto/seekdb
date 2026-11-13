FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++=4:13.2.0-7ubuntu1 \
        make=4.3-4.1build2 \
        libc6-dev=2.39-0ubuntu8.12 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY Makefile /app/Makefile
COPY src /app/src
RUN make CXX=g++ CXXFLAGS="-O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter -fopenmp" LDFLAGS="-fopenmp"

RUN mkdir -p /data
VOLUME ["/data"]

ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]
