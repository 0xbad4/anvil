#pragma once

#include <cstdio>

struct uargs
{
    
};

bool parse_args(int argc, char **argv, uargs &out) {

}

void print_help(const char* pname) {
    printf("Usage: %s [options] <file>\n", pname[0]);
}

// pretty print
void pprint() {

}

void print_src_err() {
    // 6 | ld M[19]
    //         ^^^^ out of range ...
}

void print_src_warn() {

}
