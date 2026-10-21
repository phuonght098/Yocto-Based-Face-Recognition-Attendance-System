#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <map>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <linux/fb.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr const char* SOCKET_PATH =
    "/run/pir-monitor/events.sock";

constexpr const char* FACE_AI =
    "/usr/bin/face-ai";

constexpr const char* ATTENDANCE_DIR =
    "/var/lib/face-ai";

constexpr const char* ATTENDANCE_CSV =
    "/var/lib/face-ai/attendance.csv";

constexpr const char* FACE_DB_PATH =
    "/var/lib/face-ai/faces.txt";

constexpr const char* PERFORMANCE_CSV =
    "/var/lib/face-ai/performance.csv";

constexpr const char* DEFAULT_BACKUP_ROOT =
    "/var/backups/face-ai";

constexpr int RECOGNITION_SECONDS = 8;

/* Minimum score to accept a face. Tune against real face-ai output. */
constexpr float MIN_RECOGNITION_SCORE = 0.50f;

constexpr std::size_t MAX_NAME_LEN = 64;
constexpr std::size_t MAX_CSV_BYTES = 8u * 1024u * 1024u;

/*
 * The Pi 4 has no RTC. Refuse to record while the clock is obviously
 * wrong (before NTP / fake-hwclock restores it), otherwise the daily
 * dedup would use a bogus date.
 */
constexpr int MIN_VALID_YEAR = 2018;

volatile sig_atomic_t running = 1;

void signal_handler(int)
{
    running = 0;
}

bool is_space(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string utc_timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};

    if (!gmtime_r(&now, &tm))
        return "unknown-time";

    char buf[32];

    std::strftime(
        buf, sizeof(buf),
        "%Y-%m-%dT%H:%M:%SZ", &tm);

    return buf;
}

/* "YYYY-MM-DD" in UTC. */
std::string utc_date()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};

    if (!gmtime_r(&now, &tm))
        return "";

    char buf[16];

    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);

    return buf;
}

bool clock_is_valid()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};

    if (!gmtime_r(&now, &tm))
        return false;

    return tm.tm_year + 1900 >= MIN_VALID_YEAR;
}

std::string trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
        return "";

    const auto last = s.find_last_not_of(" \t\r\n");

    return s.substr(first, last - first + 1);
}

std::string upper(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(
            std::toupper(static_cast<unsigned char>(c)));

    return s;
}

std::string csv_escape(const std::string& s)
{
    std::string out = "\"";

    for (char c : s) {
        if (c == '"')
            out += "\"\"";
        else
            out += c;
    }

    out += '"';
    return out;
}

std::vector<std::string> split_csv(const std::string& line)
{
    std::vector<std::string> fields;
    std::string cur;
    bool in_quotes = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];

        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur += '"';
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                cur += c;
            }
        } else if (c == '"') {
            in_quotes = true;
        } else if (c == ',') {
            fields.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }

    fields.push_back(cur);
    return fields;
}

/*
 * Minimal RGB565 framebuffer output.
 * Designed for the 128x160 TFT at /dev/fb0.
 */
class TFT {
public:
    TFT()
    {
        fd_ = open("/dev/fb0", O_RDWR);

        if (fd_ < 0) {
            std::cerr << "Cannot open /dev/fb0: "
                      << strerror(errno) << '\n';
            return;
        }

        if (ioctl_fb() < 0) {
            close(fd_);
            fd_ = -1;
            return;
        }

        if (var_.bits_per_pixel != 16) {
            std::cerr << "TFT requires RGB565 (16 bpp); "
                      << "actual bpp="
                      << var_.bits_per_pixel << '\n';
            close(fd_);
            fd_ = -1;
            return;
        }

        size_ = fix_.smem_len;

        if (size_ == 0)
            size_ = fix_.line_length * var_.yres_virtual;

        fb_ = static_cast<unsigned char*>(
            mmap(nullptr, size_,
                 PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd_, 0));

        if (fb_ == MAP_FAILED) {
            fb_ = nullptr;
            close(fd_);
            fd_ = -1;
            return;
        }

        available_ = true;
    }

    ~TFT()
    {
        if (fb_)
            munmap(fb_, size_);

        if (fd_ >= 0)
            close(fd_);
    }

    void show(const std::vector<std::string>& lines)
    {
        if (!available_)
            return;

        std::memset(fb_, 0, size_);

        int x = 2;
        int y = 4;

        for (std::string line : lines) {
            line = upper(line);

            for (char c : line) {
                if (c == '\n') {
                    x = 2;
                    y += 18;
                    continue;
                }

                if (x + 12 >= static_cast<int>(var_.xres)) {
                    x = 2;
                    y += 18;
                }

                if (y + 14 >= static_cast<int>(var_.yres))
                    return;

                draw_char(x, y, c);
                x += 12;
            }

            x = 2;
            y += 20;

            if (y + 14 >= static_cast<int>(var_.yres))
                break;
        }
    }

private:
    int fd_ = -1;
    bool available_ = false;

    unsigned char* fb_ = nullptr;
    size_t size_ = 0;

