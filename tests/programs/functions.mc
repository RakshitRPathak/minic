// Functions: parameters, return values, calls inside expressions.
fn add(a, b) { return a + b; }
fn square(x) { return x * x; }
fn max(a, b) { if (a > b) { return a; } return b; }

print add(2, 3);                    // 5
print square(add(1, 2));            // 9
print max(3, 7) + max(10, 4);       // 17
print add(square(2), square(3));    // 13
print -square(4);                   // -16

// Arguments are evaluated left to right.
fn show(x) { print x; return x; }
print add(show(1), show(2));        // 1 2 3
