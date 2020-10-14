#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

. ./utils/build-apple-framework.sh
            
if [ ! -d destroot/Library/Frameworks/macosx/hermes.framework ]; then
    mac_deployment_target=$(get_mac_deployment_target)

    build_apple_framework "macosx" "x86_64;arm64" "$mac_deployment_target"
fi