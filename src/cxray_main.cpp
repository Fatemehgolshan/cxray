// cxray_main.cpp — CXRay standalone simulator entry point
//
// Usage:
//   cxray --config <config.cfg> --trace <trace.txt> [options]
//
// Options:
//   --config  <file>    libconfig++ or key=value .cfg file (required)
//   --trace   <file>    input trace file (required)
//   --output  <dir>     output directory (overrides config)
//   --tag     <str>     run tag prefix for output files
//   --print-config      dump fully resolved config then exit
//   --warmup  <N>       number of warmup requests (overrides config)
//   --max     <N>       max requests to simulate (overrides config)
//   --version           show version and exit
//   --help              show this help

#include "cxl_mem.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

static constexpr const char* VERSION = "CXRay 1.0  (CXL 3.1 standalone simulator)";

static void printHelp(const char* argv0) {
    std::cout << VERSION << "\n\n"
              << "Usage: " << argv0 << " --config <cfg> --trace <trace> [options]\n\n"
              << "Options:\n"
              << "  --config  <file>   Key=value or libconfig++ .cfg file (required)\n"
              << "  --trace   <file>   Input trace file (required)\n"
              << "  --output  <dir>    Output directory (default: results/)\n"
              << "  --tag     <str>    Run tag prefix for output files\n"
              << "  --warmup  <N>      Warmup request count\n"
              << "  --max     <N>      Max requests to simulate (0=all)\n"
              << "  --print-config     Print fully resolved config then exit\n"
              << "  --version          Show version and exit\n"
              << "  --help             Show this help\n\n"
              << "Trace file format (one request per line):\n"
              << "  <timestamp_ns>  <R|W>  <addr_hex>  <size_bytes>  <requester_id>  [poison]\n\n"
              << "Example:\n"
              << "  " << argv0 << " --config configs/cxl_3.1.cfg --trace traces/seq.txt --tag baseline\n";
}

int main(int argc, char* argv[]) {
    std::string configFile;
    std::string traceFile;
    std::string outputDir;
    std::string runTag;
    uint64_t    warmup   = UINT64_MAX;   // UINT64_MAX = not overridden
    uint64_t    maxReqs  = UINT64_MAX;
    bool        printCfg = false;

    for (int i = 1; i < argc; i++) {
        auto arg = std::string(argv[i]);
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires an argument\n";
                std::exit(1);
            }
            return std::string(argv[++i]);
        };

        if      (arg == "--config")       configFile = next();
        else if (arg == "--trace")        traceFile  = next();
        else if (arg == "--output")       outputDir  = next();
        else if (arg == "--tag")          runTag     = next();
        else if (arg == "--warmup")       warmup     = std::stoull(next());
        else if (arg == "--max")          maxReqs    = std::stoull(next());
        else if (arg == "--print-config") printCfg   = true;
        else if (arg == "--version") { std::cout << VERSION << "\n"; return 0; }
        else if (arg == "--help" || arg == "-h") { printHelp(argv[0]); return 0; }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printHelp(argv[0]);
            return 1;
        }
    }

    if (configFile.empty()) {
        std::cerr << "Error: --config is required\n";
        printHelp(argv[0]);
        return 1;
    }
    if (traceFile.empty() && !printCfg) {
        std::cerr << "Error: --trace is required\n";
        printHelp(argv[0]);
        return 1;
    }

    try {
        auto sim = cxl::CXLMemController::fromConfigFile(configFile);

        // Apply CLI overrides
        if (!outputDir.empty()) const_cast<cxl::SimConfig&>(sim.config()).outputDir = outputDir;
        if (!runTag.empty())    const_cast<cxl::SimConfig&>(sim.config()).runTag    = runTag;
        if (warmup  != UINT64_MAX) const_cast<cxl::SimConfig&>(sim.config()).warmupRequests = warmup;
        if (maxReqs != UINT64_MAX) const_cast<cxl::SimConfig&>(sim.config()).maxRequests    = maxReqs;

        sim.printResolvedConfig();

        if (printCfg) return 0;

        // Run trace
        std::cout << "Running trace: " << traceFile << "\n";
        sim.runTrace(traceFile);
        sim.printStats();
        sim.writeStatsJson();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 2;
    }

    return 0;
}
