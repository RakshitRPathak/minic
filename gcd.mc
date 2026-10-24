// Euclid's algorithm by repeated subtraction
let a = 48;
let b = 18;
while (a != b) {
  if (a > b) { a = a - b; } else { b = b - a; }
}
print a;
