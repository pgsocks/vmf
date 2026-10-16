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
Use the `core-only` preset to omit this companion and its JSON dependency.
Installation rules are not yet provided. The shared installation prefix is
`$HOME/.local`.

# Documentation

Keep essential contracts, research, and design rationale in the repository so
contributors can understand both intended behavior and the reasons for it.

- [Public API contract](docs/api.md)
- [JSON payload schema format and loader](docs/schema-json.md)
- [Design rationale](docs/design.md)
- [Wire format, ambiguities and source conflicts](docs/format.md)
- [Local conformance requirements](docs/conformance.md)
- [Testing](docs/testing.md)

See [CONTRIBUTING.md](CONTRIBUTING.md) for code style, commit guidelines, and
dependency policy.

[licensing]: https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository
