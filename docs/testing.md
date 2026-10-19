# Testing

GoogleTest checks public API contracts. Cucumber describes observable behavior
and workflows in Gherkin, with Cucumber-Cpp steps calling the public software
interfaces. The VMF API delivery includes public-contract unit tests and
executable acceptance scenarios. Populated product suites must pass, including
public examples decoded as fields and a public independent C envelope fixture. Separate infrastructure regressions
verify runner behavior and must pass. No product tests are marked pending,
skipped, or expected to fail in the runner configuration. Cucumber naturally
omits later steps after an earlier step fails; this is not a skip annotation.

## Placement and maintenance

Put unit test sources under `tests/unit/`. Cover normal results, contract
boundaries, invalid inputs where specified, error results, and ownership or
cleanup obligations. Add coverage when an API contract changes and regression
tests when fixing a bug.

Put Gherkin `.feature` files under `features/` and compiled native step
definitions under `tests/acceptance/`. Add scenarios for new or newly clarified
observable behavior and meaningful workflows. Describe outcomes using the
language of a caller or user. Keep each scenario focused on one behavior;
avoid coupling scenarios to private implementation details.

Revise expectations when intended behavior changes. Implementation refactors
retain existing expectations, and bugs require fixing the implementation.
Remove coverage when behavior is intentionally retired or equivalent coverage
replaces it. Explain the reason in the change and remove unused steps and
fixtures together with the retired coverage.

## Build selections

Use the shared CMake presets from the repository root. Each selection has
matching configure, build, and test presets and its own build directory:

| Preset | Build directory | Test selection |
| --- | --- | --- |
| `default` | `build` | Unit tests |
| `core-only` | `build/core-only` | Core unit tests; both JSON companions disabled |
| `acceptance` | `build/acceptance` | Both product suites and infrastructure |
| `acceptance-only` | `build/acceptance-only` | Acceptance and infrastructure |
| `no-suites` | `build/no-suites` | Both suite options off |
| `no-tests` | `build/no-tests` | Test infrastructure off |

All test options are independent under CMake's standard master switch:

| Option | Default | Purpose |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Enable test infrastructure and dependencies |
| `BUILD_UNIT_TESTS` | `ON` | Build and discover GoogleTest unit cases |
| `BUILD_ACCEPTANCE_TESTS` | `OFF` | Build and run Cucumber acceptance tests |

The independent library options `BUILD_SCHEMA_JSON` and `BUILD_HEADER_JSON`
default to `ON`. They build `vmf::schema_json` and `vmf::header_json` respectively.
Either companion fetches pinned yyjson 0.12.0 even when tests are disabled.
Disable both to omit the parser dependency; the `core-only` preset does this.
The CLI requires both companions. Each companion's public unit cases are
included only when that companion is enabled.

Acceptance testing excludes scenarios tagged `@schema_json` or `@header_json`
when the corresponding companion is disabled. Other acceptance scenarios still
run. A manual launcher must pass `--tags 'not @schema_json'` and/or
`--tags 'not @header_json'` for the disabled companions. The header companion
also works independently of payload schema JSON; see the
[header JSON guide](header-json.md) for its public configuration contract.

`BUILD_TESTING=OFF` overrides all suite options. With unit and acceptance
options off, no test dependencies are fetched. GoogleTest is fetched
whenever unit or acceptance testing is enabled, because native Cucumber steps
also use its assertions. Cucumber
and its dependencies are fetched only for acceptance tests. All dependencies
use pinned CPM packages and are excluded from installation.

Infrastructure regressions are enabled with acceptance testing and use the
same prerequisites. They have the `infrastructure` label, which is separate
from product labels `unit` and `acceptance`. The default configuration remains
Ruby-free. The `no-suites` and `no-tests` presets explicitly allow no CTest
entries; the other shared test presets fail when a filter selects no entries.

Library code and test tooling use C++17, with C11-compatible public headers.
Test code may follow GoogleTest and Cucumber framework conventions as
described in [CONTRIBUTING.md](../CONTRIBUTING.md).

## Local customization

Use the ignored `CMakeUserPresets.json` for special local environment options
or one-off tests. Inherit from a shared configure preset, choose a unique name,
and set a separate `binaryDir` such as `${sourceDir}/build-local`. Add matching
build and test presets that reference the local configure preset. Test presets
can inherit shared settings such as failure output, but must override
`configurePreset` to select the local build directory. Do not commit this file.

Set local compiler paths, generators, cache variables, and environment options
in user presets instead of passing configuration overrides on the command
line. For a multi-configuration generator, set the same `configuration` value,
such as `Debug`, in the local build and test presets. Use executables under the
corresponding configuration subdirectory. Set `CMAKE_INSTALL_PREFIX` in a user
configure preset to override the shared `$HOME/.local` installation prefix.

