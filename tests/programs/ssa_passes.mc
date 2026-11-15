// Code the SSA optimizer should shrink (see the "ir optimizer" checks in
// run_tests.sh). The output must be the same in every mode.
let a = 2;
let b = a * 3;          // constants flow through variables: b is 6
let c = b + a;          // c is 8
print c;                                   // 8
if (c > 5) { print 1; } else { print 2; }  // 1: the branch is decided at compile time

fn same(x, y) {
    let p = x * y + 1;
    let q = y * x + 1;  // the same value as p, because * commutes
    return p - q;       // so this is 0 and nothing is left to compute
}
fn twice(x) {
    print x * x + 1;
    print x * x + 1;    // reuses the first x * x + 1
    return x * 1 + 0;   // identities: this is just x
}
print same(3, 4);       // 0
print twice(5);         // 26 26 5
