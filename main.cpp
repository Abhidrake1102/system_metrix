#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
    volatile std::sig_atomic_t running = 1;
    void stop(int) { running = 0; }
    struct Options {
        int intervalMs = 1000;
        int samples = 0;
        std::string path = "/";
        std::string csv;
        bool help = false;
    };

    struct CpuTimes { std::array<std::uint64_t, 8> ticks{}; };
    struct Memory { double usedMiB, totalMiB, percent; };
    struct Snapshot {
        std::string utc;
        std::optional<double> cpu;
        Memory memory{};
        double diskPercent = 0, availableGiB = 0, load1 = 0;
    };

    CpuTimes readCpu(std::istream& input) {
        std::string label;
        CpuTimes cpu;
        if (!(input >> label) || label != "cpu")
            throw std::runtime_error("Cannot read aggregate CPU counters from /proc/stat");
        // First eight fields only: guest counters are already included in user/nice.
        for (auto& ticks : cpu.ticks)
        if (!(input >> ticks)) throw std::runtime_error("Incomplete CPU counters");
        return cpu;
    }
    std::optional<double> cpuUsage(const CpuTimes& before, const CpuTimes& after) {
        std::uint64_t total = 0, idle = 0;
        for (std::size_t i = 0; i < before.ticks.size(); ++i) {
            // Counters, especially iowait, can decrease: rebaseline instead of wrapping.
            if (after.ticks[i] < before.ticks[i]) return std::nullopt;
            const auto delta = after.ticks[i] - before.ticks[i];
            total += delta;
            if (i == 3 || i == 4) idle += delta; // idle + iowait
        }
        if (total == 0) return std::nullopt;
        return 100.0 * (1.0 - static_cast<double>(idle) / static_cast<double>(total));
    }
    Memory readMemory(std::istream& input) {
        std::uint64_t total = 0, available = 0;
        bool foundAvailable = false;
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream row(line);
            std::string key, unit;
            std::uint64_t value = 0;
            if (!(row >> key >> value)) continue;
            if (key != "MemTotal:" && key != "MemAvailable:") continue;
            if (!(row >> unit) || unit != "kB")
                throw std::runtime_error("Unexpected memory unit in /proc/meminfo");
            if (key == "MemTotal:") total = value;
            else { available = value; foundAvailable = true; }
        }
        if (total == 0 || !foundAvailable || available > total)
            throw std::runtime_error("Missing or invalid MemTotal/MemAvailable");
        const double used = static_cast<double>(total - available);
        return {used / 1024.0, static_cast<double>(total) / 1024.0,
                100.0 * used / static_cast<double>(total)};
    }
    std::string utcNow() {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        const auto* utc = std::gmtime(&now); // Only the main thread formats timestamps.
        if (!utc) throw std::runtime_error("Cannot format timestamp");
        std::ostringstream out;
        out << std::put_time(utc, "%Y-%m-%dT%H:%M:%SZ");
        return out.str();
    }
    class SystemMonitor {
        public:
        explicit SystemMonitor(const std::string& path) : path_(path), previous_(cpuNow()) {}
        Snapshot sample() {
            Snapshot result;
            const auto current = cpuNow();
            result.cpu = cpuUsage(previous_, current);
            previous_ = current;
            std::ifstream memory("/proc/meminfo"), load("/proc/loadavg");
            result.memory = readMemory(memory);
            if (!(load >> result.load1)) throw std::runtime_error("Cannot read /proc/loadavg");
            std::error_code error;
            const auto disk = std::filesystem::space(path_, error);
            if (error || disk.capacity == 0 || disk.free > disk.capacity)
                throw std::runtime_error("Cannot read filesystem space for: " + path_);
            result.diskPercent = 100.0 * static_cast<double>(disk.capacity - disk.free)
            / static_cast<double>(disk.capacity);
            result.availableGiB = static_cast<double>(disk.available) / (1024.0 * 1024 * 1024);
            result.utc = utcNow();
            return result;
        }
        private:
        std::string path_;
        CpuTimes previous_;
        static CpuTimes cpuNow() {
            std::ifstream input("/proc/stat");
            return readCpu(input);
        }
    };
    int number(const std::string& text, int low, int high, const std::string& option) {
        int value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()
            || value < low || value > high)
            throw std::runtime_error(option + " must be an integer in ["
        std::to_string(low) + ", " + std::to_string(high) + "]");
        return value;
    }
    Options parseOptions(int argc, char* argv[]) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") { options.help = true; continue; }
            if (arg != "--interval-ms" && arg != "--samples" && arg != "--path" && arg != "--csv")
                throw std::runtime_error("Unknown option: " + arg);
            if (++i >= argc || std::string(argv[i]).empty())
                throw std::runtime_error("Missing value for " + arg);
            const std::string value = argv[i];
            if (arg == "--interval-ms") options.intervalMs = number(value, 100, 60000, arg);
            else if (arg == "--samples") options.samples = number(value, 0, 1000000, arg);
            else if (arg == "--path") options.path = value;
            else options.csv = value;
        }
        return options;
    }
    void printHelp() {
        std::cout << "SysWatch C++: Linux/WSL resource monitor
            "
        << "Usage: ./syswatch [options]
            "
        << " --interval-ms N Sampling delay, 100..60000 ms (default: 1000)
            "
        << " --samples N Number of samples, 0..1000000 (0: until Ctrl+C)
            "
        << " --path PATH Filesystem to monitor (default: /)
            "
        << " --csv FILE Write CSV; replaces existing file contents
            "
        << " -h, --help Show this help
            ";
    }
    std::string cpuText(const std::optional<double>& cpu) {
        if (!cpu) return "n/a";
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << cpu;
        return out.str();
    }
    void printHeader() {
        std::cout << std::left << std::setw(22) << "TIME (UTC)" << std::right
            << std::setw(8) << "CPU%" << std::setw(8) << "RAM%"
            << std::setw(12) << "RAM MiB" << std::setw(8) << "DISK%"
            << std::setw(12) << "AVAIL GiB" << std::setw(10) << "LOAD1" << std::endl;
    }
    void printSample(const Snapshot& s) {
        std::cout << std::left << std::setw(22) << s.utc << std::right
            << std::setw(8) << cpuText(s.cpu) << std::fixed << std::setprecision(1)
            << std::setw(8) << s.memory.percent << std::setw(12) << s.memory.usedMiB
            << std::setw(8) << s.diskPercent << std::setw(12) << s.availableGiB
            << std::setprecision(2) << std::setw(10) << s.load1 << std::endl;
    }
    void writeCsv(std::ofstream& csv, const Snapshot& s) {
        csv << s.utc << ',' << std::fixed << std::setprecision(2);
        if (s.cpu) csv << s.cpu; // A missing CPU value stays empty, never a false zero.
            csv << ',' << s.memory.percent << ',' << s.memory.usedMiB << ',' << s.memory.totalMiB
            << ',' << s.diskPercent << ',' << s.availableGiB << ',' << s.load1 << '
            ';
        csv.flush();
        if (!csv) throw std::runtime_error("Failed to write CSV output");
    }
    void waitInterval(int milliseconds) {
        // Small chunks let Ctrl+C interrupt even a long sampling delay promptly.
        for (int waited = 0; running && waited < milliseconds; waited += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(100, milliseconds - waited)));
    }
} // namespace
int main(int argc, char* argv[]) {
    try {
        const auto options = parseOptions(argc, argv);
        if (options.help) { printHelp(); return 0; }
        std::error_code error;
        if (!std::filesystem::exists(options.path, error) || error)
            throw std::runtime_error("Filesystem path does not exist or is inaccessible: " + options.path);
        SystemMonitor monitor(options.path);
        std::ofstream csv;
        if (!options.csv.empty()) {
            csv.open(options.csv);
            if (!csv) throw std::runtime_error("Cannot open CSV file: " + options.csv);
            csv << "timestamp_utc,cpu_percent,ram_percent,ram_used_mib,ram_total_mib,"
                "disk_used_percent,disk_available_gib,load_1m
            " << std::flush;
            if (!csv) throw std::runtime_error("Cannot write CSV header");
        }
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::cout << "SysWatch C++ | filesystem: " << options.path
            << " | interval: " << options.intervalMs << " ms | Ctrl+C to stop
            ";
        printHeader();
        for (std::uint64_t n = 0; running && (options.samples == 0
            || n < static_cast<std::uint64_t>(options.samples)); ++n) {
            waitInterval(options.intervalMs); // CPU needs two readings separated by time.
            if (!running) break;
            const auto snapshot = monitor.sample();
            printSample(snapshot);
            if (csv.is_open()) writeCsv(csv, snapshot);
        }
        if (!running) std::cout << "Stopped.
            ";
        if (csv.is_open()) {
            csv.close();
            if (!csv) throw std::runtime_error("Failed to close CSV output");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "
            Use --help for usage.
        ";
        return 1;
    }
}
