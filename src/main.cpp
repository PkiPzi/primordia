#include "app/Options.hpp"

int main(int argc, char** argv) {
    pm::Options opt = pm::parseOptions(argc, argv);
    if (opt.help) {
        pm::printUsage();
        return 0;
    }
    if (opt.headless) return pm::runHeadless(opt);
    return pm::runApp(opt);
}
