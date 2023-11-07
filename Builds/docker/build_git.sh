#!/usr/bin/env bash

set -ex
GIT_VERSION=${GIT_VERSION:-"2.42.0"}
tar_file=${GIT_VERSION}.tar.gz
curl -OJL https://github.com/git/git/archive/refs/tags/v${tar_file}
tar zxvf git-${tar_file} && rm git-${tar_file}

cd git-${GIT_VERSION}

. /etc/os-release
if [ $ID = centos ]; then
    source /opt/rh/devtoolset-11/enable
fi

make configure
./configure
make git -j$(nproc)
make install git
cd ..
rm -rf git-${GIT_VERSION}
git  --version | cut -d ' ' -f3