    fb_fix_screeninfo fix_{};
    fb_var_screeninfo var_{};

    int ioctl_fb()
    {
        if (ioctl(fd_, FBIOGET_FSCREENINFO, &fix_) < 0) {
            std::cerr << "FBIOGET_FSCREENINFO failed\n";
            return -1;
        }

        if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_) < 0) {
            std::cerr << "FBIOGET_VSCREENINFO failed\n";
            return -1;
        }

        return 0;
    }

    void pixel(int x, int y, uint16_t color)
    {
        if (!fb_ || x < 0 || y < 0 ||
            x >= static_cast<int>(var_.xres) ||
            y >= static_cast<int>(var_.yres))
            return;

        const size_t offset =
            static_cast<size_t>(y + var_.yoffset)
                * fix_.line_length
            + static_cast<size_t>(x + var_.xoffset) * 2;

        if (offset + 1 >= size_)
            return;

        std::memcpy(fb_ + offset, &color, sizeof(color));
    }

    std::array<uint8_t, 5> glyph(char c)
    {
        switch (c) {
        case 'A': return {0x7E,0x11,0x11,0x11,0x7E};
        case 'B': return {0x7F,0x49,0x49,0x49,0x36};
        case 'C': return {0x3E,0x41,0x41,0x41,0x22};
        case 'D': return {0x7F,0x41,0x41,0x22,0x1C};
        case 'E': return {0x7F,0x49,0x49,0x49,0x41};
        case 'F': return {0x7F,0x09,0x09,0x09,0x01};
        case 'G': return {0x3E,0x41,0x49,0x49,0x7A};
        case 'H': return {0x7F,0x08,0x08,0x08,0x7F};
        case 'I': return {0x00,0x41,0x7F,0x41,0x00};
        case 'J': return {0x20,0x40,0x41,0x3F,0x01};
        case 'K': return {0x7F,0x08,0x14,0x22,0x41};
        case 'L': return {0x7F,0x40,0x40,0x40,0x40};
        case 'M': return {0x7F,0x02,0x0C,0x02,0x7F};
        case 'N': return {0x7F,0x04,0x08,0x10,0x7F};
        case 'O': return {0x3E,0x41,0x41,0x41,0x3E};
        case 'P': return {0x7F,0x09,0x09,0x09,0x06};
        case 'Q': return {0x3E,0x41,0x51,0x21,0x5E};
        case 'R': return {0x7F,0x09,0x19,0x29,0x46};
        case 'S': return {0x46,0x49,0x49,0x49,0x31};
        case 'T': return {0x01,0x01,0x7F,0x01,0x01};
        case 'U': return {0x3F,0x40,0x40,0x40,0x3F};
        case 'V': return {0x1F,0x20,0x40,0x20,0x1F};
        case 'W': return {0x3F,0x40,0x38,0x40,0x3F};
        case 'X': return {0x63,0x14,0x08,0x14,0x63};
        case 'Y': return {0x07,0x08,0x70,0x08,0x07};
        case 'Z': return {0x61,0x51,0x49,0x45,0x43};

        case '0': return {0x3E,0x51,0x49,0x45,0x3E};
        case '1': return {0x00,0x42,0x7F,0x40,0x00};
        case '2': return {0x42,0x61,0x51,0x49,0x46};
        case '3': return {0x21,0x41,0x45,0x4B,0x31};
        case '4': return {0x18,0x14,0x12,0x7F,0x10};
        case '5': return {0x27,0x45,0x45,0x45,0x39};
        case '6': return {0x3C,0x4A,0x49,0x49,0x30};
        case '7': return {0x01,0x71,0x09,0x05,0x03};
        case '8': return {0x36,0x49,0x49,0x49,0x36};
        case '9': return {0x06,0x49,0x49,0x29,0x1E};

        case ':': return {0x00,0x36,0x36,0x00,0x00};
        case '.': return {0x00,0x60,0x60,0x00,0x00};
        case '-': return {0x08,0x08,0x08,0x08,0x08};
        case '/': return {0x20,0x10,0x08,0x04,0x02};
        case '_': return {0x40,0x40,0x40,0x40,0x40};
        case ' ': return {0,0,0,0,0};
        default:  return {0,0,0,0,0};
        }
    }

    void draw_char(int x, int y, char c)
    {
        const auto bits = glyph(c);

        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row < 7; ++row) {
                if (bits[col] & (1u << row)) {
                    for (int dx = 0; dx < 2; ++dx) {
                        for (int dy = 0; dy < 2; ++dy)
                            pixel(x + col * 2 + dx,
                                  y + row * 2 + dy,
                                  0xFFFF);
                    }
                }
            }
        }
    }
};

/* ------------------------------------------------------------------ */
/* Persistent attendance log                                           */
/* ------------------------------------------------------------------ */

enum class RecordResult {
    Recorded,
    AlreadyToday,
    ClockInvalid,
    Error
};

struct FdGuard {
    int fd;
    explicit FdGuard(int f) : fd(f) {}
    ~FdGuard() { if (fd >= 0) close(fd); }  // close() also drops flock
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
};

