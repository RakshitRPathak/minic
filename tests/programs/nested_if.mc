let x = 15;
if (x > 10) {
  if (x > 20) { print 3; }
  else { print 2; }
} else {
  print 1;
}
// dangling else binds to the nearest if
if (x > 100) if (x > 200) print 7; else print 8;
print 9;
// chained else-if
let grade = 85;
if (grade >= 90) print 4;
else if (grade >= 80) print 3;
else if (grade >= 70) print 2;
else print 1;
