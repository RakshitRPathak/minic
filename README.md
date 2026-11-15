# minic

[![build-and-test](https://github.com/RakshitRPathak/minic/actions/workflows/ci.yml/badge.svg)](https://github.com/RakshitRPathak/minic/actions/workflows/ci.yml)

A small compiler for a tiny imperative language, written in C++17 with no dependencies.
It goes through the same stages as a production compiler: lexing, parsing, semantic
analysis, optimization, code generation and execution on a virtual machine. A second
back half lowers the program to a three-address **SSA IR** with a control-flow graph
and runs the classic optimizations on it.

```
source ─► tokens ─► AST ─► symbol table ─► optimized AST ─┬─► bytecode ─► VM
         lexer.hpp  parser.hpp  sema.hpp    optimizer.hpp  │   codegen.hpp   vm.hpp
                                                           │
                                                           └─► SSA IR ─► optimized IR ─► IR interpreter   (--ir)
                                                               ir.hpp    iropt.hpp       irvm.hpp
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
build/minic prog.mc --dump-ir --no-run      # see the optimized SSA IR
build/minic prog.mc --ir                    # run through the SSA IR instead of the bytecode
tests/fuzz.py build/minic 1000              # differential fuzzing (needs python3)
ctest --test-dir build --output-on-failure  # or: tests/run_tests.sh build/minic
```

Options: `--dump-tokens`, `--dump-ast`, `--dump-opt`, `--disasm`, `--ir`, `--dump-ir`,
`--no-opt`, `--no-run`, `--stats`, `--max-steps N`.

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
| SSA IR | `ir.hpp` | Three-address code in SSA form over a control-flow graph, built straight from the AST. See [The SSA IR](#the-ssa-ir). |
| IR optimizer | `iropt.hpp` | Constant propagation, branch folding, phi simplification, global value numbering over the dominator tree, dead code elimination, block merging. |
| IR interpreter | `irvm.hpp` | Runs the IR, evaluating all phis of a block together on entry. Used to check the IR against the bytecode VM. |

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

## The SSA IR

`ir.hpp` lowers the AST into three-address code in SSA form: basic blocks in a
control-flow graph, every value assigned once, and phis where paths join. It is built
directly in SSA with the on-the-fly algorithm of Braun et al. (2013), so no dominance
frontiers are needed. This is the loop `while (i <= 100) { sum = sum + i; i = i + 1; }`:

```
main:
 b0:
  v0 = const 0
  v1 = const 1
  jmp b1
 b1:    ; from b0 b2
  v2 = phi [b0: v1], [b2: v7]
  v3 = phi [b0: v0], [b2: v6]
  v4 = const 100
  v5 = le v2, v4
  br v5, b2, b3
 b2:    ; from b1
  v6 = add v3, v2
  v7 = add v2, v1
  jmp b1
 b3:    ; from b1
  print v3
  ret v0
```

Function parameters and `let` variables become SSA values. A global that some function
reads or writes stays in memory (`load_global` / `store_global`), because a call may
change it; a global that no function touches is promoted to an SSA value of `main`.

`iropt.hpp` runs these passes to a fixed point:

| Pass | What it does |
|---|---|
| Constant propagation and folding | Constants flow through variables and across blocks, which the AST folder cannot do. Includes identities such as `x * 1`, `x + 0`, `x - x`. |
| Branch folding | A branch on a known condition becomes a jump; blocks nothing reaches any more are deleted. |
| Phi simplification | A phi whose operands are all the same value is replaced by that value. |
| Global value numbering | A computation is replaced by an identical earlier one that dominates it (common subexpression elimination). Walks the dominator tree, computed with the Cooper-Harvey-Kennedy algorithm. |
| Dead code elimination | Removes anything unused that has no effect. |
| Block merging | Joins a block to its only successor when that successor has no other predecessor. |

Calls, prints, stores to globals and any division that could be a division by zero are
never removed or reordered, so a program fails in the same place with the same output.

`irvm.hpp` interprets the IR. It is there to check the IR, not to be fast: there is no
register allocator or machine back end yet.

## Testing

`tests/run_tests.sh` runs 119 checks:

* 20 programs (arithmetic, loops, primes, Collatz, GCD, overflow, dead code, functions,
  recursion, mutual recursion, scoping, return forms...) each executed four ways:
  bytecode VM and SSA IR interpreter, optimized and unoptimized,
* 23 error cases checking both the exit code and the exact message with its line number;
  the 5 runtime errors are also checked through the IR,
* assertions that the optimizer really shrinks the bytecode, counts what it removed and
  never removes a call,
* assertions about the optimized IR: constants reach a branch and decide it, a repeated
  `x * x + 1` is computed once, the sum loop has exactly 2 phis.

`tests/fuzz.py` is a differential fuzzer. It generates random valid programs (functions,
loops, globals, repeated subexpressions, divisions that sometimes fail) and requires the
same output and exit code from all four modes. CI runs 1,000 programs on every push.

The fuzzer was checked by planting bugs in the IR code by hand and confirming it reports
each one: deleting a division that could fail, folding `x / x` to 1, treating `-` as
commutative in value numbering, keeping the wrong side of a folded branch, removing a phi
whose operands differ, promoting a global that a function writes, and writing a
block's phis one at a time instead of together. The first version
of the fuzzer missed the value-numbering bug, because random expressions almost never
repeat; it now reuses earlier subexpressions on purpose.

The suite also passes under AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
g++ -std=c++17 -g -fsanitize=address,undefined src/main.cpp -o build/minic_san
tests/run_tests.sh build/minic_san
tests/fuzz.py build/minic_san 300
```

## Design notes and limitations

* **Stack VM, not register VM.** A stack machine is simpler to generate code for.
  NVIDIA's PTX is register based (unlimited virtual registers that `ptxas` allocates
  to real ones). The SSA IR is the first half of that: it has unlimited virtual
  registers too, but it is only interpreted. Lowering it out of SSA and allocating
  registers is the next step.
* **The IR optimizer is deliberately small.** No loop-invariant code motion, no
  inlining, no reasoning about memory (two loads of the same global are not merged),
  and constant propagation is not conditional (SCCP would find more).
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

1. Lower the IR out of SSA (phi elimination with parallel copies), then a linear-scan
   register allocator and a register-machine or x86-64 back end.
2. More IR passes: loop-invariant code motion, sparse conditional constant propagation,
   inlining, redundant-load elimination.
3. Emit a real object format and write a tiny linker that resolves symbols.
4. Type checking with `int` and `bool`.

## License

MIT. See [LICENSE](LICENSE).
