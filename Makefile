# 编译器
CXX = g++
# 编译选项	
CXXFLAGS = -std=c++14 -pthread -I include 

server: main.o WebServer.o Logger.o SqlConnPool.o
	$(CXX) $(CXXFLAGS) -o server main.o WebServer.o Logger.o SqlConnPool.o -lmysqlclient
	
%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f server *.o server.log