CXX      := g++
CXXFLAGS := -std=c++23 -Wextra -Wall -I../include


.PHONY: init clean lexer

BIN_DIR := ./bin

init:
	@mkdir -p $(BIN_DIR)

lexer: init
	$(CXX) $(CXXFLAGS) ./tests/lexer.cpp -lfmt -o $(BIN_DIR)/lexer

parser: init
	$(CXX) $(CXXFLAGS) ./tests/parser.cpp -lfmt -o $(BIN_DIR)/parser

clean:
	rm -rf $(BIN_DIR)
