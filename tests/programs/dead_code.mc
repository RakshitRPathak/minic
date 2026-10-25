// constant conditions: the optimizer deletes the dead side
if (0) { print 111; } else { print 222; }
if (1) { print 333; } else { print 444; }
if (2 - 2) { print 555; }
while (0) { print 666; }
if (3 > 2) { print 777; }
// a variable declared in deleted code still exists and reads as 0
if (0) { let ghost = 5; }
print ghost;
print 888;
