# minic

[![build-and-test](https://github.com/RakshitRPathak/minic/actions/workflows/ci.yml/badge.svg)](https://github.com/RakshitRPathak/minic/actions/workflows/ci.yml)

A small compiler for a tiny imperative language, written in C++17 with no dependencies.
It goes through the same stages as a production compiler: lexing, parsing, semantic
analysis, optimization, code generation and execution on a virtual machine.

```
source ─► tokens ─► AST ─► symbol table ─► optimized AST ─► bytecode ─► VM
         lexer.hpp  parser.hpp  sema.hpp    optimizer.hpp   codegen.hpp   vm.hpp
```

## The language

```
let x = 10;                       // declare (once)
x = x * 2 + 3;                    // assign
print x;                          // output
if (x > 5) { print 1; } else { print 0; }
while (x > 0) { x = x - 1; }
// comments run to end of line
```

Types: 64-bit signed integers only. Operators: `+ - * /`, unary `-`, and
`< > <= >= == !=` (which yield 1 or 0). A condition is true when non-zero.
Integer arithmetic wraps on overflow; division truncates toward zero.

## Build and run

```bash
cmake -S . -B build && cmake --build build
build/minic examples/hello.mc               # compile and run
build/minic prog.mc --disasm --stats        # see the bytecode and optimizer stats
ctest --test-dir build --output-on-failure  # or: tests/run_tests.sh build/minic
```

Options: `--dump-tokens`, `--dump-ast`, `--dump-opt`, `--disasm`, `--no-opt`,
`--no-run`, `--stats`, `--max-steps N`.

Exit codes: `0` success, `1` compile error, `2` runtime error, `64` bad usage.

## What each stage does

| Stage | File | Idea |
|---|---|---|
| Lexer | `lexer.hpp` | Hand-written scanner. Looks one character ahead to tell `<` from `<=`. Rejects integer literals that overflow. |
| Parser | `parser.hpp` | Recursive descent, one function per precedence level. Tighter-binding operators sit deeper in the call chain, which is what makes `2 + 3 * 4` parse correctly. |
| AST | `ast.hpp` | Tree of nodes owned by `std::unique_ptr`, so memory is freed automatically. |
| Sema | `sema.hpp` | Checks every variable is declared before use and assigns each one a numbered slot. Runs *before* the optimizer so deleting dead code can never change which programs are accepted. |
| Optimizer | `optimizer.hpp` | Constant folding, dead-branch elimination (`if (0)`), dead-loop elimination (`while (0)`). |
| Code generator | `codegen.hpp` | Emits stack-machine bytecode. Uses **backpatching** for jump targets. |
| VM | `vm.hpp` | Fetch-decode-execute loop with a value stack. Checks for stack underflow, division by zero and runaway loops. |

### Backpatching in one example

```
let x = 3;
if (x > 2) { print x * 2; } else { print 0; }
```
compiles to
```
  0  PUSH 3
  1  STORE 0          ; x
  2  LOAD 0           ; x
  3  PUSH 2
  4  GT
  5  JMP_IF_FALSE 11  ; jump to the else-branch
  6  LOAD 0           ; x
  7  PUSH 2
  8  MUL
  9  PRINT
 10  JMP 13           ; skip the else-branch
 11  PUSH 0
 12  PRINT
 13  HALT
```
When instruction 5 was emitted, the else-branch did not exist yet, so its target was
unknown. The code generator emitted `JMP_IF_FALSE -1`, remembered index 5, and patched
it to 11 once the then-branch was finished. Assemblers and linkers solve the same
problem with labels and relocations.

### Why the optimizer is safe

* Constant folding calls the same `evalBinary()` the VM uses, so a value folded at
  compile time is always identical to the value the VM would compute.
* `1 / 0` is deliberately **not** folded: the program must still fail at run time,
  exactly as it does without optimization.
* The test suite runs every program twice, with and without `--no-opt`, and requires
  identical output.

## Testing

`tests/run_tests.sh` runs 43 checks:

* 14 programs (arithmetic, loops, primes, Collatz, GCD, overflow, dead code...)
  each executed optimized and unoptimized,
* 12 error cases checking both the exit code and the exact message with its line number,
* assertions that the optimizer really shrinks the bytecode and counts what it removed.

The suite also passes under AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
g++ -std=c++17 -g -fsanitize=address,undefined src/main.cpp -o build/minic_san
tests/run_tests.sh build/minic_san
```

## Design notes and limitations

* **Stack VM, not register VM.** A stack machine is simpler to generate code for.
  NVIDIA's PTX is register based (unlimited virtual registers that `ptxas` allocates
  to real ones), so a natural next step is a register-based back end with a
  register allocator.
* **Flat scope.** One scope for the whole program; `let` inside a loop re-initializes
  the variable each iteration. Per-block scopes are a good extension.
* **Uninitialised reads are 0.** A variable whose declaration was never executed
  (for example inside a skipped `if`) reads as 0.
* No functions, strings or arrays yet.

## Ideas to extend it

1. Functions with a call stack and local variables.
2. A register-based IR (three-address code) plus a linear-scan register allocator.
3. More optimizations: algebraic simplification (`x * 1`), common subexpression
   elimination, and a peephole pass over the bytecode.
4. Emit a real object format and write a tiny linker that resolves symbols.
5. Type checking with `int` and `bool`.
