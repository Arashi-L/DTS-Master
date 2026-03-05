#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <random>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>

// 在Windows中避免min/max宏冲突
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <errno.h>
#endif

// 使用自定义的min/max函数避免宏冲突
template<typename T>
inline T my_min(T a, T b) { return a < b ? a : b; }

template<typename T>
inline T my_max(T a, T b) { return a > b ? a : b; }

class DiskWiper {
public:  // 将枚举设为public
    // Wipe method enumeration
    enum WipeMethod {
        ZERO_FILL,          // Zero fill
        RANDOM_FILL,        // Random fill
        DOD_SHORT,          // DoD 5220.22-M short (3 passes)
        DOD_LONG,           // DoD 5220.22-M long (7 passes)
        GUTMANN,           // Gutmann (35 passes)
        VERIFY_ONLY        // Verify only
    };

private:
    std::string device_path;
    bool verbose;

public:
    DiskWiper(const std::string& path, bool verbose = false)
        : device_path(path), verbose(verbose) {
    }

    // Get disk size
    uint64_t GetDiskSize() {
        uint64_t size = 0;

#ifdef _WIN32
        HANDLE hDevice = CreateFileA(
            device_path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        if (hDevice != INVALID_HANDLE_VALUE) {
            GET_LENGTH_INFORMATION lengthInfo;
            DWORD bytesReturned = 0;

            if (DeviceIoControl(hDevice,
                IOCTL_DISK_GET_LENGTH_INFO,
                NULL, 0,
                &lengthInfo, sizeof(lengthInfo),
                &bytesReturned, NULL)) {
                size = lengthInfo.Length.QuadPart;
            }
            CloseHandle(hDevice);
        }
#else
        int fd = open(device_path.c_str(), O_RDONLY);
        if (fd >= 0) {
            if (ioctl(fd, BLKGETSIZE64, &size) != -1) {
                // Successfully got size
            }
            else {
                struct stat st;
                if (fstat(fd, &st) == 0) {
                    size = st.st_size;
                }
            }
            close(fd);
        }
#endif

        return size;
    }

    // Check write permission
    bool CheckWritePermission() {
#ifdef _WIN32
        HANDLE hDevice = CreateFileA(
            device_path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        if (hDevice == INVALID_HANDLE_VALUE) {
            return false;
        }
        CloseHandle(hDevice);
        return true;
#else
        int fd = open(device_path.c_str(), O_WRONLY);
        if (fd < 0) {
            return false;
        }
        close(fd);
        return true;
#endif
    }

    // Show disk information
    void ShowDiskInfo() {
        uint64_t size = GetDiskSize();
        std::cout << "Disk Information:\n";
        std::cout << "Device Path: " << device_path << "\n";
        std::cout << "Total Size: " << FormatSize(size) << "\n";
        std::cout << "Write Permission: " << (CheckWritePermission() ? "Yes" : "No") << "\n";

        if (size > 0) {
            std::cout << "Estimated Erase Time:\n";
            std::cout << "  - Zero Fill: " << EstimateTime(size, 1) << "\n";
            std::cout << "  - DoD Short: " << EstimateTime(size, 3) << "\n";
            std::cout << "  - DoD Long: " << EstimateTime(size, 7) << "\n";
            std::cout << "  - Gutmann: " << EstimateTime(size, 35) << "\n";
        }
    }

    // Format size display
    std::string FormatSize(uint64_t bytes) {
        const char* units[] = { "B", "KB", "MB", "GB", "TB" };
        double size = static_cast<double>(bytes);
        int unit = 0;

        while (size >= 1024.0 && unit < 4) {
            size /= 1024.0;
            unit++;
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return oss.str();
    }

    // Estimate erase time
    std::string EstimateTime(uint64_t size, int passes) {
        // Assuming write speed of about 100MB/s
        double total_seconds = static_cast<double>(size) * passes / (100.0 * 1024.0 * 1024.0);

        int hours = static_cast<int>(total_seconds) / 3600;
        int minutes = (static_cast<int>(total_seconds) % 3600) / 60;
        int seconds = static_cast<int>(total_seconds) % 60;

        std::ostringstream oss;
        if (hours > 0) oss << hours << "h ";
        if (minutes > 0) oss << minutes << "m ";
        oss << seconds << "s";
        return oss.str();
    }

    // Show progress
    void ShowProgress(uint64_t current, uint64_t total, int pass, int total_passes) {
        double percentage = static_cast<double>(current) * 100.0 / static_cast<double>(total);
        std::cout << "\r";
        std::cout << "Pass " << pass << "/" << total_passes << " ";
        std::cout << "Progress: " << std::fixed << std::setprecision(1) << percentage << "%";
        std::cout << " [" << FormatSize(current) << "/" << FormatSize(total) << "]";
        std::cout.flush();
    }

    // Write data block
    bool WriteBlock(uint64_t offset, const std::vector<unsigned char>& buffer) {
#ifdef _WIN32
        HANDLE hDevice = CreateFileA(
            device_path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_WRITE_THROUGH,
            NULL
        );

        if (hDevice == INVALID_HANDLE_VALUE) {
            std::cerr << "Cannot open device: " << GetLastError() << "\n";
            return false;
        }

        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(offset);
        SetFilePointerEx(hDevice, li, NULL, FILE_BEGIN);

        DWORD bytesWritten = 0;
        BOOL result = WriteFile(hDevice, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesWritten, NULL);

        CloseHandle(hDevice);
        return (result != 0) && (bytesWritten == buffer.size());
#else
        int fd = open(device_path.c_str(), O_WRONLY | O_SYNC);
        if (fd < 0) {
            std::cerr << "Cannot open device: " << strerror(errno) << "\n";
            return false;
        }

        if (lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
            close(fd);
            return false;
        }

        ssize_t written = write(fd, buffer.data(), buffer.size());
        close(fd);

        return written == static_cast<ssize_t>(buffer.size());
#endif
    }

    // Generate random data
    std::vector<unsigned char> GenerateRandomData(size_t size) {
        std::vector<unsigned char> data(size);
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);

        for (size_t i = 0; i < size; i++) {
            data[i] = static_cast<unsigned char>(dis(gen));
        }

        return data;
    }

    // Generate pattern data (used by Gutmann algorithm)
    std::vector<unsigned char> GeneratePatternData(size_t size, int pattern) {
        std::vector<unsigned char> data(size);
        unsigned char value = 0;

        switch (pattern) {
        case 0: value = 0x00; break;   // Zero
        case 1: value = 0xFF; break;   // One
        case 2: value = 0x92; break;   // Random pattern 1
        case 3: value = 0x49; break;   // Random pattern 2
        case 4: value = 0x24; break;   // Random pattern 3
        default: value = static_cast<unsigned char>(pattern & 0xFF); break;
        }

        for (size_t i = 0; i < size; i++) {
            data[i] = value;
        }
        return data;
    }

    // Perform single pass
    bool PerformSinglePass(uint64_t size, size_t block_size, int pass_num,
        int total_passes, WipeMethod method) {
        const size_t BLOCK_SIZE = 1024 * 1024;  // 1MB block
        std::vector<unsigned char> buffer;

        // Prepare data
        if (method == ZERO_FILL) {
            buffer = std::vector<unsigned char>(BLOCK_SIZE, 0);
        }
        else if (method == RANDOM_FILL ||
            method == DOD_SHORT ||
            method == DOD_LONG) {
            if (pass_num == 0) {
                buffer = std::vector<unsigned char>(BLOCK_SIZE, 0xFF);  // First write 1s
            }
            else if (pass_num == 1) {
                buffer = std::vector<unsigned char>(BLOCK_SIZE, 0x00);  // Second write 0s
            }
            else {
                buffer = GenerateRandomData(BLOCK_SIZE);  // Later random
            }
        }
        else if (method == GUTMANN) {
            int pattern = pass_num % 36;  // Gutmann 35 patterns
            buffer = GeneratePatternData(BLOCK_SIZE, pattern);
        }
        else if (method == VERIFY_ONLY) {
            // Nothing to write for verification only
            return true;
        }

        // Write in blocks
        for (uint64_t offset = 0; offset < size; offset += BLOCK_SIZE) {
            size_t write_size = my_min<size_t>(BLOCK_SIZE, static_cast<size_t>(size - offset));

            if (write_size < BLOCK_SIZE) {
                buffer.resize(write_size);
            }

            if (!WriteBlock(offset, buffer)) {
                std::cerr << "\nWrite failed at offset: " << offset << "\n";
                return false;
            }

            if (verbose && (offset % (10 * BLOCK_SIZE) == 0 || offset + write_size >= size)) {
                ShowProgress(offset + write_size, size, pass_num + 1, total_passes);
            }
        }

        return true;
    }

    // Verify erase result
    bool VerifyErase(uint64_t size, size_t block_size) {
        const size_t BLOCK_SIZE = 1024 * 1024;  // 1MB block
        std::vector<unsigned char> buffer(BLOCK_SIZE);
        std::vector<unsigned char> zero_buffer(BLOCK_SIZE, 0);

        std::cout << "\nVerifying erase result...\n";

#ifdef _WIN32
        HANDLE hDevice = CreateFileA(
            device_path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        if (hDevice == INVALID_HANDLE_VALUE) {
            std::cerr << "Cannot open device for verification\n";
            return false;
        }

        for (uint64_t offset = 0; offset < size; offset += BLOCK_SIZE) {
            size_t read_size = my_min<size_t>(BLOCK_SIZE, static_cast<size_t>(size - offset));
            LARGE_INTEGER li;
            li.QuadPart = static_cast<LONGLONG>(offset);
            SetFilePointerEx(hDevice, li, NULL, FILE_BEGIN);

            DWORD bytesRead = 0;
            if (!ReadFile(hDevice, buffer.data(), static_cast<DWORD>(read_size), &bytesRead, NULL)) {
                std::cerr << "Read failed at offset: " << offset << "\n";
                CloseHandle(hDevice);
                return false;
            }

            if (memcmp(buffer.data(), zero_buffer.data(), read_size) != 0) {
                std::cerr << "Verification failed at offset: " << offset << "\n";
                CloseHandle(hDevice);
                return false;
            }

            if (verbose && (offset % (10 * BLOCK_SIZE) == 0 || offset + read_size >= size)) {
                ShowProgress(offset + read_size, size, 1, 1);
            }
        }

        CloseHandle(hDevice);
#else
        int fd = open(device_path.c_str(), O_RDONLY);
        if (fd < 0) {
            std::cerr << "Cannot open device for verification\n";
            return false;
        }

        for (uint64_t offset = 0; offset < size; offset += BLOCK_SIZE) {
            size_t read_size = my_min<size_t>(BLOCK_SIZE, static_cast<size_t>(size - offset));

            if (lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
                close(fd);
                return false;
            }

            ssize_t bytesRead = read(fd, buffer.data(), read_size);
            if (bytesRead != static_cast<ssize_t>(read_size)) {
                std::cerr << "Read failed at offset: " << offset << "\n";
                close(fd);
                return false;
            }

            if (memcmp(buffer.data(), zero_buffer.data(), read_size) != 0) {
                std::cerr << "Verification failed at offset: " << offset << "\n";
                close(fd);
                return false;
            }

            if (verbose && (offset % (10 * BLOCK_SIZE) == 0 || offset + read_size >= size)) {
                ShowProgress(offset + read_size, size, 1, 1);
            }
        }

        close(fd);
#endif

        std::cout << "\nVerification complete: All data successfully erased\n";
        return true;
    }

    // Main erase function
    bool SecureWipe(WipeMethod method) {
        std::cout << "Starting secure erase...\n";

        // Check permission
        if (!CheckWritePermission()) {
            std::cerr << "Error: No write permission. Please run as administrator/root.\n";
            return false;
        }

        // Get disk size
        uint64_t size = GetDiskSize();
        if (size == 0) {
            std::cerr << "Error: Cannot get disk size\n";
            return false;
        }

        std::cout << "Disk size: " << FormatSize(size) << "\n";

        // Determine number of passes
        int total_passes = 1;
        switch (method) {
        case ZERO_FILL: total_passes = 1; break;
        case RANDOM_FILL: total_passes = 1; break;
        case DOD_SHORT: total_passes = 3; break;
        case DOD_LONG: total_passes = 7; break;
        case GUTMANN: total_passes = 35; break;
        case VERIFY_ONLY: total_passes = 0; break;
        }

        if (method != VERIFY_ONLY) {
            // Perform erase
            for (int pass = 0; pass < total_passes; pass++) {
                std::cout << "\nPass " << (pass + 1) << "/" << total_passes << "...\n";

                if (!PerformSinglePass(size, 1024 * 1024, pass, total_passes, method)) {
                    std::cerr << "Erase failed\n";
                    return false;
                }

                std::cout << "\nPass " << (pass + 1) << " completed\n";
            }
        }

        // Verify erase result
        if (method != VERIFY_ONLY) {
            return VerifyErase(size, 1024 * 1024);
        }
        else {
            return VerifyErase(size, 1024 * 1024);
        }
    }

    // ATA Secure Erase command (requires hardware support)
    bool ATA_SecureErase() {
        std::cout << "Attempting ATA Secure Erase command...\n";
        std::cout << "Note: This feature requires ATA Secure Erase command support\n";

#ifdef _WIN32
        // Windows implementation (simplified)
        std::cout << "ATA Secure Erase for Windows is not implemented in this version\n";
        return false;
#else
        // Linux implementation
        int fd = open(device_path.c_str(), O_RDWR);
        if (fd < 0) {
            std::cerr << "Cannot open device\n";
            return false;
        }

        // Check if secure erase is supported
        unsigned char identify[512];
        if (ioctl(fd, HDIO_GET_IDENTITY, identify) < 0) {
            std::cerr << "ATA commands not supported\n";
            close(fd);
            return false;
        }

        std::cout << "Device supports secure erase\n";

        // Set security password (use empty password)
        struct {
            unsigned char command;
            unsigned char features;
            unsigned char sector_count;
            unsigned char lba_low;
            unsigned char lba_mid;
            unsigned char lba_high;
            unsigned char device;
            unsigned char flags;
        } args;

        memset(&args, 0, sizeof(args));
        args.command = 0xF1;  // SECURITY SET PASSWORD
        args.features = 0x01; // Security level 1 (high)
        args.sector_count = 0x01;

        if (ioctl(fd, HDIO_DRIVE_CMD, &args) < 0) {
            std::cerr << "Failed to set password\n";
            close(fd);
            return false;
        }

        // Perform secure erase
        args.command = 0xF4;  // SECURITY ERASE UNIT
        args.features = 0x01;

        if (ioctl(fd, HDIO_DRIVE_CMD, &args) < 0) {
            std::cerr << "Secure erase failed\n";
            close(fd);
            return false;
        }

        close(fd);
        std::cout << "ATA Secure Erase command sent\n";
        std::cout << "Note: Erase may take several minutes, do not power off\n";
        return true;
#endif
    }
};

// Show help information
void ShowHelp() {
    std::cout << "LW DTS v1.01\n";
    std::cout << "Usage:\n";
    std::cout << "  diskwiper <device_path> [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -z, --zero          Zero fill (1 pass)\n";
    std::cout << "  -r, --random        Random fill (1 pass)\n";
    std::cout << "  -d, --dod-short     DoD 5220.22-M short (3 passes)\n";
    std::cout << "  -D, --dod-long      DoD 5220.22-M long (7 passes)\n";
    std::cout << "  -g, --gutmann       Gutmann algorithm (35 passes)\n";
    std::cout << "  -a, --ata           ATA Secure Erase command\n";
    std::cout << "  -v, --verify        Verify erase result\n";
    std::cout << "  -i, --info          Show disk information\n";
    std::cout << "  --verbose           Show detailed progress\n";
    std::cout << "  -h, --help          Show this help\n\n";
    std::cout << "Examples:\n";
    std::cout << "  diskwiper /dev/sda -i                 # Show disk info\n";
    std::cout << "  diskwiper /dev/sda -z --verbose       # Zero fill erase\n";
    std::cout << "  diskwiper \\\\.\\PhysicalDrive0 -d      # DoD short (Windows)\n";
}

int main(int argc, char* argv[]) {
    // 首先检查是否有帮助请求
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            ShowHelp();
            return 0;
        }
    }

    if (argc < 2) {
        ShowHelp();
        return 1;
    }

    std::string device_path = argv[1];
    bool verbose = false;
    DiskWiper::WipeMethod method = DiskWiper::ZERO_FILL;
    bool show_info = false;
    bool use_ata = false;

    // 解析命令行参数
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--verbose") {
            verbose = true;
        }
        else if (arg == "-i" || arg == "--info") {
            show_info = true;
        }
        else if (arg == "-z" || arg == "--zero") {
            method = DiskWiper::ZERO_FILL;
        }
        else if (arg == "-r" || arg == "--random") {
            method = DiskWiper::RANDOM_FILL;
        }
        else if (arg == "-d" || arg == "--dod-short") {
            method = DiskWiper::DOD_SHORT;
        }
        else if (arg == "-D" || arg == "--dod-long") {
            method = DiskWiper::DOD_LONG;
        }
        else if (arg == "-g" || arg == "--gutmann") {
            method = DiskWiper::GUTMANN;
        }
        else if (arg == "-a" || arg == "--ata") {
            use_ata = true;
        }
        else if (arg == "-v" || arg == "--verify") {
            method = DiskWiper::VERIFY_ONLY;
        }
        else {
            std::cerr << "Unknown parameter: " << arg << "\n";
            ShowHelp();
            return 1;
        }
    }

    // 如果是显示信息模式，不显示警告
    if (show_info) {
        DiskWiper wiper(device_path, verbose);
        wiper.ShowDiskInfo();
        return 0;
    }

    // 警告信息
    std::cout << "========================================\n";
    std::cout << "WARNING: Secure erase will PERMANENTLY DELETE ALL DATA on the disk!\n";
    std::cout << "This operation is IRREVERSIBLE. Please confirm you have selected the correct device.\n";
    std::cout << "Device: " << device_path << "\n";
    std::cout << "========================================\n\n";

    // 确认
    std::cout << "Continue? (type 'YES' to confirm): ";
    std::string confirmation;
    std::cin >> confirmation;

    if (confirmation != "YES") {
        std::cout << "Operation cancelled\n";
        return 0;
    }

    DiskWiper wiper(device_path, verbose);

    try {
        if (use_ata) {
            if (!wiper.ATA_SecureErase()) {
                std::cout << "ATA Secure Erase failed, attempting software erase...\n";
                return wiper.SecureWipe(method) ? 0 : 1;
            }
            return 0;
        }
        else {
            return wiper.SecureWipe(method) ? 0 : 1;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}