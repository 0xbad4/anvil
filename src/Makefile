CXX      := g++
CXXFLAGS := -std=c++23 -Wextra -Wall -I../include


.PHONY: init clean lexer parser validator

BIN_DIR := ./bin

init:
	@mkdir -p $(BIN_DIR)

lexer: init
	$(CXX) $(CXXFLAGS) ./tests/lexer.cpp -lfmt -o $(BIN_DIR)/lexer

parser: init
	$(CXX) $(CXXFLAGS) ./tests/parser.cpp -lfmt -o $(BIN_DIR)/parser

validator: init
	$(CXX) $(CXXFLAGS) ./tests/validator.cpp -lfmt -o $(BIN_DIR)/validator

all: lexer parser validator

clean:
	rm -rf $(BIN_DIR)
