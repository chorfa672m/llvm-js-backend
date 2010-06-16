; RUN: llc -march=x86 < %s

%vec = type <9 x float>
define %vec @vecdiv( %vec %p1, %vec %p2)
{
  %result = fdiv %vec %p1, %p2
  ret %vec %result
}
