ARG VERSION=1
FROM ghcr.io/project-chip/chip-build:${VERSION} as build
LABEL org.opencontainers.image.source https://github.com/project-chip/connectedhomeip

RUN set -x \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -fy --no-install-recommends \
    wget unzip \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/ \
    && : # last line

WORKDIR /opt/sdk
# Setup the K32W SDK
RUN set -x \
    && wget https://cache.nxp.com/lgfiles/bsps/SDK_2_6_11_K32W061DK6.zip \
    && unzip SDK_2_6_11_K32W061DK6.zip \
    && rm -rf SDK_2_6_11_K32W061DK6.zip \
    && : # last line

FROM ghcr.io/project-chip/chip-build:${VERSION}

COPY --from=build /opt/sdk/ /opt/sdk/

ENV NXP_K32W0_SDK_ROOT=/opt/sdk
