# Build with:  make          (console program + web/API server)
#              make server   (web/API server only: smartshield_server)
#              make test     (all unit tests)
#              make clean
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread

# INTEGRATION: the REST server (server.cpp) uses cpp-httplib, which needs
# Winsock on Windows (MinGW-w64) and Windows 10+ headers.
ifeq ($(OS),Windows_NT)
  SERVER_LIBS = -lws2_32
  SERVER_DEFS = -D_WIN32_WINNT=0x0A00
else
  SERVER_LIBS =
  SERVER_DEFS =
endif

CORE = hashmodule.cpp latlong.cpp time_module.cpp fraudmodule.cpp \
       location_history.cpp transaction_history.cpp transaction_input.cpp \
       card_history.cpp sortmodule.cpp dequemodule.cpp locationgraph.cpp \
       transaction_service.cpp atomic_file.cpp

all: fraudsystem smartshield_server

fraudsystem: main.cpp $(CORE)
	$(CXX) $(CXXFLAGS) -o $@ main.cpp $(CORE)

server: smartshield_server

smartshield_server: server.cpp $(CORE) third_party/httplib.h third_party/json.hpp
	$(CXX) $(CXXFLAGS) $(SERVER_DEFS) -o $@ server.cpp $(CORE) $(SERVER_LIBS)

# Build the server and start it (open http://localhost:8080)
run: smartshield_server
	./smartshield_server

test_fraud: test_fraud.cpp fraudmodule.cpp card_history.cpp latlong.cpp time_module.cpp transaction_history.cpp atomic_file.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

test_modules: test_modules.cpp sortmodule.cpp dequemodule.cpp locationgraph.cpp latlong.cpp time_module.cpp fraudmodule.cpp card_history.cpp transaction_history.cpp atomic_file.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

test_pending: test_pending.cpp $(CORE)
	$(CXX) $(CXXFLAGS) -pthread -o $@ test_pending.cpp $(CORE)

test_atomic: test_atomic_file.cpp $(CORE)
	$(CXX) $(CXXFLAGS) -o $@ test_atomic_file.cpp $(CORE)

test: test_fraud test_modules test_pending test_atomic
	./test_fraud
	./test_modules
	./test_pending
	./test_atomic

clean:
	rm -f fraudsystem smartshield_server *.exe test_fraud test_modules test_pending test_atomic *.o test_review_queue.tmp *.csv.tmp *.csv.*.tmp
	rm -rf *.csv.lock
	rm -f *.csv.append.lock

.PHONY: all server run test clean
