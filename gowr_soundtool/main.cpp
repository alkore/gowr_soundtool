#include <iostream>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <span>
#include <algorithm>
#include <mutex>
#include <format>
#include <cstring>

namespace fs = std::filesystem;

struct TOC_Entry_t
{
    std::uint32_t m_file_id;
    std::uint32_t m_file_size;
    std::uint32_t m_offset;
};

struct TOC_Header_t
{
    std::uint32_t m_magic;         // 0x4b415041
    std::uint32_t m_version;
    std::uint32_t m_entries_count;
    std::uint32_t m_parts_count;   // always 1
};

// Read binary file into string
[[nodiscard]] std::string BinToString(const fs::path& file_path)
{
    std::ifstream file(file_path, std::ios::binary);
    if (!file)
        throw std::runtime_error(std::format("Failed to open file '{}'", file_path.string()));

    file.seekg(0, std::ios::end);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size <= 0)
        throw std::runtime_error(std::format("File '{}' is empty", file_path.string()));

    std::string buffer(static_cast<std::size_t>(size), '\0');

    if (!file.read(buffer.data(), size))
        throw std::runtime_error(std::format("Failed to read file '{}'", file_path.string()));

    return buffer;
}

// Write binary file
void WriteFile(const fs::path& file_path, std::span<const char> data)
{
    std::ofstream file(file_path, std::ios::binary);
    if (!file)
        throw std::runtime_error(std::format("Failed to write file '{}'", file_path.string()));

    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!file)
        throw std::runtime_error(std::format("Failed while writing file '{}'", file_path.string()));
}

// Unpack .toc + .audiopack
void UnpackAudio(const fs::path& toc_path)
{
    fs::path out_dir = toc_path.parent_path() / toc_path.stem();
    fs::create_directories(out_dir);

    const std::string toc_data = BinToString(toc_path);

    fs::path audiopack_path = toc_path;
    audiopack_path.replace_extension("");
    audiopack_path += ".0.audiopack";

    const std::string audiopack_data = BinToString(audiopack_path);

    if (toc_data.size() < sizeof(TOC_Header_t))
        throw std::runtime_error("Invalid TOC file");

    const auto* header = reinterpret_cast<const TOC_Header_t*>(toc_data.data());

    if (header->m_magic != 0x4b415041)
        throw std::runtime_error("Invalid TOC magic");

    std::size_t offset = sizeof(TOC_Header_t);

    for (std::uint32_t i = 0; i < header->m_entries_count; ++i)
    {
        if (offset + sizeof(TOC_Entry_t) > toc_data.size())
            throw std::runtime_error("Corrupted TOC");

        const auto* entry =
            reinterpret_cast<const TOC_Entry_t*>(toc_data.data() + offset);
        offset += sizeof(TOC_Entry_t);

        if (entry->m_offset + entry->m_file_size > audiopack_data.size())
            throw std::runtime_error("Corrupted audiopack");

        std::span<const char> file_data(
            audiopack_data.data() + entry->m_offset,
            entry->m_file_size
        );

        fs::path out_file = out_dir / std::format("{}.wem", entry->m_file_id);
        std::cout << "Writing " << out_file << '\n';

        WriteFile(out_file, file_data);
    }

    std::cout << "\nUnpack completed successfully\n";
}

// Pack directory of .wem into .toc + .audiopack
void PackAudio(const fs::path& wem_directory)
{
    std::vector<fs::path> wem_files;
    std::vector<TOC_Entry_t> entries;

    for (const auto& entry : fs::directory_iterator(wem_directory))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".wem")
            wem_files.push_back(entry.path());
    }

    if (wem_files.empty())
        throw std::runtime_error("No .wem files found");

    std::sort(wem_files.begin(), wem_files.end(),
        [](const fs::path& a, const fs::path& b)
        {
            return std::stoi(a.stem().string()) < std::stoi(b.stem().string());
        });

    fs::path audiopack_file = wem_directory;
    audiopack_file.replace_extension("");
    audiopack_file += ".0.audiopack";

    std::ofstream pack_out(audiopack_file, std::ios::binary);
    if (!pack_out)
        throw std::runtime_error("Failed to create audiopack");

    std::uint32_t current_offset = 0;

    for (const auto& wem : wem_files)
    {
        std::string data = BinToString(wem);

        TOC_Entry_t entry;
        entry.m_file_id = static_cast<std::uint32_t>(std::stoi(wem.stem().string()));
        entry.m_file_size = static_cast<std::uint32_t>(data.size());
        entry.m_offset = current_offset;

        pack_out.write(data.data(), data.size());
        current_offset += entry.m_file_size;
        entries.push_back(entry);
    }

    pack_out.close();

    fs::path toc_file = wem_directory;
    toc_file.replace_extension("");
    toc_file += ".audiopack.toc";

    std::ofstream toc_out(toc_file, std::ios::binary);
    if (!toc_out)
        throw std::runtime_error("Failed to create TOC");

    TOC_Header_t header{
        0x4b415041,
        1,
        static_cast<std::uint32_t>(entries.size()),
        1
    };

    toc_out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    for (const auto& e : entries)
        toc_out.write(reinterpret_cast<const char*>(&e), sizeof(e));

    std::cout << "\nPack completed successfully\n";
}

int main(int argc, char* argv[])
{
    try
    {
        if (argc < 3)
            throw std::invalid_argument(
                "Usage:\n"
                "  gowr_soundtool --pack <wem_dir>\n"
                "  gowr_soundtool --unpack <file.toc>"
            );

        if (std::strcmp(argv[1], "--unpack") == 0)
        {
            UnpackAudio(argv[2]);
        }
        else if (std::strcmp(argv[1], "--pack") == 0)
        {
            PackAudio(argv[2]);
        }
        else
        {
            throw std::invalid_argument("Invalid argument");
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Error] " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
