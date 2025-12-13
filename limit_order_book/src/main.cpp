#include "../include/skiplist.h"
#include <iostream>

int main (int argc, char *argv[]) {
  SkipList<double, double> testList = SkipList<double, double>(0.5);


  testList.insert(50.0, 20.0);
  testList.insert(51.0, 23.0);

  // std::cout << testList.search(51.0);
  std::cout << testList.search(100.0);

  // std::cout << testList.len();
  return 0;
}