bool write_all(int fd, const std::string& data)
{
    std::size_t done = 0;

    while (done < data.size()) {
        const ssize_t n =
            write(fd, data.data() + done, data.size() - done);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }

        done += static_cast<std::size_t>(n);
    }

    return true;
}

bool sync_directory(const char* path)
{
    const int dfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (dfd < 0)
        return false;

    const bool ok = fsync(dfd) == 0;
    close(dfd);
    return ok;
}

/* Copy one regular file to a destination using a temporary file + rename.
 * O_NOFOLLOW prevents a symlink in the source path from being followed.
 */
bool copy_file_atomic(const std::string& source,
                      const std::string& destination)
{
    const int in = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0)
        return false;

    FdGuard input_guard(in);
    struct stat source_stat{};
    if (fstat(in, &source_stat) < 0 || !S_ISREG(source_stat.st_mode))
        return false;

    if (flock(in, LOCK_SH) < 0)
        return false;

    const std::string temporary = destination + ".tmp." +
                                  std::to_string(static_cast<long long>(getpid()));
    const int out = open(temporary.c_str(),
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                         0600);
    if (out < 0)
        return false;

    bool ok = true;
    char buffer[16384];
    for (;;) {
        const ssize_t n = read(in, buffer, sizeof(buffer));
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (!write_all(out, std::string(buffer, static_cast<std::size_t>(n)))) {
            ok = false;
            break;
        }
    }

    if (ok && fsync(out) < 0)
        ok = false;
    if (close(out) < 0)
        ok = false;

    if (ok && rename(temporary.c_str(), destination.c_str()) < 0)
        ok = false;

    if (!ok)
        unlink(temporary.c_str());

    const auto slash = destination.find_last_of('/');
    const std::string parent = slash == std::string::npos
        ? "." : destination.substr(0, slash);
    if (ok && !sync_directory(parent.c_str()))
        ok = false;

    return ok;
}

bool ensure_directory(const std::string& path, mode_t mode)
{
    if (mkdir(path.c_str(), mode) == 0)
        return true;
    return errno == EEXIST;
}

std::string backup_timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    if (!gmtime_r(&now, &tm))
        return "unknown-time";
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

bool write_text_file(const std::string& path, const std::string& content)
{
    const int fd = open(path.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                        0600);
    if (fd < 0)
        return false;
    const bool ok = write_all(fd, content) && fsync(fd) == 0;
    const bool close_ok = close(fd) == 0;
    return ok && close_ok;
}

void prune_old_backups(const std::string& root, std::size_t keep = 7)
{
    DIR* dir = opendir(root.c_str());
    if (!dir)
        return;
    std::vector<std::string> backups;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind("backup-", 0) != 0)
            continue;
        struct stat st{};
        const std::string full = root + "/" + name;
        if (lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            backups.push_back(name);
    }
    closedir(dir);
    std::sort(backups.begin(), backups.end());
    while (backups.size() > keep) {
        const std::string old = root + "/" + backups.front();
        const char* files[] = {"faces.txt", "attendance.csv", "performance.csv", "manifest.txt"};
        for (const char* file : files) {
            const std::string path = old + "/" + file;
            if (unlink(path.c_str()) < 0 && errno != ENOENT)
                std::cerr << "Warning: cannot remove old backup file " << path << '\n';
        }
        if (rmdir(old.c_str()) < 0)
            std::cerr << "Warning: cannot remove old backup directory " << old << '\n';
        backups.erase(backups.begin());
    }
}

int create_backup(const std::string& root)
{
    if (!ensure_directory(root, 0700)) {
        std::cerr << "Cannot create backup root " << root
                  << ": " << strerror(errno) << '\n';
        return 1;
    }

    const std::string backup_dir = root + "/backup-" + backup_timestamp();
    if (mkdir(backup_dir.c_str(), 0700) < 0) {
        std::cerr << "Cannot create backup directory " << backup_dir
                  << ": " << strerror(errno) << '\n';
        return 1;
    }

    struct Item { const char* source; const char* name; };
    const Item items[] = {
        {FACE_DB_PATH, "faces.txt"},
        {ATTENDANCE_CSV, "attendance.csv"},
        {PERFORMANCE_CSV, "performance.csv"}
    };

    std::ostringstream manifest;
    manifest << "backup_timestamp_utc=" << utc_timestamp() << '\n';
    manifest << "format=attendance-app-backup-v1\n";
    unsigned copied = 0;
    for (const auto& item : items) {
        struct stat st{};
        if (lstat(item.source, &st) < 0) {
            if (errno == ENOENT) {
                manifest << "missing=" << item.name << '\n';
                continue;
            }
            std::cerr << "Cannot inspect " << item.source << '\n';
            return 1;
        }
        if (!S_ISREG(st.st_mode)) {
            std::cerr << "Refusing non-regular backup source: "
                      << item.source << '\n';
            return 1;
        }
        const std::string destination = backup_dir + "/" + item.name;
        if (!copy_file_atomic(item.source, destination)) {
            std::cerr << "Backup copy failed: " << item.source
                      << " -> " << destination << ": "
                      << strerror(errno) << '\n';
            return 1;
        }
        manifest << "included=" << item.name << '\n';
        ++copied;
    }

    if (copied == 0) {
        std::cerr << "No data files found; empty backup refused\n";
        return 1;
    }

    if (!write_text_file(backup_dir + "/manifest.txt", manifest.str()) ||
        !sync_directory(backup_dir.c_str()) ||
        !sync_directory(root.c_str())) {
        std::cerr << "Failed to finalize backup metadata\n";
        return 1;
    }

    prune_old_backups(root, 7);
    std::cout << "Backup completed: " << backup_dir << '\n';
    std::cout << "Files copied: " << copied << '\n';
    return 0;
}

