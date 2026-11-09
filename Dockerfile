FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV PYTHONUNBUFFERED=1

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        python3 \
        python3-pip \
        python3-numpy \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY minivectordb.py /app/minivectordb.py

RUN mkdir -p /data

CMD ["python3", "/app/minivectordb.py"]
