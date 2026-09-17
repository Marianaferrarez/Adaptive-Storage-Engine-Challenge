FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY . .
RUN make clean && make

FROM ubuntu:24.04
COPY --from=build /build/engine /engine
ENTRYPOINT ["/engine"]