Add, remove, or alter supported project configurations in `CMakePresets.json`
and update their documented commands, as required by
[CONTRIBUTING.md](../CONTRIBUTING.md).

## Unit tests

Install CMake 3.24 or newer, Git, and a C/C++ toolchain supporting C++17 for
library sources and tests. Initial dependency downloads require network
access. Ruby and Bundler are unnecessary for the default configuration:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
ctest --preset default -L unit
```

An empty `unit_tests` runner fails normal execution. CTest registers a failing
`unit_empty` entry when GoogleTest discovery finds zero cases, even when other
suites have tests. After adding cases, `gtest_discover_tests` automatically
discovers them, applies the `unit` label, and removes the empty-suite fallback.
Listing remains valid with no cases. List or focus cases with CTest or GoogleTest:

```sh
ctest --preset default -N
ctest --preset default -L unit -R 'SuiteName.CaseName'
./build/tests/unit/unit_tests --gtest_list_tests
./build/tests/unit/unit_tests --gtest_filter='SuiteName.*'
```

For a multi-configuration generator, use local build and test presets with
matching `configuration` values, as described under local customization.

## Acceptance tests

Install Ruby and Bundler, then install the locked gems from the repository
root. Also install `patch`, which CPM uses for CMake build integration patches.
The Gemfile pins Cucumber 7.1.0 and cucumber-wire 6.2.1, matching
[Cucumber-Cpp 0.9.0's Gemfile][upstream-gems]. Use a Ruby version compatible
with these gems; the tooling has been verified with Ruby 3.3.6 and Bundler
2.6.9. Ruby and Bundler must be available on `PATH` during configuration.
To select other installations, set the `RUBY_EXECUTABLE` and
`BUNDLER_EXECUTABLE` cache variables in a user configure preset. Both must
belong to the same Ruby installation.

```sh
gem install bundler
bundle install
cmake --preset acceptance
cmake --build --preset acceptance
ctest --preset acceptance
ctest --preset acceptance -L acceptance
```

To keep installed gems local, configure Bundler before installing:

```sh
bundle config set --local path vendor/bundle
bundle install
```

Those directories are ignored by Git. Keep the resolved `Gemfile.lock` in
version control. Configuration checks
prerequisites and the installed bundle without installing gems. If setup is
missing, follow the reported install command and configure again.

CTest registers one suite named `acceptance`, labeled `acceptance`. The Ruby
launcher starts `acceptance_steps`, using the upstream Cucumber-Cpp server,
bound to `127.0.0.1` on an ephemeral port. It waits up to ten seconds for the
server's readiness output, passes the selected port to the wire configuration,
and runs Cucumber in strict mode. Zero selected scenarios and undefined,
pending, or failed steps fail the suite. Cucumber support glue counts selected
scenarios using its event hooks, including after feature and tag filters.
The launcher propagates the result and stops its child processes on
completion, startup failure, or an interrupt. It escalates from termination
to forced cleanup after two seconds if necessary. Each invocation gets its
own server and port.

Use the launcher for feature paths, line selectors, and tag filters. Run these
commands from the repository root, substituting an existing feature and tag:

```sh
ruby tests/acceptance/run.rb build/acceptance/tests/acceptance/acceptance_steps
ruby tests/acceptance/run.rb \
	build/acceptance/tests/acceptance/acceptance_steps \
	features/workflow.feature
ruby tests/acceptance/run.rb \
	build/acceptance/tests/acceptance/acceptance_steps \
	features/workflow.feature:12
ruby tests/acceptance/run.rb \
	build/acceptance/tests/acceptance/acceptance_steps --tags '@workflow'
