// Return forms: early return from a loop, bare `return;`, and falling off the end.
fn first_divisor(n) {
    let d = 2;
    while (d * d <= n) {
        if (n - (n / d) * d == 0) { return d; }
        d = d + 1;
    }
    return n;
}
fn nothing() { }
fn bare(x) { if (x > 0) { return; } print 111; }
fn after_return() { return 7; print 999; }      // the print is unreachable
fn no_locals_read_zero() { if (0) { let never = 1; } return never; }
fn side_effect_kept(x) { print x; return 0; }

print first_divisor(91);    // 7
print first_divisor(97);    // 97
print nothing();            // 0
print bare(1);              // 0
print bare(0);              // 111 then 0
print after_return();       // 7
print no_locals_read_zero();// 0
print 0 * side_effect_kept(42);   // 42 then 0: a call is never folded away