int restore_backup(const std::string& backup_dir)
{
    const std::string manifest_path = backup_dir + "/manifest.txt";
    const int manifest_fd = open(manifest_path.c_str(),
                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (manifest_fd < 0) {
        std::cerr << "Backup manifest missing/unreadable: "
                  << manifest_path << '\n';
        return 1;
    }
    FdGuard manifest_guard(manifest_fd);
    struct stat manifest_stat{};
    if (fstat(manifest_fd, &manifest_stat) < 0 ||
        !S_ISREG(manifest_stat.st_mode) || manifest_stat.st_size > 65536) {
        std::cerr << "Invalid backup manifest\n";
        return 1;
    }
    std::string manifest(static_cast<std::size_t>(manifest_stat.st_size), '\0');
    std::size_t offset = 0;
    while (offset < manifest.size()) {
        const ssize_t n = read(manifest_fd, &manifest[offset], manifest.size() - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            std::cerr << "Cannot read backup manifest\n";
            return 1;
        }
        offset += static_cast<std::size_t>(n);
    }
    if (manifest.find("format=attendance-app-backup-v1\n") == std::string::npos) {
        std::cerr << "Unsupported backup format\n";
        return 1;
    }

    struct Item { const char* name; const char* destination; };
    const Item items[] = {
        {"faces.txt", FACE_DB_PATH},
        {"attendance.csv", ATTENDANCE_CSV},
        {"performance.csv", PERFORMANCE_CSV}
    };

    if (!ensure_directory(ATTENDANCE_DIR, 0755)) {
        std::cerr << "Cannot create data directory: " << strerror(errno) << '\n';
        return 1;
    }

    unsigned restored = 0;
    for (const auto& item : items) {
        if (manifest.find(std::string("included=") + item.name + "\n") == std::string::npos)
            continue;
        const std::string source = backup_dir + "/" + item.name;
        if (!copy_file_atomic(source, item.destination)) {
            std::cerr << "Restore failed for " << item.name
                      << ": " << strerror(errno) << '\n';
            return 1;
        }
        ++restored;
    }

    if (restored == 0) {
        std::cerr << "Backup manifest contains no restorable data files\n";
        return 1;
    }

    std::cout << "Restore completed from: " << backup_dir << '\n';
    std::cout << "Files restored: " << restored << '\n';
    std::cout << "Restart attendance-app after restoring live data.\n";
    return 0;
}

bool parse_nonnegative_ms(const std::string& text, long long& value)
{
    if (text.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (errno || end == text.c_str() || *end != '\0' || parsed < 0)
        return false;
    value = parsed;
    return true;
}

void append_performance_record(long long recognition_ms,
                               long long total_ms,
                               const std::string& outcome)
{
    if (mkdir(ATTENDANCE_DIR, 0755) < 0 && errno != EEXIST) {
        std::cerr << "Cannot create performance data directory: "
                  << strerror(errno) << '\n';
        return;
    }
    const int fd = open(PERFORMANCE_CSV,
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                        0644);
    if (fd < 0) {
        std::cerr << "Cannot open performance log: " << strerror(errno) << '\n';
        return;
    }
    FdGuard guard(fd);
    if (flock(fd, LOCK_EX) < 0) {
        std::cerr << "Cannot lock performance log: " << strerror(errno) << '\n';
        return;
    }
    struct stat st{};
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        std::cerr << "Invalid performance log file\n";
        return;
    }
    std::string row;
    if (st.st_size == 0)
        row = "timestamp_utc,recognition_ms,total_ms,outcome\n";
    row += utc_timestamp() + "," + std::to_string(recognition_ms) + "," +
           std::to_string(total_ms) + "," + csv_escape(outcome) + "\n";
    if (!write_all(fd, row) || fdatasync(fd) < 0)
        std::cerr << "Cannot persist performance metrics: " << strerror(errno) << '\n';
}

int performance_report()
{
    std::ifstream input(PERFORMANCE_CSV);
    if (!input) {
        std::cerr << "No performance data yet: " << PERFORMANCE_CSV << '\n';
        return 1;
    }

    std::vector<long long> recognition, total;
    std::vector<std::string> outcomes;
    std::string line;
    bool first = true;
    while (std::getline(input, line)) {
        if (first) { first = false; continue; }
        const auto fields = split_csv(line);
        if (fields.size() != 4)
            continue;
        long long r = 0, t = 0;
        if (!parse_nonnegative_ms(fields[1], r) ||
            !parse_nonnegative_ms(fields[2], t))
            continue;
        recognition.push_back(r);
        total.push_back(t);
        outcomes.push_back(fields[3]);
    }
    if (total.empty()) {
        std::cerr << "Performance log has no valid samples\n";
        return 1;
    }

    auto percentile = [](std::vector<long long> values, double p) {
        std::sort(values.begin(), values.end());
        const std::size_t index = static_cast<std::size_t>(
            std::ceil(p * static_cast<double>(values.size())) - 1.0);
        return values[std::min(index, values.size() - 1)];
    };
    auto average = [](const std::vector<long long>& values) {
        long double sum = 0;
        for (long long value : values) sum += value;
        return static_cast<double>(sum / values.size());
    };

    unsigned recognized = 0;
    for (const auto& outcome : outcomes)
        if (outcome == "recognized" || outcome == "already_today")
            ++recognized;

    std::cout << "Performance report (" << total.size() << " motion events)\n";
    std::cout << "Recognition latency ms: avg=" << std::fixed << std::setprecision(1)
              << average(recognition) << " p50=" << percentile(recognition, 0.50)
              << " p95=" << percentile(recognition, 0.95)
              << " max=" << *std::max_element(recognition.begin(), recognition.end()) << '\n';
    std::cout << "End-to-end latency ms: avg=" << average(total)
              << " p50=" << percentile(total, 0.50)
              << " p95=" << percentile(total, 0.95)
              << " max=" << *std::max_element(total.begin(), total.end()) << '\n';
    std::cout << "Recognized/already recorded: " << recognized << "/" << total.size()
              << " (" << std::setprecision(1)
              << 100.0 * recognized / total.size() << "%)\n";
    std::map<std::string, unsigned> outcome_counts;
    for (const auto& outcome : outcomes)
        ++outcome_counts[outcome];
    std::cout << "Outcomes:\n";
    for (const auto& entry : outcome_counts)
        std::cout << "  " << entry.first << ": " << entry.second << '\n';
    return 0;
}

/*
 * True if `name` already has a "recognized" row for `today`
 * (UTC date, "YYYY-MM-DD"). `content` must contain complete lines only.
 */
bool already_recorded_today(const std::string& content,
                            const std::string& name,
                            const std::string& today)
{
    const std::string wanted = upper(name);

    std::istringstream input(content);
    std::string line;

    while (std::getline(input, line)) {
        const auto f = split_csv(line);

        if (f.size() < 3)
            continue;

        // timestamp_utc = "YYYY-MM-DDTHH:MM:SSZ"
        if (f[0].size() != 20 || f[0].compare(0, 10, today) != 0)
            continue;

        if (f[2] != "recognized")
            continue;

        if (upper(trim(f[1])) == wanted)
            return true;
    }

    return false;
}

/*
 * Atomic check-and-append under an exclusive flock():
 *  1. drop an incomplete last line left by a power cut,
 *  2. refuse if this person already has a row for today's UTC date,
 *  3. append one row with a single write() and fdatasync().
 */
RecordResult record_attendance(const std::string& name)
{
    if (!clock_is_valid()) {
        std::cerr << "System clock invalid; attendance refused\n";
        return RecordResult::ClockInvalid;
    }

    const std::string today = utc_date();

    if (today.size() != 10)
        return RecordResult::Error;

    if (mkdir(ATTENDANCE_DIR, 0755) < 0 && errno != EEXIST) {
        std::cerr << "Cannot create attendance directory: "
                  << strerror(errno) << '\n';
        return RecordResult::Error;
    }

    const int raw_fd = open(
        ATTENDANCE_CSV,
        O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

    if (raw_fd < 0) {
        std::cerr << "Cannot open " << ATTENDANCE_CSV << ": "
                  << strerror(errno) << '\n';
        return RecordResult::Error;
    }

    FdGuard guard(raw_fd);
    const int fd = guard.fd;

    while (flock(fd, LOCK_EX) < 0) {
        if (errno == EINTR)
            continue;

        std::cerr << "flock failed: " << strerror(errno) << '\n';
        return RecordResult::Error;
    }

    struct stat st{};

    if (fstat(fd, &st) < 0) {
        std::cerr << "fstat failed: " << strerror(errno) << '\n';
        return RecordResult::Error;
    }

    if (static_cast<std::size_t>(st.st_size) > MAX_CSV_BYTES) {
        std::cerr << "Attendance file too large\n";
        return RecordResult::Error;
    }

    std::string content(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t got = 0;

    while (got < content.size()) {
        const ssize_t n = pread(
            fd, &content[got], content.size() - got,
            static_cast<off_t>(got));

        if (n < 0) {
            if (errno == EINTR)
                continue;

            std::cerr << "pread failed: " << strerror(errno) << '\n';
            return RecordResult::Error;
        }

        if (n == 0)
            break;

        got += static_cast<std::size_t>(n);
    }

    content.resize(got);

    // Recover from a torn last line (power loss in the middle of a write).
    if (!content.empty() && content.back() != '\n') {
        const auto nl = content.rfind('\n');
        const std::size_t keep =
            nl == std::string::npos ? 0 : nl + 1;

        std::cerr << "Dropping incomplete last CSV line ("
                  << content.size() - keep << " bytes)\n";

        if (ftruncate(fd, static_cast<off_t>(keep)) < 0) {
            std::cerr << "ftruncate failed: "
                      << strerror(errno) << '\n';
            return RecordResult::Error;
        }

        content.resize(keep);

        if (fdatasync(fd) < 0) {
            std::cerr << "fdatasync failed: "
                      << strerror(errno) << '\n';
            return RecordResult::Error;
        }
    }

    if (already_recorded_today(content, name, today))
        return RecordResult::AlreadyToday;

    const bool new_file = content.empty();

    std::string payload;

    if (new_file)
        payload += "timestamp_utc,name,result\n";

    payload += utc_timestamp();
    payload += ',';
    payload += csv_escape(name);
    payload += ',';
    payload += csv_escape("recognized");
    payload += '\n';

    if (!write_all(fd, payload)) {
        std::cerr << "write failed: " << strerror(errno) << '\n';

        // Roll back a partial row so the file stays well-formed.
        if (ftruncate(fd, static_cast<off_t>(content.size())) < 0)
            std::cerr << "rollback ftruncate failed\n";

        return RecordResult::Error;
    }

    if (fdatasync(fd) < 0) {
        std::cerr << "fdatasync failed: " << strerror(errno) << '\n';
        return RecordResult::Error;
    }

    if (new_file && !sync_directory(ATTENDANCE_DIR))
        std::cerr << "Warning: directory fsync failed\n";

    return RecordResult::Recorded;
}

/* ------------------------------------------------------------------ */
/* face-ai                                                             */
/* ------------------------------------------------------------------ */

/*
 * Start face-ai for a bounded recognition window.
 * Send SIGINT to let face-ai exit through its normal signal handler.
 */
std::string run_face_ai()
{
    // Keep face-ai's own matching threshold aligned with the threshold
    // enforced again by parse_recognized_name().
    char arg0[] = "/usr/bin/face-ai";
    char arg_threshold[] = "--threshold";
    char threshold_value[16];
    std::snprintf(threshold_value, sizeof(threshold_value), "%.2f",
                  MIN_RECOGNITION_SCORE);

    int pipefd[2];

    if (pipe(pipefd) < 0) {
        std::cerr << "pipe failed: " << strerror(errno) << '\n';
        return "";
    }

    const pid_t pid = fork();

    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "";
    }

    if (pid == 0) {
        close(pipefd[0]);

        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        char* argv[] = {
            arg0,
            arg_threshold,
            threshold_value,
            nullptr
        };

        execv(FACE_AI, argv);
        _exit(127);
    }

    close(pipefd[1]);

    const int flags = fcntl(pipefd[0], F_GETFL, 0);

    if (flags >= 0)
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    std::string output;
    char buffer[4096];

    const auto deadline =
        std::chrono::steady_clock::now()
        + std::chrono::seconds(RECOGNITION_SECONDS);

    bool child_exited = false;
    int status = 0;

    while (running &&
           std::chrono::steady_clock::now() < deadline) {
        struct pollfd pfd{};
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;

        poll(&pfd, 1, 100);

        if (pfd.revents & (POLLIN | POLLHUP)) {
            for (;;) {
                const ssize_t n =
                    read(pipefd[0], buffer, sizeof(buffer));

                if (n > 0) {
                    output.append(buffer, n);
                    continue;
                }

                break;
            }
        }

        const pid_t w = waitpid(pid, &status, WNOHANG);

        if (w == pid) {
            child_exited = true;
            break;
        }

        if (w < 0 && errno != EINTR) {
            child_exited = true;
            break;
        }
    }

    if (!child_exited) {
        kill(pid, SIGINT);

        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR)
                break;
        }
    }

    for (;;) {
        const ssize_t n =
            read(pipefd[0], buffer, sizeof(buffer));

        if (n > 0) {
            output.append(buffer, n);
            continue;
        }

        if (n < 0 && errno == EINTR)
            continue;

        break;
    }

    close(pipefd[0]);

    return output;
}

