#include <assert.h>
#include <stdlib.h>

int main()
{
  int x = 0;
  int i;
  assert(x == 0);
  if (i)
  {
    assert(x == 0);
    int __cs_c;
    if (__cs_c)
    {
      ++x;
      assert(x != 0);
    }
    else
    {
      --x;
      assert(x != 0);
    }
    assert(x != 0);
  }
  else
  {
    assert(x == 0);
    int __cs_c;
    if (__cs_c)
    {
      ++x;
      assert(x != 0);
    }
    else
    {
      --x;
      assert(x != 0);
    }
    assert(x != 0);
  }
  assert(x != 0);
  return 0;
}
