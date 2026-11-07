# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Tool images are built separately and serially by the release driver.
ARG UBUNTU=ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
ARG SEA_MLIR_IMAGE=sea-mlir-release:sea-mlir
ARG SEAHORN_IMAGE=sea-mlir-release:seahorn
FROM ${SEA_MLIR_IMAGE} AS package
ARG DEBIAN_FRONTEND=noninteractive
ARG VERSION=0.1
ARG SOURCE_DATE_EPOCH=0
ARG SOURCE_REVISION=unknown
RUN apt-get update && apt-get install -y --no-install-recommends patchelf \
  && rm -rf /var/lib/apt/lists/*
COPY docker/package-release.py /release-tools/package-release.py
RUN python3 /release-tools/package-release.py \
      --name "sea-mlir-${VERSION}-linux-x86_64" \
      --source-revision "$SOURCE_REVISION" --epoch "$SOURCE_DATE_EPOCH"

FROM ${UBUNTU} AS runtime
ARG VERSION=0.1
COPY --from=package /release/sea-mlir-${VERSION}-linux-x86_64 /opt/sea-mlir
ENV PATH=/opt/sea-mlir/bin:$PATH
WORKDIR /work
CMD ["sea-vc", "--help"]

FROM ${SEAHORN_IMAGE} AS seahorn-install
FROM package AS bundle-package
ARG VERSION=0.1
ARG SOURCE_DATE_EPOCH=0
ARG SOURCE_REVISION=unknown
COPY --from=seahorn-install /opt/seahorn /opt/seahorn
RUN python3 /release-tools/package-release.py \
      --name "sea-mlir-bundle-${VERSION}-linux-x86_64" --seahorn /opt/seahorn \
      --source-revision "$SOURCE_REVISION" --epoch "$SOURCE_DATE_EPOCH"

FROM ${UBUNTU} AS bundle-runtime
ARG DEBIAN_FRONTEND=noninteractive
ARG VERSION=0.1
RUN apt-get update && apt-get install -y --no-install-recommends clang-18 python3 \
  && rm -rf /var/lib/apt/lists/*
COPY --from=bundle-package /release/sea-mlir-bundle-${VERSION}-linux-x86_64 /opt/sea-mlir
ENV PATH=/opt/sea-mlir/bin:$PATH
WORKDIR /work
CMD ["sea-vc", "--help"]
