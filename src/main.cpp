#include "cli/CommandLineInterface.h"

#include <iostream>

int main(int argc, char* argv[]) {
    // We never mix C stdio and C++ streams, so the synchronisation (which makes
    // every std::cout write go through C's stdout buffer) is pure overhead.
    std::ios::sync_with_stdio(false);

    loganalyzer::CommandLineInterface cli(std::cout, std::cerr);
    const int exitCode = cli.run(argc, argv);
    std::cout.flush();
    return exitCode;
}
