FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive MPLCONFIGDIR=/tmp/mplconfig
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make python3 python3-pip python3-numpy python3-matplotlib \
    python3-pytest libconfig++-dev git ca-certificates pkg-config \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /repo
COPY . /repo
RUN cmake -S /repo -B /repo/build -DCMAKE_BUILD_TYPE=Release -Wno-dev \
 && cmake --build /repo/build -j$(nproc) \
 && ctest --test-dir /repo/build --output-on-failure

# Standalone artifact: no Pin, no zsim build. Every reproduced figure comes
# from the CXRay trace/closed-loop tiers; Table VI re-measures only the CXRay
# columns (recorded zsim/Xerxes values ship as reference). The vendored zsim
# fork and gem5/Sniper integrations remain in the tree for users, but they
# are outside the artifact's reproduction scope (see docker/README_PIN.md).
ENTRYPOINT ["./reproduce.sh"]
