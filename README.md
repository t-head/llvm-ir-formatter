# LLVM IR Formatter

A command-line tool that converts opaque pointers to typed pointers and rewrites `memory(...)` attributes for compatibility with older LLVM toolchains.

For example, [triton-for-sail](https://github.com/t-head/triton-for-sail/blob/235464c50eb706e09b981198c1117b5a5e6c91b2/third_party/ppu/backend/compiler.py#L471) uses it to prepare LLVM IR for PPU code generation.

## Build

Run from the repository root:

```sh
./build.sh
```

The script builds all default targets in Release mode and runs the formatter tests. The executable is `build/bin/llvm-irformatter`; no separate LLVM installation is required.

## Usage

Write converted textual IR:

```sh
build/bin/llvm-irformatter -S input.ll -o output.ll
```

Omit `-S` to write **bitcode**, regardless of the output extension:

```sh
build/bin/llvm-irformatter input.ll -o output.bc
```

Input supports textual IR and compatible bitcode. Input and output default to standard input and standard output; `-` selects either explicitly:

```sh
build/bin/llvm-irformatter -S < input.ll > output.ll
```

| Option | Default | Description |
| --- | --- | --- |
| `-S` | Off | Write textual IR instead of bitcode |
| `-o <filename>` | `-` | Output file; `-` means standard output |
| `--type-pointers=<bool>` | `true` | Convert pointers and apply related compatibility handling |
| `--trans-memory-attributes=<bool>` | `true` | Convert `memory(...)` to legacy attributes |
| `--verify-pointers` | Off | Check remaining opaque pointers; not full IR verification |
| `-f` | Off | Allow binary output to a terminal |

Use `=false` to disable boolean options. `--help-hidden` includes `-S` and use-list order options omitted from `--help`.

Conversion may discard unsupported information; always validate the output with your target toolchain.

## Testing

`./build.sh` runs the formatter tests automatically. To rerun them after building:

```sh
build/bin/llvm-lit -v llvm/test/tools/llvm-irformatter
```

Tests cover pointers, attributes, and syntax compatibility, including `llvm18_dev/` and `mlirTrans/`. The latter contains LLVM IR, not MLIR text.

## Source layout

| Path | Purpose |
| --- | --- |
| [`llvm/tools/llvm-irformatter/`](llvm/tools/llvm-irformatter/) | CLI and conversion pipeline |
| [`TypingPointers.cpp`](llvm/lib/Transforms/Utils/TypingPointers.cpp) | Pointer conversion and checks |
| [`TransMemoryAttr.cpp`](llvm/lib/Transforms/Utils/TransMemoryAttr.cpp) | Memory-attribute conversion |
| [`llvm/lib/AsmParser/`](llvm/lib/AsmParser/) | Parsing and syntax compatibility |
| [`AutoUpgrade.cpp`](llvm/lib/IR/AutoUpgrade.cpp) | Intrinsic compatibility |
| [`llvm/test/tools/llvm-irformatter/`](llvm/test/tools/llvm-irformatter/) | Regression tests |