bool valid_name(const std::string& name)
{
    if (name.empty() || name.size() > MAX_NAME_LEN)
        return false;

    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);

        if (u < 0x20 || u == 0x7F)
            return false;
    }

    const std::string u = upper(name);

    return u != "UNKNOWN" && u != "NO_MATCH";
}

/*
 * Accept both the legacy face-ai format and the current face-ai format:
 *
 *   RESULT: face <index> <name> score=<float>
 *   RESULT: face <index> "<quoted name>" score=<float>
 *   RESULT: face <index> = <name> (det=<float>, cosine=<float>)
 *
 * The current face-ai's cosine similarity is the recognition score. The
 * detector confidence (det=) is validated but is NOT used as the identity
 * score. Malformed lines and UNKNOWN identities are rejected.
 */
bool parse_result_line(const std::string& line,
                       std::string& name,
                       float& score)
{
    static const std::string prefix = "RESULT: face ";

    const auto pos = line.find(prefix);
    if (pos == std::string::npos)
        return false;

    std::string rest = line.substr(pos + prefix.size());

    // Face index (digits), followed by whitespace.
    std::size_t i = 0;
    while (i < rest.size() &&
           std::isdigit(static_cast<unsigned char>(rest[i])))
        ++i;

    if (i == 0 || i >= rest.size() || !is_space(rest[i]))
        return false;

    rest = trim(rest.substr(i));

    auto parse_float_strict = [](const std::string& text, float& value) {
        const std::string token = trim(text);
        if (token.empty())
            return false;

        char* end = nullptr;
        errno = 0;
        const float parsed = std::strtof(token.c_str(), &end);

        if (end == token.c_str() || errno == ERANGE ||
            !std::isfinite(parsed) || !trim(std::string(end)).empty())
            return false;

        value = parsed;
        return true;
    };

    std::string parsed_name;
    float parsed_score = 0.0f;

    // Current face-ai format:
    //   = Phuong (det=0.950, cosine=0.721)
    if (!rest.empty() && rest.front() == '=') {
        rest = trim(rest.substr(1));
        const auto det_pos = rest.find(" (det=");
        const auto cosine_pos = rest.rfind(", cosine=");

        if (det_pos == std::string::npos ||
            cosine_pos == std::string::npos ||
            cosine_pos <= det_pos + 6 || rest.empty() ||
            rest.back() != ')')
            return false;

        parsed_name = trim(rest.substr(0, det_pos));

        float detection_score = 0.0f;
        if (!parse_float_strict(
                rest.substr(det_pos + 6,
                            cosine_pos - (det_pos + 6)),
                detection_score))
            return false;

        // Exclude the final ')' and parse cosine similarity strictly.
        const std::size_t score_start = cosine_pos + 9;
        if (score_start >= rest.size() - 1 ||
            !parse_float_strict(
                rest.substr(score_start, rest.size() - score_start - 1),
                parsed_score))
            return false;
    } else {
        // Legacy format retained for compatibility:
        //   Phuong score=0.721
        const auto sp = rest.rfind("score=");
        if (sp == std::string::npos || sp == 0 ||
            !is_space(rest[sp - 1]))
            return false;

        if (!parse_float_strict(rest.substr(sp + 6), parsed_score))
            return false;

        parsed_name = trim(rest.substr(0, sp));
        if (!parsed_name.empty() && parsed_name.front() == '"') {
            std::istringstream in(parsed_name);
            std::string quoted_name;

            if (!(in >> std::quoted(quoted_name)))
                return false;

            in >> std::ws;
            if (in.peek() != std::char_traits<char>::eof())
                return false;

            parsed_name = quoted_name;
        }
    }

    parsed_name = trim(parsed_name);
    if (!valid_name(parsed_name))
        return false;

    name = parsed_name;
    score = parsed_score;
    return true;
}

