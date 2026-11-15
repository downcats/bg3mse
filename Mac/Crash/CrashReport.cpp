#include <Crash/CrashReport.h>

#include <Platform/FaultCapture.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace bg3se::mac::crash {

void FormatReportTime(int64_t time, long utcOffset, char out[kReportTimeLength + 1])
{
    int64_t local = time + utcOffset;
    int64_t days = local >= 0 ? local / 86400 : -((86399 - local) / 86400);
    int64_t second = local - days * 86400;

    // The civil date of a day number (days since 1970-01-01), in the
    // proleptic Gregorian calendar: eras of 400 years, counted from March.
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t dayOfEra = z - era * 146097;
    int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    int64_t monthFromMarch = (5 * dayOfYear + 2) / 153;
    int64_t day = dayOfYear - (153 * monthFromMarch + 2) / 5 + 1;
    int64_t month = monthFromMarch < 10 ? monthFromMarch + 3 : monthFromMarch - 9;
    int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
    // Four digits: a year outside them is shown as its last four.
    year = ((year % 10000) + 10000) % 10000;

    auto digits = [](char* at, int64_t value, int count) {
        for (int i = count - 1; i >= 0; i--) {
            at[i] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
    };
    digits(out, year, 4);
    out[4] = '-';
    digits(out + 5, month, 2);
    out[7] = '-';
    digits(out + 8, day, 2);
    out[10] = '_';
    digits(out + 11, second / 3600, 2);
    out[13] = '-';
    digits(out + 14, second / 60 % 60, 2);
    out[16] = '-';
    digits(out + 17, second % 60, 2);
    out[19] = '\0';
}

void PruneReports(char const* directory, unsigned keep)
{
    // A report's time, from the name of either of its files.
    auto timeOf = [](std::string_view name) -> std::string_view {
        std::string_view prefix = kReportPrefix;
        if (!name.starts_with(prefix)) return {};
        name.remove_prefix(prefix.size());
        if (name.size() <= kReportTimeLength) return {};
        std::string_view suffix = name.substr(kReportTimeLength);
        if (suffix != kReportSuffix && suffix != kBacktraceSuffix && suffix != kStackSuffix) return {};
        return name.substr(0, kReportTimeLength);
    };

    std::vector<std::string> times;
    DIR* dir = opendir(directory);
    if (!dir) return;
    while (dirent* entry = readdir(dir)) {
        auto time = timeOf(entry->d_name);
        if (!time.empty() && std::find(times.begin(), times.end(), time) == times.end()) times.emplace_back(time);
    }
    closedir(dir);

    // The time in the name sorts as time does.
    std::sort(times.begin(), times.end());
    for (size_t i = 0; i + keep < times.size(); i++) {
        for (char const* suffix : { kReportSuffix, kBacktraceSuffix, kStackSuffix }) {
            unlink((std::string(directory) + "/" + kReportPrefix + times[i] + suffix).c_str());
        }
    }
}

void TextBuffer::Clear()
{
    size_ = 0;
    truncated_ = false;
}

void TextBuffer::Append(std::string_view text)
{
    size_t room = capacity_ - size_;
    size_t n = text.size();
    if (n > room) {
        n = room;
        truncated_ = true;
    }
    memcpy(data_ + size_, text.data(), n);
    size_ += n;
}

void TextBuffer::Appendf(char const* fmt, ...)
{
    size_t room = capacity_ - size_;
    va_list args;
    va_start(args, fmt);
    // vsnprintf ends what it writes with a zero, which is not part of the text.
    int n = vsnprintf(data_ + size_, room, fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((size_t)n >= room) {
        truncated_ = true;
        size_ += room ? room - 1 : 0;
    } else {
        size_ += (size_t)n;
    }
}

bool ReportWriter::Open(char const* path)
{
    failed_ = false;
    // The directory may be one others can write to (/tmp): the name must not
    // lead through a link somebody else put there, and the report is the user's.
    do {
        fd_ = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (fd_ < 0 && errno == EINTR);
    return fd_ >= 0;
}

void ReportWriter::Write(std::string_view bytes)
{
    if (fd_ < 0 || failed_) return;
    char const* p = bytes.data();
    size_t left = bytes.size();
    while (left > 0) {
        ssize_t n = write(fd_, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            failed_ = true;
            return;
        }
        p += n;
        left -= (size_t)n;
    }
}

void ReportWriter::WriteAt(uint64_t offset, std::string_view bytes)
{
    if (fd_ < 0 || failed_) return;
    char const* p = bytes.data();
    size_t left = bytes.size();
    while (left > 0) {
        ssize_t n = pwrite(fd_, p, left, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            failed_ = true;
            return;
        }
        p += n;
        left -= (size_t)n;
        offset += (uint64_t)n;
    }
}

void ReportWriter::AddHeader()
{
    uint32_t signature = tag::Signature;
    Write({ reinterpret_cast<char const*>(&signature), sizeof(signature) });
    uint32_t version = kReportVersion;
    AddSection(tag::Header, { reinterpret_cast<char const*>(&version), sizeof(version) });
}

void ReportWriter::AddSection(uint32_t tag, std::string_view body)
{
    uint32_t head[2] = { tag, (uint32_t)body.size() };
    Write({ reinterpret_cast<char const*>(head), sizeof(head) });
    Write(body);
}

void ReportWriter::BeginSection(uint32_t tag)
{
    section_ = -1;
    if (fd_ < 0 || failed_) return;
    off_t at = lseek(fd_, 0, SEEK_CUR);
    if (at < 0) {
        failed_ = true;
        return;
    }
    section_ = at;
    AddSection(tag, {});
}

void ReportWriter::EndSection()
{
    if (section_ < 0 || fd_ < 0 || failed_) return;
    off_t end = lseek(fd_, 0, SEEK_CUR);
    uint64_t length = end < 0 ? 0 : (uint64_t)end - (uint64_t)section_ - 8;
    if (end < 0 || length > UINT32_MAX) {
        failed_ = true;
        return;
    }
    uint32_t size = (uint32_t)length;
    WriteAt((uint64_t)section_ + 4, { reinterpret_cast<char const*>(&size), sizeof(size) });
    section_ = -1;
}

bool ReportWriter::Close()
{
    if (fd_ < 0) return false;
    if (close(fd_) != 0 && errno != EINTR) failed_ = true;
    fd_ = -1;
    return !failed_;
}

bool WriteStackFile(ReportWriter& file, FaultRecord const& fault, StackFileHeader& header)
{
    header = {};
    header.Signature = kStackSignature;
    header.Version = kStackVersion;
    header.HeaderSize = sizeof(StackFileHeader);
    header.ThreadId = fault.ThreadId;
    header.ThreadPort = fault.Thread;
    header.StackLow = fault.StackLow;
    header.StackHigh = fault.StackHigh;
    uint64_t sp = arm_thread_state64_get_sp(fault.Registers);
    header.StackPointer = sp;

    uint64_t start = (sp > kStackRedZone ? sp - kStackRedZone : 0) & ~uint64_t{ 15 };
    // The bounds are the stack's when the stack pointer is in them, or just
    // below them in its guard: a thread whose stack overflowed, the stack
    // pointer moved down by the frame that did not fit (as Platform/FaultCapture.cpp
    // allows for).
    constexpr uint64_t kBelowTheStack = 64 * 1024;
    uint64_t end;
    if (fault.StackLow < fault.StackHigh && sp < fault.StackHigh && sp + kBelowTheStack >= fault.StackLow) {
        header.Flags |= StackBoundsKnown;
        end = fault.StackHigh;
    } else {
        end = start + kStackLimit < start ? UINT64_MAX : start + kStackLimit;
    }
    if (end - start > kStackLimit) {
        end = start + kStackLimit;
        header.Flags |= StackCutAtLimit;
    }
    header.Base = start;

    // The header goes first and is written again once the memory is.
    file.Write({ reinterpret_cast<char const*>(&header), sizeof(header) });

    // A page at a time, or as much of one as the buffer holds: a checked read
    // fails as a whole when any of it cannot be read.
    alignas(16) uint8_t buffer[16 * 1024];
    uint64_t piece = vm_page_size && vm_page_size < sizeof(buffer) ? vm_page_size : sizeof(buffer);
    bool begun = false, skipped = false;
    for (uint64_t at = start; at < end;) {
        uint64_t next = (at / piece + 1) * piece;
        if (next > end || next < at) next = end;
        size_t size = (size_t)(next - at);
        if (ReadMemory(at, buffer, size)) {
            if (!begun) {
                begun = true;
                header.Base = at;
                if (skipped) header.Flags |= StackStartsLater;
            }
            file.Write({ reinterpret_cast<char const*>(buffer), size });
            header.Length += size;
        } else if (!begun) {
            skipped = true;
        } else {
            header.Flags |= StackEndsUnreadable;
            break;
        }
        at = next;
    }

    file.WriteAt(0, { reinterpret_cast<char const*>(&header), sizeof(header) });
    return header.Length > 0;
}

}
