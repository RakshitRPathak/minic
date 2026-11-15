// Recursion: each call gets its own frame, so n survives the inner call.
fn fact(n) {
    if (n <= 1) { return 1; }
    return n * fact(n - 1);
}
fn fib(n) {
    if (n < 2) { return n; }
    return fib(n - 1) + fib(n - 2);
}
fn gcd(a, b) {
    if (b == 0) { return a; }
    return gcd(b, a - (a / b) * b);
}
fn ackermann(m, n) {
    if (m == 0) { return n + 1; }
    if (n == 0) { return ackermann(m - 1, 1); }
    return ackermann(m - 1, ackermann(m, n - 1));
}
print fact(10);          // 3628800
print fact(20);          // 2432902008176640000
print fib(20);           // 6765
print gcd(1071, 462);    // 21
print ackermann(2, 3);   // 9
