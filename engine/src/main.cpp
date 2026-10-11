#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "commands.h"

namespace {

void usage() {
    std::cout <<
        "dedupc - deduplication and compression engine (phase 1: whole-file dedup)\n"
        "\n"
        "usage:\n"
        "  dedupc init    <repo>\n"
        "  dedupc ingest  <repo> <path>... [--level N]\n"
        "  dedupc restore <repo> --file <id|path|name> --out <dir>\n"
        "  dedupc list    <repo>\n"
        "  dedupc stats   <repo>\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return argc < 2 ? 0 : 2;
    }
    const std::string cmd = argv[1];
    const std::filesystem::path repo = argv[2];

    try {
        if (cmd == "init") return cmd_init(repo);

        if (cmd == "ingest") {
            std::vector<std::filesystem::path> inputs;
            int level = 3;
            for (int i = 3; i < argc; ++i) {
                if (std::strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
                    level = std::stoi(argv[++i]);
                    if (level < 1 || level > 19)
                        throw std::runtime_error("--level must be 1..19");
                } else {
                    inputs.emplace_back(argv[i]);
                }
            }
            if (inputs.empty()) throw std::runtime_error("ingest: no input paths given");
            return cmd_ingest(repo, inputs, level);
        }

        if (cmd == "restore") {
            std::string spec;
            std::filesystem::path outDir;
            for (int i = 3; i < argc; ++i) {
                if (std::strcmp(argv[i], "--file") == 0 && i + 1 < argc)
                    spec = argv[++i];
                else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
                    outDir = argv[++i];
                else
                    throw std::runtime_error(std::string("unknown argument: ") + argv[i]);
            }
            if (spec.empty() || outDir.empty())
                throw std::runtime_error("restore: --file and --out are required");
            return cmd_restore(repo, spec, outDir);
        }

        if (cmd == "list") return cmd_list(repo);
        if (cmd == "stats") return cmd_stats(repo);

        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 2;
    }
}
