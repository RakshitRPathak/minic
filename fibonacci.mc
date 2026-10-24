let a = 0;
let b = 1;
let count = 0;
while (count < 10) {
  print a;
  let t = a + b;
  a = b;
  b = t;
  count = count + 1;
}
