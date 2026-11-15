# autofuzz harness: cpp

This is the C and C++ harness for [autofuzz](https://github.com/DustinEwan/autofuzz). It reads a
project's headers with libclang, finds the public functions and public methods that the headers
declare, builds one driver program that can call all of them, and reports what each call did: a
returned value, a guard refusal, a sanitizer report, a crash with frames, or a deadline cut. The
engine talks to it over the `afz/1` plugin protocol (one JSON object per line on stdin and stdout),
and the plugin answers every one of the twelve conformance properties with no property declared
not-applicable.

Discovery never runs your code. It parses the selected files, lowers each parameter and return type
to a value-tree shape that the engine can generate, and for every symbol that it cannot call it
writes a skip entry whose reason says why (internal linkage, a non-public member, an
unconstructible receiver, an unsupported signature).
The driver binary is built on the first `prepare`, because building is the honest way to
answer whether a call can be made; a target whose generated call does not compile is dropped by
name and its siblings keep running.

## Host runtime

The machine needs `clang++` (it builds the driver, so keep it the compiler your project builds
with), libclang (`libclang-dev` on Debian/Ubuntu), and the LLVM tools `llvm-profdata` and
`llvm-addr2line`, resolved from the `llvmBin` option, then `$AFZ_LLVM_BIN`, then `PATH`. `cmake`
and `ninja` are needed only to build this plugin itself. Linux only: the coverage design reads ELF
profiling data and the driver supervises children through `/proc`.

## Install

```sh
afz install-harness https://codeberg.org/StillDeadcode/autofuzz-harness-cpp.git
```

`afz install-harness` builds the plugin, renders the conformance fixture with this repository's own
emitter, and drives the whole suite before it records anything. The current tree passes 12 of 12
template properties with zero violations, and the fixture-side validator returns the same verdicts
on all seven source-shaped properties.

A scan project selects the harness and passes the build facts that the driver needs:

```json
{
  "harness": {
    "name": "cpp",
    "include": ["include/*.h", "src/widget.cpp"],
    "options": {
      "cxx": "clang++",
      "std": "c++20",
      "cxxFlags": ["-Iinclude"],
      "sources": ["src/widget.cpp"],
      "libraries": ["build/libwidget.a"],
      "linkFlags": ["-lpthread"],
      "sanitize": ["address", "undefined"],
      "coverage": true
    }
  }
}
```

| option | meaning |
|---|---|
| `cxx` | compiler used for the driver, the reproducer, and any `sources` (default `clang++`) |
| `std` | language standard passed to that compiler (default `c++20`) |
| `cxxFlags` | extra flags for every compile step (defines, `-I` directories) |
| `sources` | project source files compiled into the driver and the reproducer |
| `libraries` | archives linked into both; relative paths resolve against the project root |
| `linkFlags` | extra flags for the link step |
| `sanitize` | subset of `address`, `undefined`; `address` is what turns heap faults into reports |
| `coverage` | build the driver with source-based coverage (default `true`) |
| `llvmBin` | directory holding `llvm-profdata` and `llvm-addr2line` |

A key outside this table is reported, not ignored: discovery answers with a `discovery-error` skip
that reports the unknown key exactly as the file wrote it.

## How a call runs

`prepare` builds one driver binary for the whole inventory: the scaffolding units compile at `-O0`
and the project's own sources at `-O1`, both with debug info and frame pointers, so a fault frame
names the real file and line. `invoke` starts one child per call, writes the arguments on its
stdin, and reads one answer line back. A child that outlives its deadline is killed and reported as
a `deadline-cut` failure; a child that dies without answering is reported with its signal and, when
`llvm-addr2line` can resolve them, its frames. Fault frames are given relative to the project root,
so a frame inside the target's own code names the same file as the target descriptor.

`reproducer` renders a standalone `repro.cpp` that calls the target with the recorded values,
compiles it with the project's own flags, and returns the script only when that build succeeds.
Compiled reproducers are cached under the project's `.autofuzz` directory, under a file name built
from a hash of the script, so a repeated request answers instantly.

## Array parameters and count folding

C-array APIs carry their length three ways, and the driver builds each so that the
callee can never be handed fewer elements than the count it was given. A parameter
counts as a length when its name is `n`, `len`, `length`, `count`, `size`, `numel`,
`nelem`, `nbytes`, or `bytes`, as a whole name or an underscore component (`n_words`
counts; `kind` does not).

- One length in the signature: every pointer before it is an array of that many
  elements. The first one owns the count — the engine grows or shrinks it and the
  driver passes its `size()` at the count's original position. The other pointers
  are shared buffers: they keep their own fuzzed contents and the driver grows them
  to at least the owner's size at call time.
- Several lengths: each pointer pairs with the adjacent count that follows it
  (`ids`/`n` and `bits`/`n_words` in a bitmap API), and pointers before the first
  pair share the first count the same way.
- A shared buffer written before the owner's position is treated as a scatter
  destination: it is grown to the largest index the owner's elements contain, so a
  fuzzed `ids` value cannot drive a write past the end. A byte buffer after the
  count is treated as an output buffer and gets slack beyond the count, because
  encoders write more bytes than they read.

Pointers that follow a count and are not byte pointers stay scalar out-parameters
(`uint64_t* gt` receives one result, not an array). The count parameter itself is
dropped from the descriptor: the engine fuzzes array lengths, never a copy of one.

## Author

Build, test, and validate:

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build
afz validate-harness
```

The tests cover the sha256 `targetId` derivation, the option schema including its unknown-key
report, and the wire vocabulary: `tests/wire_literals.cpp` compares every literal string and error
code that the plugin can emit against the tables copied from the engine's `docs/schema/afz-1/`
schemas, with the source file recorded above each table.

`afz validate-harness` with no argument validates this checkout: it renders the conformance project
with `build/afz-cpp-emit`, drives the suite against `build/afz-cpp`, and runs
`build/afz-cpp-check` as the project's own fixture validator. The fixture (a small library with one
deliberate defect per property) is generated, not checked in, so the emitter, the plugin, and the
validator always judge the same files.

## afz-harness.json

| key | meaning |
|---|---|
| `build` | the two cmake commands that build the plugin and its helper binaries |
| `command` | the plugin argv: `build/afz-cpp --stdio` |
| `conformance.emitter` | `build/afz-cpp-emit`, run as `--out <dir>` to render the conformance project |
| `conformance.deadlineMs` | the per-invocation deadline the suite passes (2500) |

## The contract

The plugin contract lives in `docs/harness-authoring.md` in the engine repository. The two rules
that give `src/main.cpp` its form are restated at the top of that file: stdout is the protocol
channel and nothing else may write to it, and a call that outlives its deadline must be answered
before the engine cuts the process.
