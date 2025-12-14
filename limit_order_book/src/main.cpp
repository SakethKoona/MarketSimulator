#include "../include/skiplist.h"
#include <iostream>
#include <ostream>

int main (int argc, char *argv[]) {
  SkipList<double, double> testList = SkipList<double, double>(0.5);


  testList.insert(50.0, 20.0);
  testList.insert(53.0, 20.0);
  testList.insert(51.0, 23.0);

  testList.printList();

  std::cout << testList.search(53.0) << std::endl;
  std::cout << testList.search(100.0) << std::endl;
  std::cout << testList.delete_node(51.0) << std::endl;
  std::cout << testList.delete_node(100.0) << std::endl;
  std::cout << testList.search(51.0) << std::endl;
  return 0;
}