```

Additional Cucumber arguments are forwarded; strict mode remains enabled.
The launcher loads repository support glue and wire definitions even for
focused runs. For a multi-configuration generator, use the server executable
under `tests/acceptance/Debug/` after a Debug build.

[upstream-gems]: https://github.com/cucumber/cucumber-cpp/blob/v0.9.0/Gemfile

## Infrastructure regressions

After configuring and building an acceptance-enabled preset, run the permanent
runner checks from the repository root:

```sh
ctest --preset acceptance -L infrastructure
ctest --preset acceptance -L '^(unit|acceptance)$'
```

Both commands must pass. Running `ctest --preset acceptance` executes both
groups. CI runs infrastructure checks first, then product suites, for each
compiler entry. An enabled suite with no selected cases remains an error.

Fixtures under `tests/infrastructure/` exercise real native wire steps and
controlled startup, exit, and process-tree behavior. The harness also checks
empty and populated unit discovery using an isolated CMake project. Acceptance
runs use temporary copies of the launcher, bundle manifest, and support glue,
so added product features and hooks do not affect these fixtures. Fixture
cases and features are not registered in the product suites. Temporary files,
ports, children, and descendants are cleaned up after each check, including
failure. The harness uses Ruby's standard library and the existing native test
dependencies.

Checks cover successful and focused execution, custom formatting, strict step
failures, empty selections, missing executables and gems, early exit, startup
timeout, excessive readiness output, server and consumer failures, forced
cleanup, interruption, and concurrent isolation. Repeated fixture invocations
also exercise port reuse. On POSIX, interruption uses an actual `SIGINT`. On
Windows, where CI may lack a console, a fixture raises the same exception as
the launcher's signal handler; delivery of console events remains environment
dependent.

If a regression fails, inspect its diagnostic and CTest's captured output.
Rebuild after modifying native fixtures and rerun the infrastructure label.
Avoid moving fixture cases into product directories to make empty suites pass.

## Common setup issues

- If configuration reports missing Ruby or Bundler, install them and ensure
  both executables are on `PATH`, or select matching paths in a user preset.
- If configuration or the launcher reports unavailable gems, run
  `bundle install` with the selected Ruby and Bundler, then configure again.
- If a unit suite contains no cases, register real product unit sources and
  rebuild. If acceptance selects no scenarios, add product features or correct
  the focused path, line selector, or tag filter.
- If a shared preset selects no CTest entries, check the label or regular
  expression. Use the explicit opt-out presets when intentionally disabling
  product testing.

## Opting out

Build the core without the JSON companion, build acceptance tests alone,
disable units, or disable all tests:

```sh
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only

cmake --preset acceptance-only
cmake --build --preset acceptance-only
ctest --preset acceptance-only -L acceptance

cmake --preset no-suites
cmake --build --preset no-suites
ctest --preset no-suites

cmake --preset no-tests
cmake --build --preset no-tests
ctest --preset no-tests
```

The presets explicitly set the test selections on each configuration run.
Disabling acceptance later does not delete dependency sources already
downloaded in that build directory.

## Contributor workflow

Register each new `.cpp` file explicitly with `target_sources(unit_tests
PRIVATE ...)` in `tests/unit/CMakeLists.txt`, or with
`target_sources(acceptance_steps PRIVATE ...)` in
`tests/acceptance/CMakeLists.txt`. Rebuild to discover new unit cases. Gherkin
files need no CMake registration: Cucumber finds them under `features/` or
through a focused path. VMF product cases cover schemas, allocation, envelopes, exact selection,
streaming, replay, measurement and failure outputs. Literal vectors live under
`tests/acceptance/`; they are separate from infrastructure fixtures.

As software targets are added under `src/`, link the software under test with
`target_link_libraries(unit_tests PRIVATE your_library)` and
`target_link_libraries(acceptance_steps PRIVATE your_library)`. Use public
headers and interfaces rather than compiling a second copy of implementation
sources into the test runners. Register native steps directly as executable
sources so the linker retains their static registrations.

For native steps using GoogleTest assertions, include `<gtest/gtest.h>` before
`<cucumber-cpp/autodetect.hpp>` so Cucumber selects its GoogleTest driver.
Use Cucumber scenario contexts and before/after hooks to establish and clean
up each scenario's resources. Reset state between scenarios, use unique
temporary paths, and release files, handles, and processes even on failure.
Scenarios must work alone and in any order. Share reusable steps without
depending on another scenario's actions or fixtures.

Before review, follow the checks in [CONTRIBUTING.md](../CONTRIBUTING.md).
Run both suites for affected behavior and inspect the discovered test and
scenario counts. For runner or build changes, exercise the `default`,
`acceptance`, `acceptance-only`, `no-suites`, and `no-tests` selections,
failure results, focused runs, repeated runs, missing
prerequisites, startup failures, interrupts, and server cleanup.

## Interoperability record

The codec has been tested as interoperable with the
[DFDL definition of MIL-STD-2045-47001](https://github.com/DFDLSchemas/mil-std-2045)
at commit `b1a489122f2d18bd3a329009d2a8c5f0ba0f0616`, using Apache Daffodil
4.2.0. The successful checks covered the upstream C parser/unparser TDML cases
`test_2045_C_msghdr1` and `test_2045_C_msghdr1_unparsed`, plus synthetic C and
D1 binary-file PDUs in both directions: Daffodil parsed library output into
the expected infosets, unparsed it to matching literal bytes, and the library
decoded the independent output through its public API.

The interoperability harness has been removed; this records completed testing
before the native schema API revision. The current public payload schema API
accepts VMF definitions; historical binary-file frames remain supported by
schema-independent envelope inspection.
The standard unit suite retains the independent C fixture and field-level
public examples in `tests/unit/`.
Java and Daffodil are not build or test dependencies.

The checks treated length-bounded bodies as opaque binary data and supplied
no MIL-STD-6017 definitions. They establish C/D1 envelope interoperability
for the tested fixtures, without validating tactical meaning, signatures or a
fielded message set. B/D tests use primary standard-based literals and do not
establish DFDL interoperability. See [format.md](format.md) for fixture details
and source conflicts.
