# attn-server

[![Build Status](https://travis-ci.org/ripple/attn-server.svg?branch=master)](https://travis-ci.org/ripple/attn-server)
[![Build status](https://ci.appveyor.com/api/projects/status/dd42bs8pfao8k82p/branch/master?svg=true)](https://ci.appveyor.com/project/ripple/attn-server)
[![codecov](https://codecov.io/gh/ripple/attn-server/branch/master/graph/badge.svg)](https://codecov.io/gh/ripple/attn-server)

Attestation Server for XRPL Sidechains

## Table of contents

* [Dependencies](#dependencies)
  * [rippled inclusion](#rippled-inclusion)
  * [Other dependencies](#other-dependencies)
* [Build and run](#build-and-run)
* [Guide](#guide)

## Dependencies

### conan inclusion

This project depends on conan (v1.5 and higher, v2.0 not supported) to build it's dependencies. See https://conan.io/ to install conan.

Once conan is installed, the following can be used to build the project:

1) Create a build directory. For example: build
2) Change to that directory.
3) Configure conan (once before very 1st build)

``` bash
conan profile update settings.cppstd=20 default
conan profile update settings.compiler.libcxx=libstdc++11 default
conan profile update settings.arch=x86_64 default
```

3) Run conan. The command is:

``` bash
conan install -b missing -s build_type=Debug --output-folder . ..
```

(Note: the exact command I use is as follows, but this assumes gcc 12 is used and a gcc12 conan profile is present):
```bash
CC=$(which gcc) CXX=$(which g++) conan install -b missing --profile gcc12 -s build_type=Debug --output-folder . ..
```

4) Create a build file (replace .. with the appropriate directory):

``` bash
cmake -DCMAKE_BUILD_TYPE=Debug -Dunity=Off ..
```


(Note: the exact command I use is as follows, but this is specific to my setup:)
``` bash
CC=$(which gcc) CXX=$(which g++) cmake -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DCMAKE_BUILD_TYPE=Debug -GNinja ..
```

5) Build the project:

``` bash
make -j $(nproc)
```

### Other dependencies

* C++20
* [cmake](https://cmake.org) - at least 3.20


## Build and run

For linux and other unix-like OSes, run the following commands (see note above about adding `CMAKE_PREFIX_PATH` as needed):

```
$ cd ${ATTN_SERVER_DIRECTORY}
$ mkdir -p build
$ cd build
$ conan install -b missing -s build_type=Release --output-folder . ..
$ cmake -DCMAKE_BUILD_TYPE=Release ..
$ make -j $(nproc)
$ ./xbridge_witness --conf path_to_conf
```

For 64-bit Windows, open a MSBuild Command Prompt for Visual Studio
and run the following commands:

```
> cd %ATTN_SERVER_DIRECTORY%
> mkdir build
> cd build
> conan install -b missing -s build_type=Release --output-folder . ..
> cmake ..
> cmake --build . --config Release
> .\Release\xbridge_witness.exe
```

32-bit Windows builds are not supported.

## Guide

[Attestation Server Documentation](https://github.com/XRPLF/XRPL-Standards/discussions/92)
