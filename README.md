# Witness Server for XRPL Sidechains

## Table of contents

* [Documentation](#documentation)
* [Build guide](#build-guide)
  * [Dependencies](#dependencies)
    * [Conan inclusion](#conan-inclusion)
    * [Other dependencies](#other-dependencies)
  * [Build steps](#build-and-run)

## Documentation

- [XRPL Sidechains concept](https://xrpl.org/xrpl-sidechains.html)
- [Cross bridge transactions](https://opensource.ripple.com/docs/xls-38d-cross-chain-bridge/cross-chain-bridges/)
- [XRPL-Standards documentation](https://github.com/XRPLF/XRPL-Standards/tree/master/XLS-0038d-cross-chain-bridge)


## Build guide
### Dependencies

#### Conan inclusion

This project depends on conan (v1.5 and higher, v2.0 not supported) to build it's dependencies. See [conan doc](https://docs.conan.io/1/installation.html) to install conan.

#### Other dependencies

* C++20 compiler (gcc >=11, clang >=13)
* [cmake](https://cmake.org) - at least 3.20


### Build and run

1) Create a build directory. For example: build
2) Change to that directory.
3) Configure conan (once before very 1st build)

``` bash
conan profile update settings.cppstd=20 default
conan profile update settings.compiler.libcxx=libstdc++11 default
conan profile update settings.arch=x86_64 default
```

4) Run conan. The command is:

``` bash
conan install -b missing -s build_type=Release --output-folder . ..
```

5) Create a build file (replace .. with the appropriate directory). 2 pckage system supported - deb and rpm:
* 5.1 Default. If you have [rippled](https://github.com/XRPLF/rippled/tree/develop) installed by your packet manager - cmake will try to find it. If nothing found - this setup will download rippled and build it.
``` bash
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build/generators/conan_toolchain.cmake -DPKG=deb ..
```
* 5.2 If you have [rippled](https://github.com/XRPLF/rippled/tree/develop) build from source you can use it
``` bash
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build/generators/conan_toolchain.cmake -DRIPPLE_SRC_DIR=/home/user/repo/rippled -DRIPPLE_BIN_DIR=/home/user/repo/rippled/build-release -DPKG=deb ..
```

6) Build the project:

``` bash
make -j $(nproc)
```

7) Run

``` bash
./xbridge_witnessd --conf /home/user/repo/config.json
```
Check for config examples in documenation
