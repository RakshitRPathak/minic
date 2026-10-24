// primes up to 30 by trial division
let n = 2;
while (n <= 30) {
  let d = 2;
  let isPrime = 1;
  while (d * d <= n) {
    // n is divisible by d when (n / d) * d == n (integer division truncates)
    if ((n / d) * d == n) { isPrime = 0; }
    d = d + 1;
  }
  if (isPrime) { print n; }
  n = n + 1;
}
