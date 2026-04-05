FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive
ENV PORT=8000
ENV HOST=0.0.0.0

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        g++ \
        make \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY . .

RUN make \
    && python3 tools/generate_powerlaw_graph.py --output data/powerlaw_graph.edgelist --vertices 4096 --fanout 12

EXPOSE 8000

CMD ["python3", "tools/server.py"]
