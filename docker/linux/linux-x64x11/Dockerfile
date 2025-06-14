# Copyright 2021 The Cobalt Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

ARG FROM_IMAGE
FROM ${FROM_IMAGE:-cobalt-build-base}

RUN apt update -qqy \
    && apt install -qqy --no-install-recommends \
        clang \
        libgles2-mesa-dev \
        mesa-common-dev \
        libpulse-dev \
        libasound2-dev \
        libxrender-dev \
        libxcomposite-dev \
        libxi-dev \
    && /opt/clean-after-apt.sh

COPY ./linux-x64x11/requirements.txt /opt/requirements.txt
RUN python3 -m pip install --require-hashes --no-deps -r /opt/requirements.txt

CMD gn gen ${OUTDIR}/${PLATFORM}${SB_API_VERSION:+-sbversion-$SB_API_VERSION}_${CONFIG:-debug} --args="target_platform=\"${PLATFORM}\" build_type=\"${CONFIG:-debug}\" build_with_separate_cobalt_toolchain=true ${SB_API_VERSION:+sb_api_version=$SB_API_VERSION}" && \
    ninja -j ${NINJA_PARALLEL} -C ${OUTDIR}/${PLATFORM}${SB_API_VERSION:+-sbversion-$SB_API_VERSION}_${CONFIG:-debug} ${TARGET:-cobalt_install}
