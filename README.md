MIL-STD-2045-47001 defines an application envelope for Variable Message Format
(VMF) and other message formats. This library supports editions B, C, D and
D with Change 1 (D1). Edition A was never issued; its placeholder defines no
technical format. The original 1995 edition is unsupported pending clarification
of its header-end padding and header/body boundary. The
[format reference](docs/format.md#original-1995-edition) preserves its historical
sources and the limits of the available evidence.

A message contains an envelope and a payload whose fields depend on a schema
agreed by sender and receiver. This library provides C11 and procedural C++17
interfaces, logical header structures, ordered VMF message/index definitions,
and incremental event-based encoding and decoding. MIL-STD-6017 message
definitions are not included. Public examples and synthetic schemas exercise
the codec without claiming compatibility with any fielded message set.

# Quickstart

Install CMake 3.24 or newer, Git, and a C/C++ toolchain with C++17 support.
Initial dependency downloads require network access. Run these commands from
the repository root to configure and build the default unit suite:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

The build and populated unit suite must pass. Ruby and Bundler are unnecessary
for this configuration.

Acceptance testing is optional. Install Ruby, Bundler, and `patch` before
running the following commands from the repository root:

```sh
gem install bundler
bundle install
cmake --preset acceptance
cmake --build --preset acceptance
ctest --preset acceptance -L infrastructure
ctest --preset acceptance -L '^(unit|acceptance)$'
```

Infrastructure and product suites must pass. Infrastructure fixtures do not
count as product coverage. See the [testing guide](docs/testing.md) for
prerequisites, focused runs, opt-out presets, and troubleshooting.

The codec has been tested as interoperable with the
[DFDL definition of MIL-STD-2045-47001](https://github.com/DFDLSchemas/mil-std-2045)
using Apache Daffodil 4.2.0 for C and D1 envelopes. See the
[interoperability record](docs/testing.md#interoperability-record) for the
tested scope and source revision.

Test dependencies are excluded from installation. The static target is
`vmf::vmf`. The default build also provides `vmf::schema_json`, which loads
`vmf-schema/1` JSON definitions into that same native model using pinned
yyjson 0.12.0. The core resolves profiles, defaults, scopes and controls for
both native and JSON callers; JSON is optional. Payload definitions support
VMF (UMF 2), while envelope inspection supports all documented formats.
The optional `vmf::header_json` companion loads owned native envelope
configurations, serializes inspection metadata and generates minimal templates
for exact message identities. The CLI uses this public implementation. Both
companions use the same pinned yyjson dependency. Use the `core-only` preset
to omit both companions and their JSON dependency.

# Install and consume

Install the libraries, public headers, CMake package, enabled CLI, and pinned
static yyjson dependency after building:

```sh
cmake --preset no-tests
cmake --build --preset no-tests
cmake --install build/no-tests
```

The shared prefix is `$HOME/.local`; `cmake --install build/no-tests --prefix
/path/to/prefix` overrides it. The installed tree may be moved together. Set
`CMAKE_PREFIX_PATH` to its current prefix when configuring a C11 or C++17
consumer:

```cmake
cmake_minimum_required(VERSION 3.24)
project(example LANGUAGES C)
find_package(vmf REQUIRED CONFIG COMPONENTS schema_json header_json)
add_executable(example main.c)
set_property(TARGET example PROPERTY C_STANDARD 11)
target_link_libraries(example PRIVATE
    vmf::vmf vmf::schema_json vmf::header_json)
```

A C++17 consumer may select `LANGUAGES CXX`, a `.cpp` source, and
`CXX_STANDARD 17`. Request only the JSON components needed by the application.
Enabled companions are exported under their existing target names, and their
package locates and links yyjson's separate archive transitively. A core-only
consumer uses `find_package(vmf REQUIRED CONFIG)` and `vmf::vmf`.

To install without either JSON companion, use `core-only` instead of `no-tests`
in the commands above. Its installation has no yyjson dependency and no JSON
component targets. A required component unavailable in an installation makes
`find_package` fail. CPM source builds remain supported.

# Documentation

Keep essential contracts, research, and design rationale in the repository so
contributors can understand both intended behavior and the reasons for it.

- [Public API contract](docs/api.md)
- [JSON payload schema format and loader](docs/schema-json.md)
- [Public envelope header JSON](docs/header-json.md)
- [Design rationale](docs/design.md)
- [Wire format, ambiguities and source conflicts](docs/format.md)
- [Local conformance requirements](docs/conformance.md)
- [Testing](docs/testing.md)

See [CONTRIBUTING.md](CONTRIBUTING.md) for code style, commit guidelines, and
dependency policy.

[licensing]: https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository
