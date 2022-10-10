CXXFLAGS += -g -ggdb

quantiflop:	quantiflop.o
	$(CXX) $(CXXFLAGS) -o $@ $^