/* Returns the best registered name with score >= threshold, or "". */
std::string parse_recognized_name(const std::string& output)
{
    std::string best_name;
    float best_score = -1.0e30f;

    std::istringstream input(output);
    std::string line;

    while (std::getline(input, line)) {
        std::string name;
        float score = 0.0f;

        if (!parse_result_line(line, name, score))
            continue;

        if (score < MIN_RECOGNITION_SCORE)
            continue;

        if (score > best_score) {
            best_score = score;
            best_name = name;
        }
    }

    return best_name;
}

int connect_pir_socket()
{
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    std::snprintf(
        addr.sun_path, sizeof(addr.sun_path),
        "%s", SOCKET_PATH);

    if (connect(fd,
                reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

void handle_motion(TFT& tft)
{
    const auto event_start = std::chrono::steady_clock::now();
    std::cout << '[' << utc_timestamp()
              << "] Motion detected; starting recognition\n";
    std::cout.flush();

    tft.show({"MOTION", "CHECKING FACE"});

    const auto recognition_start = std::chrono::steady_clock::now();
    const std::string output = run_face_ai();
    const auto recognition_end = std::chrono::steady_clock::now();
    const long long recognition_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        recognition_end - recognition_start).count();
    const std::string name = parse_recognized_name(output);

    if (name.empty()) {
        std::cout << '[' << utc_timestamp()
                  << "] No registered face recognized\n";
        tft.show({"UNKNOWN FACE", "TRY AGAIN"});
        const long long total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - event_start).count();
        append_performance_record(recognition_ms, total_ms, "unrecognized");
        std::cout << "PERF: recognition_ms=" << recognition_ms
                  << " total_ms=" << total_ms << " outcome=unrecognized\n";
        std::cout.flush();
        return;
    }

    std::string outcome;
    switch (record_attendance(name)) {
    case RecordResult::Recorded:
        outcome = "recognized";
        std::cout << '[' << utc_timestamp()
                  << "] ATTENDANCE RECORDED: " << name << '\n';
        tft.show({"WELCOME", name, "RECORDED"});
        break;

    case RecordResult::AlreadyToday:
        outcome = "already_today";
        std::cout << '[' << utc_timestamp()
                  << "] Already recorded today: " << name << '\n';
        tft.show({"WELCOME", name, "ALREADY TODAY"});
        break;

    case RecordResult::ClockInvalid:
        outcome = "clock_error";
        std::cout << '[' << utc_timestamp()
                  << "] Clock invalid; not recording\n";
        tft.show({"CLOCK ERROR", "NOT RECORDED"});
        break;

    case RecordResult::Error:
        outcome = "csv_error";
        std::cout << '[' << utc_timestamp()
                  << "] CSV ERROR for: " << name << '\n';
        tft.show({"CSV ERROR", name});
        break;
    }

    const long long total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - event_start).count();
    append_performance_record(recognition_ms, total_ms, outcome);
    std::cout << "PERF: recognition_ms=" << recognition_ms
              << " total_ms=" << total_ms << " outcome=" << outcome << '\n';
    std::cout.flush();
}

} // namespace

