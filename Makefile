test_objs=tests.o
test_libs=-pthread

CXXFLAGS := $(CXXFLAGS) -g -O0 -Wall -Wextra -Wpedantic -Werror -std=c++11
CPPFLAGS := $(CPPFLAGS) -I"$(PWD)/include" -I"$(PWD)/vendor/concurrentqueue"

.PHONY: run-test clean

run-test: test
	@echo -e "\n\n----\n\n" \
	     "Running tests with various settings. This will take some time." \
	     "\nUsage is:" \
	     "number of producer threads | number of consumer threads" \
			 "number of packages to produce in this run | capacity of the channel"
	@echo -e "\n\n----\n\n"
	ionice -c 3 nice -n 19 ./test  1  1 80000000 2000000
	@echo -e "\n\n----\n\n"
	ionice -c 3 nice -n 19 ./test  4  4 80000000 2000000
	@echo -e "\n\n----\n\n"
	ionice -c 3 nice -n 19 ./test 16 16 80000000 2000000
	@echo -e "\n\n----\n\n"
	ionice -c 3 nice -n 19 ./test 90 90 80000000 2000000
	@echo -e "\n\n----\n\n"
	ionice -c 3 nice -n 19 ./test 90 90 80000000 2000
	@echo -e "\n\n----\n\n"
	ionice -c 3 nice -n 19 ./test 90 90 80000000 200
	@echo -e "\n\n----\n\n"
	ionice -c 3 nice -n 19 ./test 90 90 80000000 20
	@echo -e "\n\n----\n\n"
	ionice -c 3 nice -n 19 ./test 90 90 80000000 2

test: $(test_objs) include/softwear/concurrent_channel.hpp
	$(CXX) $(LDFLAGS) $(CXXFLAGS) $(test_objs) $(test_libs) -o test

clean:
	rm -f ./test $(test_objs)
