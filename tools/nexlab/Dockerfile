# Splatoon PC — NEX server image (not built or tested on this machine: Docker is absent).
#
# Build from the repository root, so the build context only carries the server tools:
#   docker build -f tools/nexlab/Dockerfile -t splatoon-nex .
# Run with host networking: the NAT check needs two real IPs, which Docker's default
# bridge network cannot provide.
#   docker run --rm --network host -v /srv/splatoon:/data splatoon-nex
# /data holds server.json and accounts.sqlite3 (the account store is written at runtime).
FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1

WORKDIR /app

COPY tools/nexlab/requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt

COPY tools /app/tools

EXPOSE 60000/udp 60001/udp 10025/udp 10125/udp 60002/tcp
VOLUME ["/data"]

CMD ["python", "tools/nexlab/nexserver.py", "--config", "/data/server.json", "--log", "/data/nexserver.log"]
