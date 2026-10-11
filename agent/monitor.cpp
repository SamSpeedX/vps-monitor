#include <iostream>
#include <fstream>
#include <string>
#include <sys/statvfs.h>

int main() {
    struct statvfs disk {};
    if (statvfs("/", &disk) != 0) {
        std::cerr << "Cannot read root filesystem\n";
        return 1;
    }

    unsigned long long total =
        static_cast<unsigned long long>(disk.f_blocks) * disk.f_frsize;
    unsigned long long available =
        static_cast<unsigned long long>(disk.f_bavail) * disk.f_frsize;
    unsigned long long used = total - 
        static_cast<unsigned long long>(disk.f_bfree) * disk.f_frsize;

    unsigned long long memTotal = 0, memAvailable = 0;
    std::ifstream mem("/proc/meminfo");
    std::string key, unit;
    unsigned long long value;

    while (mem >> key >> value >> unit) {
        if (key == "MemTotal:") memTotal = value * 1024;
        if (key == "MemAvailable:") memAvailable = value * 1024;
    }

    std::cout << "{"
        << "\"disk\":{\"total_bytes\":" << total
        << ",\"used_bytes\":" << used
        << ",\"available_bytes\":" << available
        << ",\"used_percent\":"
        << (total ? (100.0 * used / total) : 0.0) << "},"
        << "\"memory\":{\"total_bytes\":" << memTotal
        << ",\"available_bytes\":" << memAvailable
        << ",\"used_bytes\":" << (memTotal >= memAvailable ? memTotal - memAvailable : 0)
        << ",\"used_percent\":"
        << (memTotal ? 100.0 * (memTotal - memAvailable) / memTotal : 0.0)
        << "}}";
}
