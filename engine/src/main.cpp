#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#include "commands.h"
#include "utf8.h"

using namespace std;

namespace {

void usage() {
    cout <<
        "dedupc - deduplication and compression engine (phase 1: whole-file dedup)\n"
        "\n"
        "usage:\n"
        "  dedupc init    <repo>\n"
        "  dedupc ingest  <repo> <path>... [--level N]\n"
        "  dedupc restore <repo> --file <id|path|name> --out <dir>\n"
        "  dedupc list    <repo>\n"
        "  dedupc stats   <repo>\n";
}

// Command-line arguments as UTF-8. On Windows, argv is encoded in the legacy
// ANSI code page, which turns non-ASCII file names into "?", so the wide
// command line is re-parsed instead.
vector<string> utf8_args(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int n = 0;
    wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) throw runtime_error("cannot read command line");
    vector<string> out;
    for (int i = 0; i < n; ++i) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        string s(len > 0 ? len - 1 : 0, '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        out.push_back(move(s));
    }
    LocalFree(w);
    return out;
#else
    return vector<string>(argv, argv + argc);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const vector<string> args = utf8_args(argc, argv);
        if (args.size() < 3) {
            usage();
            return args.size() < 2 ? 0 : 2;
        }
        const string& cmd = args[1];
        const filesystem::path repo = path_from_utf8(args[2]);

        if (cmd == "init") return cmd_init(repo);

        if (cmd == "ingest") {
            vector<filesystem::path> inputs;
            int level = 3;
            for (size_t i = 3; i < args.size(); ++i) {
                if (args[i] == "--level" && i + 1 < args.size()) {
                    level = stoi(args[++i]);
                    if (level < 1 || level > 19)
                        throw runtime_error("--level must be 1..19");
                } else {
                    inputs.push_back(path_from_utf8(args[i]));
                }
            }
            if (inputs.empty()) throw runtime_error("ingest: no input paths given");
            return cmd_ingest(repo, inputs, level);
        }

        if (cmd == "restore") {
            string spec;
            filesystem::path outDir;
            for (size_t i = 3; i < args.size(); ++i) {
                if (args[i] == "--file" && i + 1 < args.size())
                    spec = args[++i];
                else if (args[i] == "--out" && i + 1 < args.size())
                    outDir = path_from_utf8(args[++i]);
                else
                    throw runtime_error("unknown argument: " + args[i]);
            }
            if (spec.empty() || outDir.empty())
                throw runtime_error("restore: --file and --out are required");
            return cmd_restore(repo, spec, outDir);
        }

        if (cmd == "list") return cmd_list(repo);
        if (cmd == "stats") return cmd_stats(repo);

        usage();
        return 2;
    } catch (const exception& e) {
        cerr << "fatal: " << e.what() << "\n";
        return 2;
    }
}
