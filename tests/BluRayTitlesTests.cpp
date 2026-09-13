#include "app/BluRayTitles.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace {

class TemporaryDisc {
public:
    TemporaryDisc() {
        wchar_t temporary[MAX_PATH]{};
        const DWORD length = GetTempPathW(ARRAYSIZE(temporary), temporary);
        if (length == 0 || length >= ARRAYSIZE(temporary)) {
            throw std::runtime_error("GetTempPath failed");
        }
        m_root = std::filesystem::path(temporary)
            / (L"odyssey-bluray-titles-" + std::to_wstring(GetCurrentProcessId())
               + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(playlistDirectory());
    }

    ~TemporaryDisc() {
        std::error_code error;
        std::filesystem::remove_all(m_root, error);
    }

    const std::filesystem::path& root() const noexcept { return m_root; }
    std::filesystem::path playlistDirectory() const {
        return m_root / L"BDMV" / L"PLAYLIST";
    }

private:
    std::filesystem::path m_root;
};

void put16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

std::vector<std::uint8_t> makePlaylist(
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& times,
    std::uint16_t recordLength = 20) {
    constexpr std::size_t playlistOffset = 40;
    std::vector<std::uint8_t> bytes(playlistOffset + 10, 0);
    std::copy_n("MPLS0200", 8, bytes.begin());
    put32(bytes, 8, static_cast<std::uint32_t>(playlistOffset));
    put16(bytes, playlistOffset + 6, static_cast<std::uint16_t>(times.size()));

    for (const auto& [inTime, outTime] : times) {
        const std::size_t record = bytes.size();
        bytes.resize(record + 2 + recordLength, 0x5a);
        put16(bytes, record, recordLength);
        put32(bytes, record + 2 + 12, inTime);
        put32(bytes, record + 2 + 16, outTime);
    }
    const std::size_t markOffset = bytes.size();
    bytes.resize(markOffset + 4, 0);
    put32(
        bytes, playlistOffset,
        static_cast<std::uint32_t>(markOffset - (playlistOffset + 4)));
    put32(bytes, 12, static_cast<std::uint32_t>(markOffset));
    put32(bytes, 16, 0);
    return bytes;
}

void writeFile(
    const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.good());
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good());
}

TEST(BluRayTitles, SortsValidSiblingsAndReportsMalformedLengths) {
    TemporaryDisc disc;
    writeFile(
        disc.playlistDirectory() / L"00002.mpls",
        makePlaylist({{0, 45000}}, 64));
    writeFile(
        disc.playlistDirectory() / L"00001.mpls",
        makePlaylist({{90000, 135000}}, 32));
    writeFile(
        disc.playlistDirectory() / L"00003.mpls",
        makePlaylist({{0, 45000}, {1000, 46000}}, 48));

    auto sectionOverflow = makePlaylist({{0, 45000}});
    put32(sectionOverflow, 40, 0xffffffffu);
    writeFile(disc.playlistDirectory() / L"00004.mpls", sectionOverflow);

    auto recordOverflow = makePlaylist({{0, 45000}});
    put16(recordOverflow, 50, 0xffffu);
    writeFile(disc.playlistDirectory() / L"00005.mpls", recordOverflow);
    writeFile(
        disc.playlistDirectory() / L"ABCDE.mpls",
        makePlaylist({{0, 45000}}));

    const odyssey::BluRayTitleScan scan =
        odyssey::enumerateBluRayTitles(disc.root().wstring());

    ASSERT_FALSE(scan.cancelled);
    ASSERT_EQ(scan.titles.size(), 3u);
    EXPECT_EQ(scan.titles[0].label, L"00003");
    EXPECT_DOUBLE_EQ(scan.titles[0].durationSeconds, 2.0);
    EXPECT_EQ(scan.titles[1].label, L"00001");
    EXPECT_EQ(scan.titles[2].label, L"00002");
    EXPECT_EQ(scan.titles[1].relativePlaylistPath, L"BDMV\\PLAYLIST\\00001.mpls");
    EXPECT_EQ(scan.issues.size(), 3u);
}

TEST(BluRayTitles, StopsBeforeReadingWhenCancelled) {
    TemporaryDisc disc;
    writeFile(
        disc.playlistDirectory() / L"00001.mpls",
        makePlaylist({{0, 45000}}));
    HANDLE event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    ASSERT_NE(event, nullptr);

    const odyssey::BluRayTitleScan scan =
        odyssey::enumerateBluRayTitles(disc.root().wstring(), event);
    CloseHandle(event);

    EXPECT_TRUE(scan.cancelled);
    EXPECT_TRUE(scan.titles.empty());
    EXPECT_TRUE(scan.issues.empty());
}

TEST(BluRayTitles, EnumeratesTheIndependentBigHeroPlaylistSnapshot) {
    const std::filesystem::path source = std::filesystem::path(__FILE__)
        .parent_path().parent_path() / L"build" / L"mvc_probe"
        / L"big-hero-playlists";
    if (!std::filesystem::is_directory(source)) {
        GTEST_SKIP() << "Ignored Big Hero playlist snapshot is unavailable";
    }
    TemporaryDisc disc;
    std::size_t copied = 0;
    for (const std::filesystem::directory_entry& entry
         : std::filesystem::directory_iterator(source)) {
        if (entry.is_regular_file() && entry.path().extension() == L".mpls") {
            std::filesystem::copy_file(
                entry.path(), disc.playlistDirectory() / entry.path().filename());
            ++copied;
        }
    }
    ASSERT_EQ(copied, 94u);

    const odyssey::BluRayTitleScan scan =
        odyssey::enumerateBluRayTitles(disc.root().wstring());
    ASSERT_FALSE(scan.cancelled);
    ASSERT_TRUE(scan.issues.empty());
    ASSERT_EQ(scan.titles.size(), 94u);
    const auto title800 = std::find_if(
        scan.titles.begin(), scan.titles.end(),
        [](const odyssey::BluRayTitle& title) { return title.label == L"00800"; });
    const auto title801 = std::find_if(
        scan.titles.begin(), scan.titles.end(),
        [](const odyssey::BluRayTitle& title) { return title.label == L"00801"; });
    ASSERT_NE(title800, scan.titles.end());
    ASSERT_NE(title801, scan.titles.end());
    EXPECT_NEAR(title800->durationSeconds, 6112.68988888889, 1e-9);
    EXPECT_DOUBLE_EQ(title800->durationSeconds, title801->durationSeconds);
}

} // namespace
