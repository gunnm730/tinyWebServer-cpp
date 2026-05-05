CXX      ?= g++
CXXFLAGS += -std=c++17 -Isrc
LDFLAGS   = -lpthread -lmysqlcppconn

DEBUG    ?= 1
ifeq ($(DEBUG),1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2
endif

SRCS = src/main.cpp src/config/config.cpp src/server/webserver.cpp src/http/http_connection.cpp
OBJS = $(SRCS:.cpp=.o)

server: $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f server $(OBJS)

.PHONY: clean
