#!/usr/bin/env python3
"""Differential fuzzer for minic.

Generates random valid programs and runs each one four ways:

    bytecode VM, optimized        bytecode VM, --no-opt
    SSA IR interpreter, optimized SSA IR interpreter, --no-opt

All four must print the same output and exit with the same code. A program that
divides by zero must do so in all four, after printing the same lines: that is
what catches an optimizer that removes or reorders a failing division.

    tests/fuzz.py build/minic            # 300 programs, seed 1
    tests/fuzz.py build/minic 2000 42    # count, seed

Programs always terminate: loops count up to a small constant with a counter
the body cannot assign, and a function may only call functions defined before it.
"""
import random
import subprocess
import sys
import tempfile

MODES = [[], ["--no-opt"], ["--ir"], ["--ir", "--no-opt"]]
# "/" is rarer than the rest so that most programs run to the end.
BINOPS = ["+", "+", "-", "-", "*", "*", "/", "<", "<", ">", "<=", ">=", "==", "==", "!="]


class Gen:
    def __init__(self, rng):
        self.rng = rng
        self.counter = 0
        self.funcs = []       # (name, nparams) defined so far
        self.globals = []     # globals visible to functions (declared before them)

    def fresh(self, prefix):
        self.counter += 1
        return f"{prefix}{self.counter}"

    def expr(self, scope, depth=0):
        r = self.rng
        roll = r.random()
        if depth > 3 or roll < 0.25:
            # Small constants make 0, 1 and equal operands likely, which is
            # where the algebraic identities and division by zero live.
            return str(r.choice([0, 0, 1, 1, 2, 3, 5, 7, 10, 100, 9223372036854775807]))
        if roll < 0.55 and scope["vars"]:
            return r.choice(scope["vars"])
        if roll < 0.62:
            return f"(-{self.expr(scope, depth + 1)})"
        if roll < 0.72 and scope["callable"]:
            name, nparams = r.choice(scope["callable"])
            args = ", ".join(self.expr(scope, depth + 1) for _ in range(nparams))
            return f"{name}({args})"
        # Repeat an earlier expression, sometimes with its operands swapped. Random
        # expressions almost never coincide, and common-subexpression elimination
        # is only exercised (and only goes wrong) when they do.
        if roll < 0.82 and scope["seen"]:
            lhs, op, rhs = r.choice(scope["seen"])
            if r.random() < 0.5:
                lhs, rhs = rhs, lhs
            return f"({lhs} {op} {rhs})"
        op = r.choice(BINOPS)
        lhs, rhs = self.expr(scope, depth + 1), self.expr(scope, depth + 1)
        if len(scope["seen"]) < 20:
            scope["seen"].append((lhs, op, rhs))
        return f"({lhs} {op} {rhs})"

    def block(self, scope, depth, indent):
        return "".join(self.stmt(scope, depth, indent) for _ in range(self.rng.randint(1, 4)))

    def stmt(self, scope, depth, indent):
        r = self.rng
        pad = "    " * indent
        roll = r.random()
        if roll < 0.22:
            value = self.expr(scope)
            name = self.fresh("v")
            scope["vars"].append(name)  # flat scope: visible for the rest of the function
            scope["assignable"].append(name)
            return f"{pad}let {name} = {value};\n"
        if roll < 0.42 and scope["assignable"]:
            return f"{pad}{r.choice(scope['assignable'])} = {self.expr(scope)};\n"
        if roll < 0.62:
            return f"{pad}print {self.expr(scope)};\n"
        if roll < 0.67 and scope["callable"]:
            name, nparams = r.choice(scope["callable"])
            args = ", ".join(self.expr(scope) for _ in range(nparams))
            return f"{pad}{name}({args});\n"
        if roll < 0.72 and scope["in_function"]:
            return f"{pad}return {self.expr(scope)};\n"
        if depth >= 3:
            return f"{pad}print {self.expr(scope)};\n"
        if roll < 0.88:
            out = f"{pad}if ({self.expr(scope)}) {{\n{self.block(scope, depth + 1, indent + 1)}{pad}}}"
            if r.random() < 0.5:
                out += f" else {{\n{self.block(scope, depth + 1, indent + 1)}{pad}}}"
            return out + "\n"
        counter = self.fresh("i")
        scope["vars"].append(counter)  # readable, but never assigned by the body
        limit = r.randint(0, 4)
        body = self.block(scope, depth + 1, indent + 1)
        return (f"{pad}let {counter} = 0;\n{pad}while ({counter} < {limit}) {{\n{body}"
                f"{pad}    {counter} = {counter} + 1;\n{pad}}}\n")

    def program(self):
        r = self.rng
        src = ""
        for _ in range(r.randint(0, 3)):  # globals that functions can see
            name = self.fresh("g")
            src += f"let {name} = {r.choice([0, 1, 2, 5])};\n"
            self.globals.append(name)
        for _ in range(r.randint(0, 3)):
            name = self.fresh("f")
            params = [self.fresh("p") for _ in range(r.randint(0, 3))]
            scope = {"vars": params + self.globals, "assignable": params + self.globals,
                     "callable": list(self.funcs), "in_function": True, "seen": []}
            src += f"fn {name}({', '.join(params)}) {{\n{self.block(scope, 0, 1)}}}\n"
            self.funcs.append((name, len(params)))
        scope = {"vars": list(self.globals), "assignable": list(self.globals),
                 "callable": list(self.funcs), "in_function": False, "seen": []}
        for _ in range(r.randint(2, 8)):
            src += self.stmt(scope, 0, 0)
        for name in scope["vars"]:
            src += f"print {name};\n"
        return src


def run(minic, path, flags):
    p = subprocess.run([minic, path, "--max-steps", "2000000"] + flags,
                       capture_output=True, text=True, timeout=60)
    # Error text names a bytecode offset or an IR function, so compare only its kind.
    kind = "division by zero" if "division by zero" in p.stderr else p.stderr.strip()[:40]
    return p.stdout, p.returncode, kind if p.returncode else ""


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    minic = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rng = random.Random(seed)
    trapped = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = f"{tmp}/prog.mc"
        for n in range(count):
            src = Gen(rng).program()
            with open(path, "w") as f:
                f.write(src)
            results = [run(minic, path, flags) for flags in MODES]
            if results[0][1] not in (0, 2):
                print(f"program {n} (seed {seed}) was rejected or crashed:\n{src}\n{results[0]}")
                sys.exit(1)
            trapped += results[0][1] == 2
            for flags, res in zip(MODES[1:], results[1:]):
                if res != results[0]:
                    print(f"MISMATCH on program {n} (seed {seed}), flags {flags or '(default)'}")
                    print(f"--- source ---\n{src}")
                    print(f"--- default: exit {results[0][1]} {results[0][2]}\n{results[0][0]}")
                    print(f"--- {' '.join(flags)}: exit {res[1]} {res[2]}\n{res[0]}")
                    sys.exit(1)
    print(f"fuzz: {count} programs agree across {len(MODES)} modes "
          f"({trapped} ended in a runtime error in all of them)")


if __name__ == "__main__":
    main()
