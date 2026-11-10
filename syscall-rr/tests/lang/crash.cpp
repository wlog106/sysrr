// C++: an exception nobody catches -> std::terminate -> abort.
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <iostream>
#include <chrono>
#include <thread>
int main() {
	std::ifstream rnd("/dev/urandom", std::ios::binary);
	std::map<int, std::string> m;
	auto t0 = std::chrono::system_clock::now();
	unsigned long sum = 0;
	for (int i = 0;; i++) {
		unsigned long r = 0;
		rnd.read(reinterpret_cast<char *>(&r), sizeof(r));
		sum ^= r;
		m[i % 16] = std::to_string(sum);
		std::cout << "iter " << i << " " << sum << std::endl;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if (std::chrono::system_clock::now() - t0 > std::chrono::milliseconds(3200))
			throw std::runtime_error("key " + m.at(99));
	}
}
