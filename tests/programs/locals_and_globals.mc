// Scoping: a function sees globals declared above it, and its own variables hide them.
let counter = 0;
let x = 100;

fn bump() { counter = counter + 1; return counter; }   // writes the global
fn shadow(x) { x = x + 1; return x; }                   // parameter hides global x
fn local_let() { let x = 5; x = x * 2; return x; }      // local hides global x
fn sum_to(n) {                                          // locals in a loop
    let total = 0;
    let i = 1;
    while (i <= n) { total = total + i; i = i + 1; }
    return total;
}

bump(); bump();           // call statements: the result is discarded
print bump();             // 3
print counter;            // 3
print shadow(1);          // 2
print local_let();        // 10
print x;                  // 100: neither function touched the global
print sum_to(100);        // 5050
print sum_to(10);         // 55: `total` starts again at 0 in a new frame

// Each activation has its own locals, even when calls are nested.
fn depth(n) { let mine = n * 10; if (n > 0) { depth(n - 1); } return mine; }
print depth(3);           // 30
