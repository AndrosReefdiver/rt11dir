#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <string>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <cctype>

// ------------------------------
// Constants
// ------------------------------
static constexpr size_t   BLOCK_SIZE         = 512; // 256 words * 2 bytes
static constexpr uint32_t DIR_SEGMENT_BLOCKS = 2;   // 2 blocks per directory segment

// Status word bits
static constexpr uint16_t E_TENT = 0x0100;
static constexpr uint16_t E_MPTY = 0x0200;
static constexpr uint16_t E_PERM = 0x0400;
static constexpr uint16_t E_EOS  = 0x0800;
static constexpr uint16_t E_READ = 0x4000;
static constexpr uint16_t E_PRE  = 0x8000;

static const char RAD50_TABLE[40] = {
    ' ', 'A','B','C','D','E','F','G','H','I',
    'J','K','L','M','N','O','P','Q','R','S',
    'T','U','V','W','X','Y','Z',
    '$','.', '%','0','1','2','3','4','5','6','7','8','9'
};

// ------------------------------
// Types
// ------------------------------
struct Rt11Entry {
    std::string name;        // NAME.EXT (upper-case)
    uint16_t startBlock = 0;
    uint16_t lengthBlocks = 0;
    uint16_t status = 0;
    uint16_t dateWord = 0;

    bool tentative = false;
    bool empty     = false;
    bool permanent = false;
    bool eos       = false;

    uint16_t segNumber = 0; // 1-based logical segment number
    uint16_t wordIndex = 0; // index in words[] of status word
};

struct DirSegmentHeader {
    uint16_t totalSegments;
    uint16_t nextSegment;
    uint16_t highestInUse;
    uint16_t extraBytes;
    uint16_t dataStartBlock;
};

// ------------------------------
// Basic block I/O
// ------------------------------
std::vector<uint8_t> readBlock(std::istream& f, uint32_t block) {
    std::vector<uint8_t> buf(BLOCK_SIZE);
    f.seekg(static_cast<std::streamoff>(block) * BLOCK_SIZE, std::ios::beg);
    if (!f.good()) throw std::runtime_error("Failed to seek to block " + std::to_string(block));
    f.read(reinterpret_cast<char*>(buf.data()), BLOCK_SIZE);
    if (!f.good()) throw std::runtime_error("Failed to read block " + std::to_string(block));
    return buf;
}

void writeBlock(std::ostream& f, uint32_t block, const std::vector<uint8_t>& buf) {
    if (buf.size() != BLOCK_SIZE) throw std::runtime_error("writeBlock: buffer size mismatch");
    f.seekp(static_cast<std::streamoff>(block) * BLOCK_SIZE, std::ios::beg);
    if (!f.good()) throw std::runtime_error("Failed to seek (write) to block " + std::to_string(block));
    f.write(reinterpret_cast<const char*>(buf.data()), BLOCK_SIZE);
    if (!f.good()) throw std::runtime_error("Failed to write block " + std::to_string(block));
}

// ------------------------------
// RAD50 helpers
// ------------------------------
int rad50Index(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (int i = 0; i < 40; ++i) {
        if (RAD50_TABLE[i] == c) return i;
    }
    return 0;
}

uint16_t encodeRad50(const std::string& s3) {
    char c1 = (s3.size() > 0) ? s3[0] : ' ';
    char c2 = (s3.size() > 1) ? s3[1] : ' ';
    char c3 = (s3.size() > 2) ? s3[2] : ' ';
    int i1 = rad50Index(c1);
    int i2 = rad50Index(c2);
    int i3 = rad50Index(c3);
    return static_cast<uint16_t>(i1 * 1600 + i2 * 40 + i3);
}

std::string decodeRad50(uint16_t w) {
    char c1 = RAD50_TABLE[w / 1600];
    w %= 1600;
    char c2 = RAD50_TABLE[w / 40];
    char c3 = RAD50_TABLE[w % 40];

    std::string s;
    if (c1 != ' ') s.push_back(c1);
    if (c2 != ' ') s.push_back(c2);
    if (c3 != ' ') s.push_back(c3);
    return s;
}

std::string decodeFileName(uint16_t name1, uint16_t name2, uint16_t ext) {
    std::string base = decodeRad50(name1) + decodeRad50(name2);
    if (base.size() > 6) base = base.substr(0, 6);

    std::string extension = decodeRad50(ext);
    if (extension.size() > 3) extension = extension.substr(0, 3);

    if (extension.empty()) return base;
    return base + "." + extension;
}

void encodeFileName(const std::string& rtname,
                    uint16_t& name1,
                    uint16_t& name2,
                    uint16_t& ext)
{
    std::string base, extension;
    auto pos = rtname.find('.');
    if (pos == std::string::npos) {
        base = rtname;
        extension.clear();
    } else {
        base = rtname.substr(0, pos);
        extension = rtname.substr(pos + 1);
    }

    if (base.size() > 6) base = base.substr(0, 6);
    if (extension.size() > 3) extension = extension.substr(0, 3);

    while (base.size() < 6) base.push_back(' ');
    while (extension.size() < 3) extension.push_back(' ');

    std::string b1 = base.substr(0, 3);
    std::string b2 = base.substr(3, 3);
    name1 = encodeRad50(b1);
    name2 = encodeRad50(b2);
    ext   = encodeRad50(extension);
}

// ------------------------------
// RT-11 filename normalization (for /copyto)
// ------------------------------
std::string normalizeRt11Name(const std::string& name) {
    auto pos = name.find('.');
    std::string base = (pos == std::string::npos) ? name : name.substr(0, pos);
    std::string ext  = (pos == std::string::npos) ? ""   : name.substr(pos + 1);

    if (base.empty()) throw std::runtime_error("RT-11 filename must have a name");

    std::string b = base;
    std::string e = ext;
    std::transform(b.begin(), b.end(), b.begin(),
                   [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c){ return static_cast<char>(std::toupper(c)); });

    if (b.size() > 6) b = b.substr(0, 6);
    if (e.size() > 3) e = e.substr(0, 3);

    if (e.empty()) return b;
    return b + "." + e;
}

