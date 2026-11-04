// maximo_kit: turns the player's own disc into the inputs of the build. Nothing it writes may ever enter git.
//
//   maximo_kit prepare <disc.iso or extracted folder> <repository root> <generated folder>
//
// It opens the disc read-only, checks the game executable against the PAL release, and writes:
//   <generated>/SLES_507.03          the executable, for the recompiler
//   <generated>/recomp.toml          the recompiler's settings (data/funcs.csv names the functions)
//   <generated>/vu1/vu1_<hash>.bin   VU1 microcode images, assembled from the executable by data/vu1/recipe.txt
//   <generated>/vu1/vu1_<hash>.pcs   their entry points (data/vu1)
//   <generated>/vu1aot/vu1aot_registry.cpp
//   <generated>/vu1_images.txt       the image names, for build.cmd
//
//   maximo_kit sha256 <file>         prints the file's SHA-256
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace
{
    constexpr const char *kElfName = "SLES_507.03";
    constexpr const char *kElfSha256 = "03615557E6291B882A54165E068F653C8613856852E4250F7DE4B075ABFC3C99";
    constexpr size_t kVu1ImageSize = 16 * 1024;
    constexpr uint64_t kSector = 2048;

    std::string sha256(const uint8_t *data, size_t size)
    {
        uint8_t digest[32];
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data), static_cast<ULONG>(size), digest,
                       sizeof digest) != 0)
            throw std::runtime_error("Windows could not compute a SHA-256 hash");
        std::string text;
        char hex[3];
        for (uint8_t b : digest)
        {
            std::snprintf(hex, sizeof hex, "%02X", b);
            text += hex;
        }
        return text;
    }

    Bytes readFile(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw std::runtime_error("cannot open " + path.string());
        return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    void writeFile(const fs::path &path, const void *data, size_t size)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
        if (!out)
            throw std::runtime_error("cannot write " + path.string());
    }

    uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

    // ISO 9660: the executable sits in the root directory of a plain 2048-byte-sector image.
    Bytes readElfFromIso(const fs::path &iso)
    {
        std::ifstream in(iso, std::ios::binary);
        if (!in)
            throw std::runtime_error("cannot open " + iso.string());

        const auto readAt = [&](uint64_t offset, size_t size) {
            Bytes data(size);
            in.seekg(static_cast<std::streamoff>(offset));
            in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));
            if (!in)
                throw std::runtime_error("the disc image ended early");
            return data;
        };

        const Bytes descriptor = readAt(16 * kSector, kSector);
        if (descriptor[0] != 1 || std::string(descriptor.begin() + 1, descriptor.begin() + 6) != "CD001")
            throw std::runtime_error("this is not a plain ISO image with 2048-byte sectors");

        const uint8_t *root = &descriptor[156];
        const Bytes directory = readAt(le32(root + 2) * kSector, le32(root + 10));
        const std::string wanted = std::string(kElfName) + ";1";

        for (size_t at = 0; at < directory.size();)
        {
            const uint8_t length = directory[at];
            if (length == 0)
            {
                at = (at / kSector + 1) * kSector; // records do not cross sectors
                continue;
            }
            const uint8_t nameLength = directory[at + 32];
            std::string name(reinterpret_cast<const char *>(&directory[at + 33]), nameLength);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
            if (name == wanted)
                return readAt(le32(&directory[at + 2]) * kSector, le32(&directory[at + 10]));
            at += length;
        }
        throw std::runtime_error(std::string("this is not the PAL Maximo disc: ") + kElfName + " is missing");
    }

    Bytes readElf(const fs::path &disc)
    {
        Bytes elf = fs::is_directory(disc) ? readFile(disc / kElfName) : readElfFromIso(disc);
        if (sha256(elf.data(), elf.size()) != kElfSha256)
            throw std::runtime_error("this disc's game executable does not match the PAL release (SLES-50703, v1.00)");
        return elf;
    }

    std::string forwardSlashes(const fs::path &path) { return path.generic_string(); }

    void writeRecompilerSettings(const fs::path &repo, const fs::path &generated)
    {
        std::ofstream out(generated / "recomp.toml", std::ios::binary);
        out << "[general]\n"
            << "input = \"" << forwardSlashes(generated / kElfName) << "\"\n"
            << "ghidra_output = \"" << forwardSlashes(repo / "data" / "funcs.csv") << "\"\n"
            << "output = \"" << forwardSlashes(generated / "game") << "\"\n"
            << "single_file_output = false\npatch_syscalls = false\npatch_cop0 = true\npatch_cache = true\n"
            << "stubs = []\nskip = []\n";
        if (!out)
            throw std::runtime_error("cannot write recomp.toml");
    }

    // recipe.txt: "image <name> <sha256>", then "copy <image offset> <executable offset> <bytes>" in hex.
    std::vector<std::string> buildVu1Images(const Bytes &elf, const fs::path &repo, const fs::path &generated)
    {
        const fs::path data = repo / "data" / "vu1";
        const fs::path out = generated / "vu1";
        fs::create_directories(out);

        std::ifstream recipe(data / "recipe.txt");
        if (!recipe)
            throw std::runtime_error("data/vu1/recipe.txt is missing");

        std::vector<std::string> names;
        Bytes image;
        std::string expected;

        const auto finish = [&] {
            if (names.empty())
                return;
            if (sha256(image.data(), image.size()) != expected)
                throw std::runtime_error("VU1 image " + names.back() + " does not match its recipe");
            writeFile(out / (names.back() + ".bin"), image.data(), image.size());
            fs::copy_file(data / (names.back() + ".pcs"), out / (names.back() + ".pcs"),
                          fs::copy_options::overwrite_existing);
        };

        std::string line;
        while (std::getline(recipe, line))
        {
            std::istringstream words(line);
            std::string word;
            if (!(words >> word) || word[0] == '#')
                continue;
            if (word == "image")
            {
                finish();
                std::string name;
                words >> name >> expected;
                names.push_back(name);
                image.assign(kVu1ImageSize, 0);
            }
            else if (word == "copy")
            {
                uint64_t to = 0, from = 0, size = 0;
                words >> std::hex >> to >> from >> size;
                if (names.empty() || to + size > image.size() || from + size > elf.size())
                    throw std::runtime_error("bad line in data/vu1/recipe.txt: " + line);
                std::copy_n(elf.begin() + static_cast<std::ptrdiff_t>(from), size,
                            image.begin() + static_cast<std::ptrdiff_t>(to));
            }
            else
                throw std::runtime_error("bad line in data/vu1/recipe.txt: " + line);
        }
        finish();
        return names;
    }

    void writeRegistry(const std::vector<std::string> &names, const fs::path &generated)
    {
        fs::create_directories(generated / "vu1aot");
        std::ofstream out(generated / "vu1aot" / "vu1aot_registry.cpp", std::ios::binary);
        out << "// Written by maximo_kit: registers the VU1 microcode compiled from the player's disc.\n"
            << "#include \"runtime/ps2_vu1_aot.h\"\n\n";
        for (const std::string &name : names)
            out << "extern const VU1AotImage vu1aot_image_" << name.substr(4) << ";\n";
        out << "\nvoid registerMaximoVu1Aot()\n{\n    static const VU1AotImage images[] = {\n";
        for (const std::string &name : names)
            out << "        vu1aot_image_" << name.substr(4) << ",\n";
        out << "    };\n    VU1Interpreter::registerAotImages(images, sizeof(images) / sizeof(images[0]));\n}\n";
        if (!out)
            throw std::runtime_error("cannot write vu1aot_registry.cpp");

        std::ofstream list(generated / "vu1_images.txt", std::ios::binary);
        for (const std::string &name : names)
            list << name << "\n";
    }

    void prepare(const fs::path &disc, const fs::path &repoArg, const fs::path &generatedArg)
    {
        const fs::path repo = fs::absolute(repoArg);
        const fs::path generated = fs::absolute(generatedArg);
        fs::create_directories(generated);

        const Bytes elf = readElf(disc);
        writeFile(generated / kElfName, elf.data(), elf.size());
        writeRecompilerSettings(repo, generated);
        writeRegistry(buildVu1Images(elf, repo, generated), generated);
    }
}

int main(int argc, char **argv)
{
    try
    {
        if (argc == 5 && std::string(argv[1]) == "prepare")
        {
            prepare(argv[2], argv[3], argv[4]);
            std::puts("Disc checked: PAL Maximo. Build inputs written.");
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "sha256")
        {
            const Bytes file = readFile(argv[2]);
            std::puts(sha256(file.data(), file.size()).c_str());
            return 0;
        }
        std::fputs("usage: maximo_kit prepare <disc.iso or folder> <repository> <generated folder>\n"
                   "       maximo_kit sha256 <file>\n", stderr);
        return 2;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
