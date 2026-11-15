// A function can be called before its definition, so two functions can call each other.
print is_even(10);   // 1
print is_odd(10);    // 0
print is_even(7);    // 0

fn is_even(n) { if (n == 0) { return 1; } return is_odd(n - 1); }
fn is_odd(n)  { if (n == 0) { return 0; } return is_even(n - 1); }

print is_odd(7);     // 1
