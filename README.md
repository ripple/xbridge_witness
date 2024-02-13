



# Witness Server for XRPL Sidechains

This is the Witness Server for [XLS-38d Cross-Chain bridge project](https://github.com/XRPLF/XRPL-Standards/tree/master/XLS-0038d-cross-chain-bridge). "Witness Servers" validate transfers between "door accounts" that connect a locking chain to each issuing chain by listening for transactions on one or both of the chains and signing attestations to prove certain events happened on a chain.

## Additional documentation

- [XRPL Sidechains concept](https://xrpl.org/xrpl-sidechains.html)
- [Cross bridge transactions](https://xrpl.org/cross-chain-bridges.html#how-do-bridges-work/)
- [XRPL-Standards documentation](https://github.com/XRPLF/XRPL-Standards/tree/master/XLS-0038d-cross-chain-bridge)


## Minimum Requirements

- [Python 3.7](https://www.python.org/downloads/)
- [Conan 1.55](https://conan.io/downloads.html) Conan 2.0 is **_not_** supported yet
- [CMake 3.20](https://cmake.org/download/)

| Compiler    | Version |
|-------------|---------|
| GCC         | 11      |
| Clang       | 13      |
| Apple clang | 15      |

## Build and run

1. Create a build directory and `cd` into it. 

```bash
mkdir .build && cd .build
```

2. Configure Conan.

Add Ripple's Artifactory as a Conan remote to source the `libxrpl` Conan package.

```bash
conan remote add --insert 0 conan-non-prod http://18.143.149.228:8081/artifactory/api/conan/conan-non-prod

```
<details>
<summary> Optional local development of <code>xrpl</code> library</summary>

The Conan `xrpl` recipe is also available by checking out the [`rippled` source](https://github.com/XRPLF/rippled.git) and exporting the recipe locally.

```bash
git clone https://github.com/XRPLF/rippled.git
cd rippled 
conan export .
```
</details>

```bash
conan profile update settings.cppstd=20 default
conan profile update settings.compiler.libcxx=libstdc++11 default
```
<br>
<details>
      <summary>Example Conan profiles</summary>
  
### Linux
```ini
  [settings]
  arch=x86_64
  arch_build=x86_64
  os=Linux
  os_build=Linux
  build_type=Release
  compiler=gcc
  compiler.cppstd=20
  compiler.libcxx=libstdc++11
  compiler.version=11
```
### macOS

On macOS [you may get an error from Boost](https://github.com/XRPLF/rippled/blob/develop/BUILD.md#call-to-async_teardown-is-ambiguous) 
which requires adding some CMake flags to your Conan profile.
```
conan profile update 'env.CXXFLAGS="-DBOOST_ASIO_DISABLE_CONCEPTS"' default
conan profile update 'conf.tools.build:cxxflags+=["-DBOOST_ASIO_DISABLE_CONCEPTS"]' default
```

```ini
[settings]
os=Macos
os_build=Macos
arch=armv8
arch_build=armv8
compiler=apple-clang
compiler.version=15
build_type=Release
compiler.cppstd=20
compiler.libcxx=libc++
[options]
[conf]
tools.build:cxxflags=['-DBOOST_ASIO_DISABLE_CONCEPTS']
[build_requires]
[env]
CXXFLAGS=-DBOOST_ASIO_DISABLE_CONCEPTS
```
</details>

3. Run Conan to install and/or build dependencies.

``` bash
conan install .. \
  --output-folder . \
  --build missing \
  --settings build_type=Release
```
4. Configure CMake.

``` bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake
```

5. Build with CMake.

``` bash
cmake --build --parallel $(nproc)
```

6. Run the unit tests.

``` bash
./xbridge_witnessd --unittest
```

[Check the documentation for configuration examples.](https://xrpl.org/witness-servers.html#witness-server-configuration)

## Additional help

Additional help for Conan/CMake issues may be found in [`rippled`'s build instructions.](https://github.com/XRPLF/rippled/blob/develop/BUILD.md)

Feel free to open an [issue](https://github.com/ripple/xbridge_witness/issues) if you have a feature request or something doesn't work as expected.
