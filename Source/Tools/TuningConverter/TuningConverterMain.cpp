// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Tuning/TuningTable.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

std::string readFile(const char* path)
{
    std::ifstream input(path, std::ios::binary);
    if (! input)
        return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: SwaraXtTuningConverter input.scl output.swtn [map.kbm]\n";
        return 2;
    }
    const std::string scl = readFile(argv[1]);
    if (scl.empty())
    {
        std::cerr << "failed to read scala file\n";
        return 1;
    }
    std::string kbm;
    if (argc >= 4)
    {
        kbm = readFile(argv[3]);
        if (kbm.empty())
        {
            std::cerr << "failed to read keyboard map\n";
            return 1;
        }
    }
    swaraxt::tuning::TuningSnapshot snapshot;
    swaraxt::tuning::ParseError error;
    if (! swaraxt::tuning::parseScalaAndKeyboard(scl.c_str(), kbm.empty() ? nullptr : kbm.c_str(), snapshot, error))
    {
        std::cerr << error.message << "\n";
        return 1;
    }
    std::vector<uint8_t> bytes;
    if (! swaraxt::tuning::writeTuningTable(snapshot, bytes, error))
    {
        std::cerr << error.message << "\n";
        return 1;
    }
    std::ofstream output(argv[2], std::ios::binary);
    if (! output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        std::cerr << "failed to write tuning table\n";
        return 1;
    }
    std::cout << "wrote " << bytes.size() << " bytes, degrees=" << snapshot.degreeCount
              << " periodCents=" << snapshot.periodCents << "\n";
    return 0;
}