void print_usage(const char* program)
{
    std::cout << "Usage:\n"
              << "  " << program << "                 Run attendance service\n"
              << "  " << program << " --backup [BACKUP_ROOT]\n"
              << "  " << program << " --restore BACKUP_DIR\n"
              << "  " << program << " --perf-report\n"
              << "  " << program << " --help\n";
}

int main(int argc, char** argv)
{
    if (argc >= 2) {
        const std::string command = argv[1];
        if (command == "--help" || command == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (command == "--backup") {
            if (argc > 3) { print_usage(argv[0]); return 2; }
            return create_backup(argc == 3 ? argv[2] : DEFAULT_BACKUP_ROOT);
        }
        if (command == "--restore") {
            if (argc != 3) { print_usage(argv[0]); return 2; }
            return restore_backup(argv[2]);
        }
        if (command == "--perf-report") {
            if (argc != 2) { print_usage(argv[0]); return 2; }
            return performance_report();
        }
        std::cerr << "Unknown option: " << command << '\n';
        print_usage(argv[0]);
        return 2;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    TFT tft;
    tft.show({"ATTENDANCE", "SYSTEM START"});

    std::cout << "attendance-app started\n";
    std::cout << "PIR socket: " << SOCKET_PATH << '\n';
    std::cout << "CSV: " << ATTENDANCE_CSV << '\n';
    std::cout << "Min score: " << MIN_RECOGNITION_SCORE << '\n';
    std::cout << "Performance log: " << PERFORMANCE_CSV << '\n';
    std::cout.flush();

    std::string last_auto_backup_date;
    auto maybe_auto_backup = [&]() {
        if (!clock_is_valid())
            return;
        const std::string date = utc_date();
        if (date.empty() || date == last_auto_backup_date)
            return;
        std::cout << "Starting daily data backup for " << date << '\n';
        const int result = create_backup(DEFAULT_BACKUP_ROOT);
        last_auto_backup_date = date; // avoid retry loops if storage is unavailable
        if (result != 0)
            std::cerr << "Automatic backup failed for " << date << '\n';
        std::cout.flush();
    };

    maybe_auto_backup();
    while (running) {
        maybe_auto_backup();
        const int fd = connect_pir_socket();

        if (fd < 0) {
            std::cerr << "Waiting for PIR socket: "
                      << strerror(errno) << '\n';

            tft.show({"WAITING PIR"});
            std::this_thread::sleep_for(
                std::chrono::seconds(2));
            continue;
        }

        std::cout << "Connected to PIR event socket\n";
        std::cout.flush();

        tft.show({"PIR CONNECTED", "WAIT MOTION"});

        std::string pending;
        char buffer[1024];

        while (running) {
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;

            const int pr = poll(&pfd, 1, 1000);

            if (pr < 0) {
                if (errno == EINTR)
                    continue;

                break;
            }

            if (pr == 0) {
                maybe_auto_backup();
                continue;
            }

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
                break;

            if (!(pfd.revents & POLLIN))
                continue;

            const ssize_t n = read(fd, buffer, sizeof(buffer));

            if (n <= 0)
                break;

            pending.append(buffer, static_cast<size_t>(n));

            if (pending.size() > 16384) {
                std::cerr << "PIR event buffer reset\n";
                pending.clear();
                continue;
            }

            std::size_t newline;

            while ((newline = pending.find('\n'))
                   != std::string::npos) {
                const std::string event =
                    pending.substr(0, newline);

                pending.erase(0, newline + 1);

                if (event.find("MOTION_DETECTED")
                    != std::string::npos) {
                    handle_motion(tft);
                }
            }
        }

        close(fd);

        if (running) {
            std::cerr << "PIR socket disconnected; reconnecting\n";
            std::this_thread::sleep_for(
                std::chrono::seconds(1));
        }
    }

    tft.show({"SYSTEM STOP"});
    std::cout << "attendance-app stopped\n";

    return 0;
}
