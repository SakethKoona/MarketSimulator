all: main

main: main.o orderbook.o skiplist.o
<TAB>g++ main.o orderbook.o skiplist.o -o main

main.o: main.cpp
<TAB>g++ -c main.cpp

orderbook.o: orderbook.cpp
<TAB>g++ -c orderbook.cpp

skiplist.o: skiplist.cpp
<TAB>g++ -c skiplist.cpp

