#include "utils.h"

int main(int argc, char **argv) {
    uargs args;

    if (!parse_args(argc, argv, args)) {
        print_help(argv[0]);
        return 1;
    }
}