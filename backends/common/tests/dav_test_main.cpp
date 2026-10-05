// The main of the CalDAV and WebDAV tests: starts fake_dav.py on a free port,
// runs every test, stops the server.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>

#include "dav_test.hpp"
#include "test.hpp"

std::string davtest::server_url;

using davtest::server_url;

int main(int argc, char** argv) {
	// TESTS PATH/fake_dav.py
	if (argc < 2) {
		std::cerr << "usage: " << argv[0] << " fake_dav.py\n";
		return 2;
	}
	auto cmd = std::format("python3 '{}'", argv[1]);
	auto* server = popen(cmd.c_str(), "r");
	char port[32] = {};
	if (!server || !std::fgets(port, sizeof port, server)) {
		std::cerr << "couldn't start the fake DAV server\n";
		return 2;
	}
	server_url = std::format("http://127.0.0.1:{}",
							 std::string(port, std::strcspn(port, "\n")));
	for (auto& c : test::registry()) {
		int before = test::failures();
		try {
			c.fn();
		} catch (const std::exception& e) {
			++test::failures();
			std::cerr << c.name << ": threw " << e.what() << "\n";
		}
		if (test::failures() != before) {
			std::cerr << "FAILED: " << c.name << "\n";
		}
	}
	std::cout << test::registry().size() << " tests, " << test::failures()
			  << " failed checks\n";

	// Stop the server.
	if (std::system(std::format("curl -s '{}/_quit' > /dev/null", server_url)
						.c_str()) != 0) {
		std::cerr << "couldn't stop the fake server; it stops by itself after "
					 "120 s\n";
	}
	pclose(server);
	return test::failures() == 0 ? 0 : 1;
}
