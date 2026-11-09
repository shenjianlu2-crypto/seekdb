FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends g++ make libc6-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY Makefile /app/Makefile
COPY src /app/src
RUN make CXX=g++ CXXFLAGS="-O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter"

RUN mkdir -p /data
VOLUME ["/data"]

ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]