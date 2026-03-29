## ----- Build Stage: md380_vocoder_dynarmic -----
FROM ubuntu:24.04 AS md380-builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    g++ \
    cmake \
    git \
    curl \
    unzip \
    python3 \
    xxd \
    libboost-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
RUN git clone --depth 1 https://github.com/jcmerg/md380_vocoder_dynarmic.git

WORKDIR /build/md380_vocoder_dynarmic/build
RUN cmake .. && make -j$(nproc) && sh ../makelib.sh

## ----- Build Stage: imbe_vocoder -----
FROM ubuntu:24.04 AS imbe-builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    g++ \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
RUN git clone --depth 1 https://github.com/nostar/imbe_vocoder.git

WORKDIR /build/imbe_vocoder
RUN make -j$(nproc) && make install

## ----- Build Stage: libftd2xx -----
FROM ubuntu:24.04 AS ftdi-builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
RUN curl -L -o libftd2xx.tgz https://ftdichip.com/wp-content/uploads/2025/11/libftd2xx-linux-x86_64-1.4.34.tgz \
    && tar xzf libftd2xx.tgz

## ----- Build Stage: tcd -----
FROM ubuntu:24.04 AS tcd-builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    g++ \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install libftd2xx
COPY --from=ftdi-builder /build/linux-x86_64/libftd2xx.so.1.4.34 /usr/local/lib/
COPY --from=ftdi-builder /build/linux-x86_64/ftd2xx.h /usr/local/include/
COPY --from=ftdi-builder /build/linux-x86_64/WinTypes.h /usr/local/include/
RUN ln -sf /usr/local/lib/libftd2xx.so.1.4.34 /usr/local/lib/libftd2xx.so \
    && ldconfig

# Install imbe_vocoder
COPY --from=imbe-builder /usr/local/lib/libimbe_vocoder.a /usr/local/lib/
COPY --from=imbe-builder /usr/local/include/imbe_vocoder_api.h /usr/local/include/

# Install md380_vocoder
COPY --from=md380-builder /build/md380_vocoder_dynarmic/build/libmd380_vocoder.a /usr/local/lib/
COPY --from=md380-builder /build/md380_vocoder_dynarmic/md380_vocoder.h /usr/local/include/

# Clone tcd and urfd (tcd has symlinks to urfd sources)
WORKDIR /build
RUN git clone --depth 1 https://github.com/n7tae/tcd.git \
    && git clone --depth 1 https://github.com/nostar/urfd.git

WORKDIR /build/tcd
RUN cp config/* .

# Enable software AMBE2 vocoder and build
# Add -fpermissive to work around const-correctness mismatch in md380_vocoder.h
RUN sed -i 's/^swambe2 = false/swambe2 = true/' tcd.mk \
    && sed -i 's/-W -Werror/-W -fpermissive/' Makefile \
    && make -j$(nproc)

## ----- Runtime Stage -----
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    usbutils \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install libftd2xx runtime
COPY --from=ftdi-builder /build/linux-x86_64/libftd2xx.so.1.4.34 /usr/local/lib/
RUN ln -sf /usr/local/lib/libftd2xx.so.1.4.34 /usr/local/lib/libftd2xx.so.1 \
    && ln -sf /usr/local/lib/libftd2xx.so.1 /usr/local/lib/libftd2xx.so \
    && ldconfig

# Copy tcd binary
COPY --from=tcd-builder /build/tcd/tcd /usr/local/bin/tcd

# Default config (will be overridden by volume mount)
COPY tcd.ini.example /etc/tcd/tcd.ini

# Entrypoint script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
