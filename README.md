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

fn fact(n) {                      // functions: parameters, locals, recursion
    if (n <= 1) { return 1; }
    return n * fact(n - 1);
}
print fact(10);                   // a call is an expression...
fact(3);                          // ...or a statement (result discarded)
```

Types: 64-bit signed integers only. Operators: `+ - * /`, unary `-`, and
`< > <= >= == !=` (which yield 1 or 0). A condition is true when non-zero.
Integer arithmetic wraps on overflow; division truncates toward zero.

Functions are declared at the top level with `fn` and can be called before their
definition, so mutual recursion works. A function sees its own parameters and `let`
variables first, then any global declared above it. `return;` and falling off the end
both return 0.

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
| Sema | `sema.hpp` | Checks every variable and function is declared, checks call arity, and resolves each variable to a global slot or a slot in its function's frame. Runs *before* the optimizer so deleting dead code can never change which programs are accepted. |
| Optimizer | `optimizer.hpp` | Constant folding, dead-branch elimination (`if (0)`), dead-loop elimination (`while (0)`), removal of statements after a `return`. Calls are never folded or removed, because a function can print or write a global. |
| Code generator | `codegen.hpp` | Emits stack-machine bytecode. Uses **backpatching** for jump targets. Function bodies follow the top-level code; `CALL` names a function-table index, so calls need no patching. |
| VM | `vm.hpp` | Fetch-decode-execute loop with a value stack and a stack of call frames. Checks for stack underflow, division by zero, runaway loops and runaway recursion (depth limit 10,000). |

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

`tests/run_tests.sh` runs 66 checks:

* 19 programs (arithmetic, loops, primes, Collatz, GCD, overflow, dead code, functions,
  recursion, mutual recursion, scoping, return forms...) each executed optimized and
  unoptimized,
* 23 error cases checking both the exit code and the exact message with its line number,
* assertions that the optimizer really shrinks the bytecode, counts what it removed and
  never removes a call.

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
* **Flat scope.** One global scope plus one scope per function; blocks do not open new
  scopes. `let` inside a loop re-initializes
  the variable each iteration. Per-block scopes are a good extension.
* **Uninitialised reads are 0.** A variable whose declaration was never executed
  (for example inside a skipped `if`) reads as 0.
* **Calls.** Arguments are evaluated left to right and passed by value. Each call gets a
  fresh frame, so recursion works; functions are not values and cannot be nested.
  Functions and variables have separate namespaces.
* No strings or arrays yet.

## Ideas to extend it

1. A register-based IR (three-address code) plus a linear-scan register allocator.
2. More optimizations: algebraic simplification (`x * 1`), common subexpression
   elimination, and a peephole pass over the bytecode.
3. Emit a real object format and write a tiny linker that resolves symbols.
4. Type checking with `int` and `bool`.

## License

MIT. See [LICENSE](LICENSE).
