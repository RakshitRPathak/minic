// steps for 27 to reach 1 in the Collatz sequence is 111
let n = 27;
let steps = 0;
while (n != 1) {
  if ((n / 2) * 2 == n) { n = n / 2; }
  else { n = 3 * n + 1; }
  steps = steps + 1;
}
print steps;
