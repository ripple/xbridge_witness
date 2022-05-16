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

### rippled inclusion

This project depends on the [rippled](https://github.com/ripple/rippled.git) repository for core signing functionality. If you have built and installed rippled, you can point this project at your installation using `CMAKE_PREFIX_PATH` (if you have installed in a standard system search path, this is not needed), e.g.:

```
$ cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/rippled/installation/root ../..
```

Alternatively, if you do not have a local installation of rippled development files that you want to use, then this project will fetch an appropriate version of the source code using CMake's FetchContent.

### Other dependencies

* C++17 or greater
* [Boost](http://www.boost.org/) - 1.70+ required
* [OpenSSL](https://www.openssl.org/) 
* [cmake](https://cmake.org) - 3.11+ required

## Build and run

For linux and other unix-like OSes, run the following commands (see note above about adding `CMAKE_PREFIX_PATH` as needed):

```
$ cd ${ATTN_SERVER_DIRECTORY}
$ mkdir -p build/gcc.release
$ cd build/gcc.release
$ cmake -DCMAKE_BUILD_TYPE=Release ../..
$ cmake --build .
$ ./attn_server
```

For 64-bit Windows, open a MSBuild Command Prompt for Visual Studio
and run the following commands:

```
> cd %ATTN_SERVER_DIRECTORY%
> mkdir build
> cd build
> cmake ..
> cmake --build . --config Release
> .\Release\attn_server.exe
```

32-bit Windows builds are not supported.

## Guide

[Attestation Server Documentation](doc/attn-server-guide.md)
