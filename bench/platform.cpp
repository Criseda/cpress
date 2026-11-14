#include "platform.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fcntl.h>
#include <io.h>
// psapi.h must come after windows.h.
#include <psapi.h>
#else
#ifdef __linux__
#include <sched.h>
#endif
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace platform {

namespace {

std::string trim_right(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '\t')) {
        text.pop_back();
    }
    return text;
}

}  // namespace

std::string capture(const std::string& command) {
#ifdef _WIN32
    std::FILE* pipe = _popen(command.c_str(), "r");
#else
    std::FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) return {};

    std::string output;
    std::array<char, 256> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return trim_right(std::move(output));
}

#ifdef _WIN32

const char* const kNullDevice = "nul";

std::uint64_t peak_memory_bytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters) == 0) return 0;
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
}

bool peak_memory_omits_large_pages() {
    return true;
}

void make_stdout_binary() {
    _setmode(_fileno(stdout), _O_BINARY);
}

std::uint64_t huge_page_bytes_faulted() {
    return 0;
}

bool pin_to_cpu(int cpu) {
    // One bit per logical processor of the process's group.
    if (cpu < 0 || cpu >= std::numeric_limits<DWORD_PTR>::digits) return false;
    const DWORD_PTR mask = DWORD_PTR{1} << cpu;
    return SetProcessAffinityMask(GetCurrentProcess(), mask) != 0;
}

std::filesystem::path self_path(const char* argv0) {
    std::string buffer(32768, '\0');
    const DWORD length =
        GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return argv0;
    buffer.resize(length);
    return buffer;
}

namespace {

// Quotes one argument the way the Microsoft C runtime parses a command line.
std::string quote_argument(std::string_view argument) {
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append((backslashes * 2) + 1, '\\');
            out += '"';
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

}  // namespace

int run_process(const std::filesystem::path& exe, const std::vector<std::string>& args) {
    std::string command = quote_argument(exe.string());
    for (const std::string& arg : args) {
        command += ' ';
        command += quote_argument(arg);
    }

    STARTUPINFOA startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    // CreateProcess may modify the command line, so it needs a mutable buffer.
    if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                       &startup, &process) == 0) {
        return -1;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}

#else  // POSIX

const char* const kNullDevice = "/dev/null";

bool peak_memory_omits_large_pages() {
    return false;
}

// Nothing to do: only Windows has text streams that change bytes.
void make_stdout_binary() {}

std::uint64_t peak_memory_bytes() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
    const auto max_rss = static_cast<std::uint64_t>(usage.ru_maxrss);
#ifdef __APPLE__
    return max_rss;  // bytes
#else
    return max_rss * 1024;  // kibibytes
#endif
}

#ifdef __linux__
namespace {

// A transparent huge page on x86-64 and on ARM64 with 4 KiB base pages.
constexpr std::uint64_t kHugePageBytes = std::uint64_t{2} << 20;

// The value of `name` in /proc/vmstat, or nothing if it is not there.
std::optional<std::uint64_t> vmstat(std::string_view name) {
    std::ifstream file("/proc/vmstat");
    std::string key;
    std::uint64_t value = 0;
    while (file >> key >> value) {
        if (key == name) return value;
    }
    return {};
}

}  // namespace

std::uint64_t huge_page_bytes_faulted() {
    return vmstat("thp_fault_alloc").value_or(0) * kHugePageBytes;
}

// The process has one thread when this is called, so pinning the calling
// thread pins the process.
bool pin_to_cpu(int cpu) {
    if (cpu < 0 || cpu >= CPU_SETSIZE) return false;
    cpu_set_t set{};
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    return sched_setaffinity(0, sizeof set, &set) == 0;
}
#else
std::uint64_t huge_page_bytes_faulted() {
    return 0;
}

bool pin_to_cpu(int /*cpu*/) {
    return false;
}
#endif

std::filesystem::path self_path(const char* argv0) {
    std::error_code ec;
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        buffer.resize(std::char_traits<char>::length(buffer.c_str()));
        const auto resolved = std::filesystem::canonical(buffer, ec);
        if (!ec) return resolved;
    }
#else
    const auto resolved = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return resolved;
#endif
    return argv0;
}

int run_process(const std::filesystem::path& exe, const std::vector<std::string>& args) {
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(exe.string());
    storage.insert(storage.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& item : storage) argv.push_back(item.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    if (posix_spawn(&pid, storage[0].c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif

}  // namespace platform