// ------------------------------
// RT-11 pattern matching (for /copyfrom)
// ------------------------------
std::string normalizePattern(const std::string& pattern) {
    std::string p = pattern;
    std::transform(p.begin(), p.end(), p.begin(),
                   [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
    return p;
}

bool hasWildcard(const std::string& s) {
    return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

bool matchComponent(const std::string& value, const std::string& pattern) {
    if (pattern == "*" || pattern.empty()) return true;

    auto starPos = pattern.find('*');
    if (starPos == std::string::npos) {
        if (value.size() != pattern.size()) return false;
        for (size_t i = 0; i < pattern.size(); ++i) {
            if (pattern[i] == '?') continue;
            if (pattern[i] != value[i]) return false;
        }
        return true;
    }

    std::string prefix = pattern.substr(0, starPos);
    std::string suffix = pattern.substr(starPos + 1);

    if (value.size() < prefix.size() + suffix.size()) return false;
    if (value.compare(0, prefix.size(), prefix) != 0) return false;
    if (!suffix.empty() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    return true;
}

bool matchRt11Pattern(const std::string& rtName, const std::string& pattern) {
    std::string valueName, valueExt;
    auto vpos = rtName.find('.');
    if (vpos == std::string::npos) {
        valueName = rtName;
        valueExt.clear();
    } else {
        valueName = rtName.substr(0, vpos);
        valueExt  = rtName.substr(vpos + 1);
    }

    std::string patName, patExt;
    auto ppos = pattern.find('.');
    if (ppos == std::string::npos) {
        patName = pattern;
        patExt.clear();
    } else {
        patName = pattern.substr(0, ppos);
        patExt  = pattern.substr(ppos + 1);
    }

    return matchComponent(valueName, patName) &&
           matchComponent(valueExt,  patExt);
}

// ------------------------------
// RT-11 date encode/decode
// ------------------------------
std::string formatRt11Date(uint16_t dateWord) {
    if (dateWord == 0) return "        ";

    uint16_t age   = (dateWord >> 14) & 0x3;
    uint16_t month = (dateWord >> 10) & 0xF;
    uint16_t day   = (dateWord >> 5)  & 0x1F;
    uint16_t yl    = dateWord & 0x1F;

    int year = 1972 + yl + 32 * age;

    static const char* monthNames[13] = {
        "", "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };

    if (month < 1 || month > 12 || day < 1 || day > 31) return "        ";

    std::ostringstream oss;
    int year2 = year % 100;
    oss << std::setw(2) << std::setfill('0') << day << "-"
        << monthNames[month] << "-"
        << std::setw(2) << std::setfill('0') << year2;
    return oss.str();
}

uint16_t encodeRt11DateFromSystem() {
    std::time_t t = std::time(nullptr);
    std::tm tmLocal{};
#ifdef _WIN32
    localtime_s(&tmLocal, &t);
#else
    tmLocal = *std::localtime(&t);
#endif

    int year  = tmLocal.tm_year + 1900;
    int month = tmLocal.tm_mon + 1;
    int day   = tmLocal.tm_mday;

    if (year < 1972) return 0;

    int yearOffset = year - 1972;
    int age = yearOffset / 32;
    if (age > 3) age = 3;
    int base = 1972 + age * 32;
    int yearLow = year - base;
    if (yearLow < 0) yearLow = 0;
    if (month < 1 || month > 12 || day < 1 || day > 31) return 0;

    uint16_t dw = 0;
    dw |= static_cast<uint16_t>((age & 0x3)   << 14);
    dw |= static_cast<uint16_t>((month & 0xF) << 10);
    dw |= static_cast<uint16_t>((day & 0x1F)  << 5);
    dw |= static_cast<uint16_t>(yearLow & 0x1F);
    return dw;
}

// Parse date string in format dd-MMM-yy (e.g., "15-JAN-97")
// Returns true if successful, false otherwise
bool parseDateString(const std::string& dateStr, int& day, int& month, int& year) {
    static const char* monthNames[12] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    
    // Expected format: dd-MMM-yy (e.g., "15-JAN-97" or "01-FEB-99")
    if (dateStr.size() != 9) return false;
    if (dateStr[2] != '-' || dateStr[6] != '-') return false;
    
    // Parse day
    try {
        day = std::stoi(dateStr.substr(0, 2));
        if (day < 1 || day > 31) return false;
    } catch (...) {
        return false;
    }
    
    // Parse month
    std::string monthStr = dateStr.substr(3, 3);
    std::transform(monthStr.begin(), monthStr.end(), monthStr.begin(),
                   [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
    
    month = 0;
    for (int i = 0; i < 12; ++i) {
        if (monthStr == monthNames[i]) {
            month = i + 1;  // 1-based month
            break;
        }
    }
    if (month == 0) return false;
    
    // Parse year (2-digit)
    try {
        int year2 = std::stoi(dateStr.substr(7, 2));
        
        // Interpret 2-digit year: 72-99 -> 1972-1999, 00-71 -> 2000-2071
        if (year2 >= 72) {
            year = 1900 + year2;
        } else {
            year = 2000 + year2;
        }
        
        // RT-11 date format supports 1972-2099
        if (year < 1972 || year > 2099) return false;
    } catch (...) {
        return false;
    }
    
    return true;
}

// Encode specific date to RT-11 format
uint16_t encodeRt11Date(int year, int month, int day) {
    if (year < 1972 || year > 2099) return 0;
    if (month < 1 || month > 12 || day < 1 || day > 31) return 0;

    int yearOffset = year - 1972;
    int age = yearOffset / 32;
    if (age > 3) age = 3;
    int base = 1972 + age * 32;
    int yearLow = year - base;
    if (yearLow < 0) yearLow = 0;

    uint16_t dw = 0;
    dw |= static_cast<uint16_t>((age & 0x3)   << 14);
    dw |= static_cast<uint16_t>((month & 0xF) << 10);
    dw |= static_cast<uint16_t>((day && 0x1F)  << 5);
    dw |= static_cast<uint16_t>(yearLow & 0x1F);
    return dw;
}

// ------------------------------
// Home block / first dir block
// ------------------------------
uint32_t getFirstDirectoryBlock(std::istream& f) {
    auto buf = readBlock(f, 1); // home block
    uint16_t words[256];
    for (int i = 0; i < 256; ++i) {
        uint16_t lo = buf[2*i];
        uint16_t hi = buf[2*i+1];
        words[i] = static_cast<uint16_t>(lo | (hi << 8));
    }

    // Octal 724 = decimal 468 bytes = word 234
    const int wordIndex = 234;
    uint16_t firstDirBlock = words[wordIndex];
    if (firstDirBlock == 0) return 6;
    return firstDirBlock;
}

void checkBadBlockTable(const std::string& imagePath) {
    std::ifstream f(imagePath, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open disk image");
    
    auto buf = readBlock(f, 1); // home block
    uint16_t words[256];
    for (int i = 0; i < 256; ++i) {
        uint16_t lo = buf[2*i];
        uint16_t hi = buf[2*i+1];
        words[i] = static_cast<uint16_t>(lo | (hi << 8));
    }
    
    std::cout << "\n=== BAD BLOCK TABLE ===" << std::endl;
    std::cout << "Home block bad block table (starts at word 16 / octal byte 040):\n";
    
    // Bad block table starts at octal 040 (decimal 32 bytes = word 16)
    // Each entry is 2 words: block number, count
    bool foundBad = false;
    for (int i = 16; i < 16 + 65; i += 2) {
        uint16_t blockNum = words[i];
        uint16_t count = words[i + 1];
        
        if (blockNum == 0 && count == 0) {
            // End of bad block table
            break;
        }
        
        if (blockNum != 0 || count != 0) {
            std::cout << "  Entry " << ((i - 16) / 2) << ": Block " << blockNum 
                      << ", Count " << count << std::endl;
            foundBad = true;
        }
    }
    
    if (!foundBad) {
        std::cout << "  (No bad blocks registered)\n";
    }
    
    std::cout << "\nOther home block info:\n";
    // Octal 724 = decimal 468 bytes = word 234
    std::cout << "  First directory block (word 234 / octal byte 724): " << words[234] << "\n";
    // Octal 722 = decimal 466 bytes = word 233
    std::cout << "  Pack cluster size (word 233 / octal byte 722): " << words[233] << "\n";
    // Octal 726 = decimal 470 bytes = word 235
    std::cout << "  System version (word 235 / octal byte 726): " << std::hex << words[235] << std::dec << "\n";
}

// ------------------------------
// Directory parsing
// ------------------------------
DirSegmentHeader parseSegmentHeader(const uint16_t* words) {
    DirSegmentHeader h{};
    h.totalSegments   = words[0];
    h.nextSegment     = words[1];
    h.highestInUse    = words[2];
    h.extraBytes      = words[3];
    h.dataStartBlock  = words[4];
    return h;
}

void readDirectory(std::istream& f,
                   uint32_t totalBlocks,
                   std::vector<Rt11Entry>& entries)
{
    entries.clear();

    uint32_t firstDirBlock = getFirstDirectoryBlock(f);
    if (firstDirBlock >= totalBlocks) {
        throw std::runtime_error("First directory block out of range");
    }

    // Read the first segment to get total segments count
    auto segBuf0 = readBlock(f, firstDirBlock);
    auto segBuf1 = readBlock(f, firstDirBlock + 1);

    uint16_t segWords[512];
    for (int i = 0; i < 256; ++i)
        segWords[i] = segBuf0[2*i] | (segBuf0[2*i+1] << 8);
    for (int i = 0; i < 256; ++i)
        segWords[256+i] = segBuf1[2*i] | (segBuf1[2*i+1] << 8);

    DirSegmentHeader firstHeader = parseSegmentHeader(segWords);
    uint16_t totalSegments = firstHeader.totalSegments;
    if (totalSegments == 0 || totalSegments > 31) totalSegments = 1;

    // Get the data start block from the first segment - this applies to ALL segments
    uint32_t dataStartBlock = firstHeader.dataStartBlock;

    // Follow the linked list of segments starting from segment 1
    uint16_t currentSegNum = 1;  // Start with logical segment 1
    std::vector<bool> visitedSegments(totalSegments + 1, false);
    
    // Track cumulative offset ACROSS ALL SEGMENTS
    uint32_t globalCumulativeOffset = 0;

    while (currentSegNum != 0) {
        // Prevent infinite loops
        if (currentSegNum < 1 || currentSegNum > totalSegments) {
            std::cerr << "Warning: Invalid segment number " << currentSegNum 
                      << " in directory chain\n";
            break;
        }
        
        if (visitedSegments[currentSegNum]) {
            std::cerr << "Warning: Directory loop detected at segment " 
                      << currentSegNum << "\n";
            break;
        }
        visitedSegments[currentSegNum] = true;

        // Calculate physical block location for this segment
        uint32_t segBlock = firstDirBlock + (currentSegNum - 1) * DIR_SEGMENT_BLOCKS;
        if (segBlock + 1 >= totalBlocks) {
            std::cerr << "Warning: Segment " << currentSegNum 
                      << " is beyond volume bounds\n";
            break;
        }

        auto buf0 = readBlock(f, segBlock);
        auto buf1 = readBlock(f, segBlock + 1);

        uint16_t words[512];
        for (int i = 0; i < 256; ++i)
            words[i] = buf0[2*i] | (buf0[2*i+1] << 8);
        for (int i = 0; i < 256; ++i)
            words[256+i] = buf1[2*i] | (buf1[2*i+1] << 8);

        DirSegmentHeader hdr = parseSegmentHeader(words);
        uint16_t extraWordsSeg = hdr.extraBytes / 2;
        uint16_t entryWordsSeg = 7 + extraWordsSeg;

        uint16_t idx = 5;

        // Parse entries until we hit EOS
        while (idx + entryWordsSeg <= 512) {
            uint16_t status = words[idx + 0];
            
            // Check for end-of-segment marker (E_EOS bit set)
            if (status & E_EOS) {
                break;  // End of this segment
            }
            
            // Skip if status is 0 (shouldn't happen but be safe)
            if (status == 0) {
                break;
            }

            uint16_t name1 = words[idx + 1];
            uint16_t name2 = words[idx + 2];
            uint16_t ext   = words[idx + 3];
            uint16_t len   = words[idx + 4];
            uint16_t dateW = words[idx + 6];

            Rt11Entry e;
            e.segNumber    = currentSegNum;
            e.wordIndex    = idx;
            e.status       = status;
            e.lengthBlocks = len;
            e.dateWord     = dateW;
            e.name         = decodeFileName(name1, name2, ext);

            // Calculate start block using GLOBAL cumulative offset and dataStartBlock from first segment
            uint32_t start = dataStartBlock + globalCumulativeOffset;
            e.startBlock = static_cast<uint16_t>(start);

            e.tentative = (status & E_TENT) != 0;
            e.empty     = (status & E_MPTY) != 0;
            e.permanent = (status & E_PERM) != 0;
            e.eos       = (status & E_EOS)  != 0;

            entries.push_back(e);

            // Increment GLOBAL cumulative offset for next entry
            globalCumulativeOffset += len;
            idx = static_cast<uint16_t>(idx + entryWordsSeg);
        }

        // Follow the link to the next logical segment
        currentSegNum = hdr.nextSegment;
    }
}

// ------------------------------
// Directory listing
// ------------------------------
void showDirectory(const std::string& imagePath, bool brief, bool showEmpty) {
    std::ifstream f(imagePath, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open disk image");

    auto size = f.tellg();
    if (size <= 0) throw std::runtime_error("Disk image is empty or invalid size");
    uint32_t totalBlocks = static_cast<uint32_t>(size / BLOCK_SIZE);
    f.seekg(0, std::ios::beg);

    std::vector<Rt11Entry> entries;
    readDirectory(f, totalBlocks, entries);

    std::cout << "Directory of " << imagePath << "\n\n";

    uint32_t totalUsed = 0;
    uint32_t totalFree = 0;
    uint32_t fileCount = 0;

    for (const auto& e : entries) {
        if (e.permanent) {
            totalUsed += e.lengthBlocks;
            fileCount++;
        }
        if (e.empty)     totalFree += e.lengthBlocks;

        if (e.empty && !showEmpty) continue;
        if (!e.permanent && !e.empty) continue;

        if (brief) {
            if (e.empty) std::cout << "<EMPTY>\n";
            else         std::cout << e.name << "\n";
            continue;
        }

        if (e.empty) {
            std::cout << std::left << std::setw(12) << "<EMPTY>"
                      << " len="   << std::setw(6) << e.lengthBlocks
                      << " start=" << std::setw(6) << e.startBlock
                      << "\n";
        } else {
            std::string dateStr = formatRt11Date(e.dateWord);
            std::cout << std::left << std::setw(12) << e.name
                      << " len="   << std::setw(6) << e.lengthBlocks
                      << " start=" << std::setw(6) << e.startBlock
                      << " "      << dateStr
                      << "\n";
        }
    }

    std::cout << "\n"
              << "Files: " << fileCount << "\n"
              << "Total used blocks: " << totalUsed << "\n"
              << "Total free blocks: " << totalFree << "\n";
}

// ------------------------------
// Case-insensitive string equality
// ------------------------------
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::toupper(ca) != std::toupper(cb)) return false;
    }
    return true;
}

// ------------------------------
// Copy FROM RT-11 -> Windows
// ------------------------------
void copySingleFromRt11(std::ifstream& f,
                        uint32_t totalBlocks,
                        const Rt11Entry& e,
                        const std::filesystem::path& outPath,
                        bool noReplace)
{
    if (!e.permanent) {
        throw std::runtime_error("Cannot copy non-permanent file: " + e.name);
    }

    uint32_t endBlock = static_cast<uint32_t>(e.startBlock) + e.lengthBlocks - 1;
    if (e.startBlock == 0 || endBlock >= totalBlocks) {
        throw std::runtime_error("RT-11 entry has invalid range; cannot copy " + e.name);
    }

    if (noReplace && std::filesystem::exists(outPath)) {
        std::cout << "Skipping " << outPath.string()
                  << " — already exists (noreplace)\n";
        return;
    }

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot create output file: " + outPath.string());

    for (uint16_t i = 0; i < e.lengthBlocks; ++i) {
        auto block = readBlock(f, static_cast<uint32_t>(e.startBlock) + i);
        out.write(reinterpret_cast<char*>(block.data()), BLOCK_SIZE);
        if (!out.good()) throw std::runtime_error("Failed writing to output file: " + outPath.string());
    }

    std::cout << "Copied " << e.name << " -> " << outPath.string() << "\n";
}

void copyFromRt11(const std::string& imagePath,
                  const std::string& patternRaw,
                  const std::string& toPathRaw,
                  bool noReplace)
{
    std::ifstream f(imagePath, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open disk image");

    auto size = f.tellg();
    if (size <= 0) throw std::runtime_error("Disk image is empty or invalid size");
    uint32_t totalBlocks = static_cast<uint32_t>(size / BLOCK_SIZE);
    f.seekg(0, std::ios::beg);

    std::vector<Rt11Entry> entries;
    readDirectory(f, totalBlocks, entries);

    std::string pattern = patternRaw.empty() ? "*.*" : normalizePattern(patternRaw);

    std::filesystem::path destDir;
    if (toPathRaw.empty()) {
        destDir = std::filesystem::current_path();
    } else {
        std::string p = toPathRaw;
        if (hasWildcard(p)) {
            auto pos = p.find_last_of("\\/");
            if (pos != std::string::npos) p = p.substr(0, pos);
        }
        if (p.empty()) destDir = std::filesystem::current_path();
        else          destDir = p;
    }

    std::vector<const Rt11Entry*> matches;
    for (const auto& e : entries) {
        if (e.permanent && matchRt11Pattern(e.name, pattern)) {
            matches.push_back(&e);
        }
    }

    if (matches.empty()) {
        throw std::runtime_error("No RT-11 files matched pattern: " + patternRaw);
    }

    for (const auto* e : matches) {
        std::filesystem::path outPath = destDir / e->name;
        copySingleFromRt11(f, totalBlocks, *e, outPath, noReplace);
    }
}

// ------------------------------
// Windows wildcard matching and expansion (for /copyto)
// ------------------------------
bool matchFsName(const std::string& name, const std::string& pattern) {
    size_t n = 0, p = 0, star = std::string::npos, match = 0;

    while (n < name.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' ||
             std::toupper(static_cast<unsigned char>(pattern[p])) ==
             std::toupper(static_cast<unsigned char>(name[n])))) {
            ++n;
            ++p;
        }
        else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = n;
        }
        else if (star != std::string::npos) {
            p = star + 1;
            n = ++match;
        }
        else {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool hasFsWildcard(const std::string& s) {
    return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

std::vector<std::filesystem::path> expandWindowsWildcard(const std::string& pattern) {
    std::filesystem::path p(pattern);
    std::filesystem::path dir = p.parent_path();
    std::string pat = p.filename().string();

    if (dir.empty()) {
        dir = std::filesystem::current_path();
    }

    std::vector<std::filesystem::path> result;

    for (auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto name = entry.path().filename().string();
        if (matchFsName(name, pat)) {
            result.push_back(entry.path());
        }
    }

    return result;
}

// ------------------------------
void splitDirectorySegment(const std::string& imagePath, uint16_t segToSplit)
{
    std::fstream f(imagePath, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) throw std::runtime_error("Cannot open disk image for directory split");

    // 1) Read segment 1 header
    uint32_t firstDirBlock = getFirstDirectoryBlock(f);

    auto seg1b0 = readBlock(f, firstDirBlock);
    auto seg1b1 = readBlock(f, firstDirBlock + 1);

    uint16_t seg1Words[512];
    for (int i = 0; i < 256; ++i)
        seg1Words[i] = seg1b0[2*i] | (seg1b0[2*i+1] << 8);
    for (int i = 0; i < 256; ++i)
        seg1Words[256+i] = seg1b1[2*i] | (seg1b1[2*i+1] << 8);

    DirSegmentHeader seg1Hdr = parseSegmentHeader(seg1Words);

    uint16_t totalSegments = seg1Hdr.totalSegments;   // word 0
    if (totalSegments == 0 || totalSegments > 31)
        throw std::runtime_error("Invalid totalSegments in directory header");

    // 2) Build set of segments that are currently linked/in use
    std::vector<bool> used(totalSegments + 1, false); // 1..totalSegments

    // Follow the link chain starting at segment 1
    uint16_t currentSeg = 1;
    std::vector<uint16_t> chain;
    while (currentSeg != 0 && currentSeg >= 1 && currentSeg <= totalSegments) {
        if (used[currentSeg]) {
            // Loop in directory links – corrupt disk
            throw std::runtime_error("Directory link loop detected while splitting");
        }
        used[currentSeg] = true;
        chain.push_back(currentSeg);

        uint32_t segBlock = firstDirBlock + (currentSeg - 1) * DIR_SEGMENT_BLOCKS;
        auto b0 = readBlock(f, segBlock);
        auto b1 = readBlock(f, segBlock + 1);

        uint16_t w[512];
        for (int i = 0; i < 256; ++i)
            w[i] = b0[2*i] | (b0[2*i+1] << 8);
        for (int i = 0; i < 256; ++i)
            w[256+i] = b1[2*i] | (b1[2*i+1] << 8);

        uint16_t nextSeg = w[1]; // header word 1 = link to next logical segment
        currentSeg = nextSeg;
    }

    // At this point, "used[s]" = true iff segment "s" is in the directory chain.
    // We are going to split segToSplit, so it must be in the chain:
    if (segToSplit < 1 || segToSplit > totalSegments || !used[segToSplit]) {
        throw std::runtime_error("Attempted to split a segment not in the directory chain");
    }

    // 3) Find an unused segment number we can use as the new segment
    uint16_t newSegNum = 0;
    for (uint16_t s = 1; s <= totalSegments; ++s) {
        if (!used[s]) {
            newSegNum = s;
            break;
        }
    }
    if (newSegNum == 0) {
        // All segments 1..totalSegments are already in the chain; no place to split
        throw std::runtime_error("Directory full: no more segments available to split into");
    }

    // 4) Read the segment we are splitting
    uint32_t segBlock = firstDirBlock + (segToSplit - 1) * DIR_SEGMENT_BLOCKS;
    auto sb0 = readBlock(f, segBlock);
    auto sb1 = readBlock(f, segBlock + 1);

    uint16_t words[512];
    for (int i = 0; i < 256; ++i)
        words[i] = sb0[2*i] | (sb0[2*i+1] << 8);
    for (int i = 0; i < 256; ++i)
        words[256+i] = sb1[2*i] | (sb1[2*i+1] << 8);

    DirSegmentHeader hdr = parseSegmentHeader(words);
    uint16_t extraWords  = hdr.extraBytes / 2;
    uint16_t entryWords  = 7 + extraWords;

    // 5) Collect indices of all directory entries in this segment (excluding EOS)
    std::vector<uint16_t> entryIdx;
    uint16_t idx = 5;
    while (idx + entryWords/*Seg*/ <= 512) {
        uint16_t st = words[idx + 0];
        
        // Stop at EOS marker (E_EOS bit set)
        if (st & E_EOS) {
            break;
        }
        
        // Skip if status is 0 (shouldn't happen but be defensive)
        if (st == 0) {
            break;
        }
        
        entryIdx.push_back(idx);
        idx = static_cast<uint16_t>(idx + entryWords/*Seg*/);
    }
    
    if (entryIdx.empty()) {
        throw std::runtime_error("Attempted to split an empty directory segment");
    }

    // 6) Choose a permanent/tentative entry near the middle as the split point
    // We want to split on a file entry, not an empty entry if possible
    std::vector<size_t> movablePositions;
    for (size_t i = 0; i < entryIdx.size(); ++i) {
        uint16_t st = words[entryIdx[i] + 0];
        // Only split at permanent or tentative files, not empty areas
        if (st & (E_PERM | E_TENT)) {
            movablePositions.push_back(i);
        }
    }
    
    size_t midPos;
    if (!movablePositions.empty()) {
        // Split at the middle of file entries
        midPos = movablePositions[movablePositions.size() / 2];
    } else {
        // No file entries, just split in the middle
        midPos = entryIdx.size() / 2;
    }

    // Ensure we're not splitting at the last entry (need at least one entry in new segment)
    if (midPos >= entryIdx.size() - 1) {
        midPos = entryIdx.size() / 2;
    }

    uint16_t middleIdx      = entryIdx[midPos];
    uint16_t originalStatus = words[middleIdx + 0];
    uint16_t originalLink   = words[1]; // old link to "next segment" from this header

    // 7) Build the "old" segment with EOS at middle and link -> newSegNum
    uint16_t oldSegWords[512];
    std::copy(std::begin(words), std::end(words), std::begin(oldSegWords));

    oldSegWords[middleIdx + 0] = E_EOS;      // EOS at split point
    oldSegWords[1]             = newSegNum;  // link current segment to new segment

    // Write modified old segment back to disk
    for (int i = 0; i < 256; ++i) {
        sb0[2*i]     = static_cast<uint8_t>(oldSegWords[i] & 0x00FF);
        sb0[2*i + 1] = static_cast<uint8_t>((oldSegWords[i] >> 8) & 0x00FF);
    }
    for (int i = 0; i < 256; ++i) {
        sb1[2*i]     = static_cast<uint8_t>(oldSegWords[256+i] & 0x00FF);
        sb1[2*i + 1] = static_cast<uint8_t>((oldSegWords[256+i] >> 8) & 0x00FF);
    }

    writeBlock(f, segBlock,     sb0);
    writeBlock(f, segBlock + 1, sb1);

    // 8) Build the new segment in memory: copy header, restore original link,
    //    move entries from middle..end to the top
    uint16_t newWords[512] = {0};

    // Header: copy from original segment header (words[0..4]),
    // but word 1 (link) should be the original link
    newWords[0] = words[0];          // total segments
    newWords[1] = originalLink;      // link restored (to next segment after this one)
    newWords[2] = words[2];          // "highestInUse" is tracked only in seg 1
    newWords[3] = words[3];          // extra bytes
    newWords[4] = words[4];          // dataStartBlock (same for all segments)

    // Restore the middle entry's status in the original words array so we can copy it
    words[middleIdx + 0] = originalStatus;

    size_t entriesMoved = 0;
    for (size_t i = midPos; i < entryIdx.size(); ++i) {
        uint16_t srcIdxEntry  = entryIdx[i];
        uint16_t destIdxEntry = static_cast<uint16_t>(5 + entriesMoved * entryWords);
        if (destIdxEntry + entryWords > 512) {
            break;
        }

        for (uint16_t w = 0; w < entryWords; ++w) {
            newWords[destIdxEntry + w] = words[srcIdxEntry + w];
        }
        ++entriesMoved;
    }

    // Place EOS in the new segment at first free slot
    uint16_t newEosIdx = static_cast<uint16_t>(5 + entriesMoved * entryWords);
    if (newEosIdx < 512) {
        newWords[newEosIdx + 0] = E_EOS;
    }

    // 9) Write the new segment to its physical location
    uint32_t newSegBlock = firstDirBlock + (newSegNum - 1) * DIR_SEGMENT_BLOCKS;
    auto nb0 = readBlock(f, newSegBlock);
    auto nb1 = readBlock(f, newSegBlock + 1);

    for (int i = 0; i < 256; ++i) {
        nb0[2*i]     = static_cast<uint8_t>(newWords[i] & 0x00FF);
        nb0[2*i + 1] = static_cast<uint8_t>((newWords[i] >> 8) & 0x00FF);
    }
    for (int i = 0; i < 256; ++i) {
        nb1[2*i]     = static_cast<uint8_t>(newWords[256+i] & 0x00FF);
        nb1[2*i + 1] = static_cast<uint8_t>((newWords[256+i] >> 8) & 0x00FF);
    }

    writeBlock(f, newSegBlock,     nb0);
    writeBlock(f, newSegBlock + 1, nb1);

    // 10) Update "highest segment in use" in segment 1 header (word 2)
    // RT-11 ignores this in other segments.
    uint16_t oldHighest = seg1Hdr.highestInUse;
    uint16_t newHighest = std::max<uint16_t>(oldHighest, newSegNum);
    
    if (newHighest != oldHighest) {
        seg1Hdr.highestInUse = newHighest;
        seg1Words[2]         = newHighest;

        for (int i = 0; i < 256; ++i) {
            seg1b0[2*i]     = static_cast<uint8_t>(seg1Words[i] & 0x00FF);
            seg1b0[2*i + 1] = static_cast<uint8_t>((seg1Words[i] >> 8) & 0x00FF);
        }
        for (int i = 0; i < 256; ++i) {
            seg1b1[2*i]     = static_cast<uint8_t>(seg1Words[256+i] & 0x00FF);
            seg1b1[2*i + 1] = static_cast<uint8_t>((seg1Words[256+i] >> 8) & 0x00FF);
        }

        writeBlock(f, firstDirBlock,     seg1b0);
        writeBlock(f, firstDirBlock + 1, seg1b1);
    }

    f.close();
}

// ------------------------------
// Copy TO RT-11 (Windows -> RT-11)
// ------------------------------
void copySingleToRt11(const std::string& imagePath,
                      const std::filesystem::path& srcPath,
                      bool noReplace,
                      uint16_t optionalDateWord = 0)
{
    if (!std::filesystem::exists(srcPath)) {
        throw std::runtime_error("Source file does not exist: " + srcPath.string());
    }

    std::string baseName = srcPath.filename().string();
    std::string rtname = normalizeRt11Name(baseName);


    std::ifstream fin(imagePath, std::ios::binary | std::ios::ate);
    if (!fin) throw std::runtime_error("Cannot open disk image (read)");

    auto size = fin.tellg();
    if (size <= 0) throw std::runtime_error("Disk image is empty or invalid size");
    uint32_t totalBlocks = static_cast<uint32_t>(size / BLOCK_SIZE);
    fin.seekg(0, std::ios::beg);

    std::vector<Rt11Entry> entries;
    readDirectory(fin, totalBlocks, entries);
    fin.close();

    if (noReplace) {
        for (const auto& e : entries) {
            if (e.permanent && iequals(e.name, rtname)) {
                std::cout << "Skipping " << rtname
                          << " — already exists on RT-11 (noreplace)\n";
                return;
            }
        }
    }

    std::ifstream in(srcPath, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open input file: " + srcPath.string());
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    in.close();

    uint32_t blocksNeeded = static_cast<uint32_t>((data.size() + BLOCK_SIZE - 1) / BLOCK_SIZE);
    if (blocksNeeded == 0) blocksNeeded = 1;

    Rt11Entry emptyEntry{};
    bool found = false;
    for (const auto& e : entries) {
        if (e.empty && !e.permanent && !e.tentative && e.lengthBlocks >= blocksNeeded) {
            emptyEntry = e;
            found = true;
            break;
        }
    }
    if (!found) throw std::runtime_error("No empty area large enough found for allocation");

    uint32_t start = emptyEntry.startBlock;
    uint32_t endBlock = start + blocksNeeded - 1;
    if (start == 0 || endBlock >= totalBlocks) {
        throw std::runtime_error("Selected empty area has invalid range on disk");
    }

    std::fstream f(imagePath, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) throw std::runtime_error("Cannot open disk image (read/write)");

    // Write file data first
    uint32_t offset = 0;
    for (uint32_t i = 0; i < blocksNeeded; ++i) {
        std::vector<uint8_t> block(BLOCK_SIZE, 0);
        size_t remaining = data.size() - offset;
        size_t toCopy = (remaining > BLOCK_SIZE) ? BLOCK_SIZE : remaining;
        if (toCopy > 0) {
            std::memcpy(block.data(), data.data() + offset, toCopy);
            offset += toCopy;
        }
        writeBlock(f, start + i, block);
    }

    uint32_t firstDirBlock = getFirstDirectoryBlock(f);
    uint32_t segBlock = firstDirBlock + (emptyEntry.segNumber - 1) * DIR_SEGMENT_BLOCKS;

    auto buf0 = readBlock(f, segBlock);
    auto buf1 = readBlock(f, segBlock + 1);

    uint16_t words[512];
    for (int i = 0; i < 256; ++i)
        words[i] = buf0[2*i] | (buf0[2*i+1] << 8);
    for (int i = 0; i < 256; ++i)
        words[256+i] = buf1[2*i] | (buf1[2*i+1] << 8);

    DirSegmentHeader hdr = parseSegmentHeader(words);
    uint16_t extraWordsSeg = hdr.extraBytes / 2;
    uint16_t entryWordsSeg = 7 + extraWordsSeg;

    uint16_t idx = emptyEntry.wordIndex;
    uint16_t originalLen = words[idx + 4];

    if (originalLen < blocksNeeded) {
        throw std::runtime_error("Internal error: chosen empty area smaller than required");
    }

    uint16_t remaining = static_cast<uint16_t>(originalLen - blocksNeeded);

    // PRE-CHECK: Calculate how much space we need
    // We need space for: the new file entry (already exists as empty)
    // Plus potentially: new empty entry + new EOS
    // Find current EOS position
    uint16_t eosIdx = 0;
    uint16_t scanIdx = 5;
    while (scanIdx + entryWordsSeg <= 512) {
        uint16_t st = words[scanIdx + 0];
        if (st & E_EOS) {
            eosIdx = scanIdx;
            break;
        }
        if (st == 0) {
            eosIdx = scanIdx;
            break;
        }
        scanIdx = static_cast<uint16_t>(scanIdx + entryWordsSeg);
    }
    if (eosIdx == 0) eosIdx = scanIdx;

    // If we have remaining space, we'll need to insert a new empty entry
    // Check if there's room for: [new empty entry] + [EOS]
    if (remaining > 0) {
        uint16_t insertIdx = static_cast<uint16_t>(idx + entryWordsSeg);
        uint16_t spaceNeededAfterInsert = static_cast<uint16_t>(insertIdx + entryWordsSeg + entryWordsSeg);
        
        if (spaceNeededAfterInsert > 512) {
            f.close();
            splitDirectorySegment(imagePath, emptyEntry.segNumber);
            copySingleToRt11(imagePath, srcPath, noReplace, optionalDateWord);
            return;
        }
    }

    // Now proceed with the actual directory update
    uint16_t status = E_PERM;
    uint16_t name1, name2, ext;
    encodeFileName(rtname, name1, name2, ext);
    uint16_t len   = static_cast<uint16_t>(blocksNeeded);
    uint16_t jobCh = 0;
    
    // Use optional date if provided, otherwise use system date
    uint16_t dateW = (optionalDateWord != 0) ? optionalDateWord : encodeRt11DateFromSystem();

    words[idx + 0] = status;
    words[idx + 1] = name1;
    words[idx + 2] = name2;
    words[idx + 3] = ext;
    words[idx + 4] = len;
    words[idx + 5] = jobCh;
    words[idx + 6] = dateW;

    if (remaining > 0) {
        uint16_t insertIdx = static_cast<uint16_t>(idx + entryWordsSeg);

        // If insertIdx < eosIdx, we need to shift entries to make room
        if (insertIdx < eosIdx) {
            // Shift entries UP to make room for the new empty entry
            for (int i = eosIdx - 1; i >= static_cast<int>(insertIdx); --i) {
                words[i + entryWordsSeg] = words[i];
            }
        }

        // Insert the new empty entry at insertIdx
        words[insertIdx + 0] = E_MPTY;
        words[insertIdx + 1] = 0;
        words[insertIdx + 2] = 0;
        words[insertIdx + 3] = 0;
        words[insertIdx + 4] = remaining;
        words[insertIdx + 5] = 0;
        words[insertIdx + 6] = 0;
        
        // Place the EOS marker AFTER the new empty entry
        uint16_t newEosIdx = static_cast<uint16_t>(insertIdx + entryWordsSeg);
        
        // Clear all words for the new EOS entry
        for (uint16_t w = 0; w < entryWordsSeg; ++w) {
            words[newEosIdx + w] = 0;
        }
        words[newEosIdx] = E_EOS;
    }

    for (int i = 0; i < 256; ++i) {
        buf0[2*i]     = static_cast<uint8_t>(words[i] & 0x00FF);
        buf0[2*i + 1] = static_cast<uint8_t>((words[i] >> 8) & 0x00FF);
    }
    for (int i = 0; i < 256; ++i) {
        buf1[2*i]     = static_cast<uint8_t>(words[256+i] & 0x00FF);
        buf1[2*i + 1] = static_cast<uint8_t>((words[256+i] >> 8) & 0x00FF);
    }

    writeBlock(f, segBlock,     buf0);
    writeBlock(f, segBlock + 1, buf1);

    f.close();

    std::cout << "Copied " << srcPath.string() << " -> " << rtname
              << " on " << imagePath << "\n";
}

void copyToRt11(const std::string& imagePath,
                const std::string& fromPatternRaw,
                bool noReplace,
                uint16_t optionalDateWord = 0)
{
    if (fromPatternRaw.empty()) {
        throw std::runtime_error("/from requires a filename or wildcard");
    }

    std::vector<std::filesystem::path> srcFiles;

    if (hasFsWildcard(fromPatternRaw)) {
        srcFiles = expandWindowsWildcard(fromPatternRaw);
        if (srcFiles.empty()) {
            throw std::runtime_error("No Windows files matched pattern: " + fromPatternRaw);
        }
    } else {
        std::filesystem::path p(fromPatternRaw);
        if (!std::filesystem::exists(p) || !std::filesystem::is_regular_file(p)) {
            throw std::runtime_error("Source file does not exist: " + p.string());
        }
        srcFiles.push_back(p);
    }

    for (const auto& p : srcFiles) {
        copySingleToRt11(imagePath, p, noReplace, optionalDateWord);
    }
}

// ------------------------------
// Help
// ------------------------------
void printHelp() {
    std::cout
        << "RT-11 Disk Utility (Rt11Dir)\n\n"
        << "Usage:\n"
        << "  Rt11Dir <rt11diskimage.dsk>\n"
        << "      Lists all permanent files on the RT-11 disk.\n\n"
        << "  Rt11Dir <rt11diskimage.dsk> /brief | /b\n"
        << "      Lists only NAME.EXT for permanent files.\n\n"
        << "  Rt11Dir <rt11diskimage.dsk> /empty | /e\n"
        << "      Includes empty directory entries (<EMPTY>) in the listing.\n\n"
        << "Copying FROM RT-11 to Windows:\n"
        << "  Rt11Dir <rt11diskimage.dsk> /copyfrom /to\n"
        << "      Copies all RT-11 files to the current Windows directory.\n\n"
        << "  Rt11Dir <rt11diskimage.dsk> /copyfrom:RT11FILE.EXT /to\n"
        << "      Copies a specific RT-11 file to the current Windows directory.\n\n"
        << "  Rt11Dir <rt11diskimage.dsk> /copyfrom:pattern /to:folder\n"
        << "      Copies matching RT-11 files (supports wildcards) into the given folder.\n"
        << "      Examples:\n"
        << "          /copyfrom:*.SAV /to\n"
        << "          /copyfrom:*.TSX /to:C:\\TEMP\\\n"
        << "          /copyfrom:*.TSX /to:C:\\TEMP\\*.*\n\n"
        << "Copying TO RT-11 from Windows:\n"
        << " IMPORTANT IF YOU DON'T HAVE A Y2K PATCHED RT-11 use the /todate option and specify a pre-1990 date.\n"
        << " YOU CAN FIND PATCHED FILES AT: https://pdp.org.ru/files.pl \n"
        << "  Rt11Dir <rt11diskimage.dsk> /copyto /from:file.txt\n"
        << "      Copies file.txt into RT-11, truncating the name to 6.3 and uppercasing.\n\n"
        << "  Rt11Dir <rt11diskimage.dsk> /copyto /from:*.* [/todate:dd-MMM-yy]\n"
        << "      Copies all files in the current Windows directory to RT-11, each\n"
        << "      truncated to 6.3 upper-case RT-11 names.\n"
        << "      Optional /todate specifies the file date to use (e.g., /todate:15-JAN-97).\n"
        << "      If not specified, the current system date is used.\n\n"
        << "/noreplace:\n"
        << "  When used with /copyto or /copyfrom, existing destination files are not\n"
        << "  overwritten. Comparisons are case-insensitive.\n\n"
        << "/todate:dd-MMM-yy:\n"
        << "  Specifies the date to write to RT-11 directory entries when copying files\n"
        << "  to RT-11 with /copyto. Format is 2-digit day, 3-letter month, 2-digit year.\n"
        << "  Examples: /todate:15-JAN-97 or /todate:01-DEC-99\n"
        << "  Year interpretation: 72-99 = 1972-1999, 00-71 = 2000-2071\n"
        << "  RT-11 date format supports years 1972-2099.\n\n"
        << "Notes:\n"
        << "  - RT-11 filenames on disk are max 6 characters + 3-character extension.\n"
        << "  - Filenames are stored as RAD50 and decoded per the RT-11 Volume and File\n"
        << "    Formats manual.\n"
        << "  - Creation dates are shown and set using the RT-11 packed date format.\n";
}

// ------------------------------
// Main
// ------------------------------
int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            printHelp();
            return 0;
        }

        std::string arg1 = argv[1];
        if (arg1 == "/help" || arg1 == "/h" || arg1 == "/?") {
            printHelp();
            return 0;
        }

        std::string imagePath = argv[1];

        bool brief      = false;
        bool showEmpty  = false;
        bool doCopyFrom = false;
        bool doCopyTo   = false;
        bool noReplace  = false;

        std::string copyFromPattern;
        std::string copyToFromPattern;
        std::string toPath;
        std::string toDateStr;
        uint16_t optionalDateWord = 0;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];

            if (arg == "/help" || arg == "/h" || arg == "/?") {
                printHelp();
                return 0;
            } else if (arg == "/brief" || arg == "/b") {
                brief = true;
            } else if (arg == "/empty" || arg == "/e") {
                showEmpty = true;
            } else if (arg == "/copyfrom") {
                doCopyFrom = true;
            } else if (arg.rfind("/copyfrom:", 0) == 0) {
                doCopyFrom = true;
                copyFromPattern = arg.substr(11);
            } else if (arg == "/copyto") {
                doCopyTo = true;
            } else if (arg.rfind("/from:", 0) == 0) {
                copyToFromPattern = arg.substr(6);
            } else if (arg == "/to") {
                toPath.clear();
            } else if (arg.rfind("/to:", 0) == 0) {
                toPath = arg.substr(4);
            } else if (arg == "/noreplace") {
                noReplace = true;
            } else if (arg.rfind("/todate:", 0) == 0) {
                toDateStr = arg.substr(8);
                // Parse and validate the date string
                int day, month, year;
                if (!parseDateString(toDateStr, day, month, year)) {
                    std::cerr << "Error: Invalid date format: " << toDateStr << "\n";
                    std::cerr << "Expected format: dd-MMM-yy (e.g., 15-JAN-97 or 01-DEC-99)\n";
                    return 1;
                }
                optionalDateWord = encodeRt11Date(year, month, day);
                if (optionalDateWord == 0) {
                    std::cerr << "Error: Failed to encode date: " << toDateStr << "\n";
                    return 1;
                }
                std::cout << "Using custom date: " << toDateStr 
                          << " (" << formatRt11Date(optionalDateWord) << ")\n";
            }
        }

        if (doCopyFrom && doCopyTo) {
            std::cerr << "Cannot use /copyfrom and /copyto in the same command.\n";
            return 1;
        }

        if (doCopyFrom) {
            copyFromRt11(imagePath, copyFromPattern, toPath, noReplace);
        } else if (doCopyTo) {
            if (copyToFromPattern.empty()) {
                throw std::runtime_error("/copyto requires a /from:filename or pattern");
            }
            copyToRt11(imagePath, copyToFromPattern, noReplace, optionalDateWord);
        } else {
            showDirectory(imagePath, brief, showEmpty);
            
            // Also show bad block table for diagnostics (unless in brief mode)
            if (!brief) {
                checkBadBlockTable(imagePath);
            }
        }

        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
