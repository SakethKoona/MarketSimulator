#include <iostream>
#include <thread>

void worker() {
    // Sample worker thread that just prints someting out
    std::cout << "Printing from Worker thread" << std::endl;
}

void worker2() {
    std::cout << "Printing from the second worker thread" << std::endl;
}

int main() {
    std::cout << "Event Emission Testing" << std::endl;
    std::cout << "Part 1" << std::endl;

    std::thread t(worker);
    std::thread t2(worker2);
    t.join();
    t2.join();

    std::cout << "printing from main" << std::endl;
}
