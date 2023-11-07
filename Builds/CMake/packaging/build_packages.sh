#!/usr/bin/env bash
set -ex

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
src_dir=${SCRIPT_DIR}/../../..
bin_dir="${src_dir}/build"
pkg_dir="${src_dir}/packages"
build_config=Release
conan_packages_to_build="missing"
#conan_profile="default"
nproc=$(($(nproc) - 2))

if [ $nproc -lt 3 ]; then
    nproc=$(nproc)
fi
exe="xbridge_witnessd"

. /etc/os-release
if [ $ID = centos ]; then
    source /opt/rh/devtoolset-11/enable
    source /opt/rh/rh-python38/enable
    conan_packages_to_build="" # blank bc all dependencies need to be built for CentOS 7 currently
    #conan_profile="centos" # TODO: Make a "centos" profile and upload the bin pkgs
    #conan remote add --insert 0 conan-non-prod http://18.143.149.228:8081/artifactory/api/conan/conan-non-prod || true
fi

conan profile new default --detect
conan profile update settings.compiler.cppstd=20 default
conan profile update settings.compiler.libcxx=libstdc++11 default

conan install ${src_dir} \
    --output-folder "${bin_dir}" \
    --install-folder "${bin_dir}" \
    --build ${conan_packages_to_build} \
    --settings build_type=${build_config}

cmake \
    -S "${src_dir}" \
    -B "${bin_dir}" \
    -DCMAKE_BUILD_TYPE=${build_config} \
    -DPKG=deb \
    -DCMAKE_TOOLCHAIN_FILE:FILEPATH=build/generators/conan_toolchain.cmake

cmake \
    --build "${bin_dir}" \
    --parallel $nproc \
    --target ${exe} \
    --target package

cmake \
    -S "${src_dir}" \
    -B "${bin_dir}" \
    -DCMAKE_BUILD_TYPE=${build_config} \
    -DPKG=rpm \
    -DCMAKE_TOOLCHAIN_FILE:FILEPATH=build/generators/conan_toolchain.cmake

cmake \
    --build "${bin_dir}" \
    --parallel $nproc \
    --target package

if [ $build_config = "Release" ]; then
    strip ${bin_dir}/${exe}
fi

rm -rf ${pkg_dir}/_CPack_Packages
