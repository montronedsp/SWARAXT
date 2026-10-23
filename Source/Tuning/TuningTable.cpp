// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Tuning/TuningTable.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace swaraxt::tuning {
namespace {

bool finitePositive(double value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

void setError(ParseError& error, const char* message)
{
    error.message = message;
}

void writeU16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
}

void writeI16(std::vector<uint8_t>& out, int16_t value)
{
    writeU16(out, static_cast<uint16_t>(value));
}

void writeF64(std::vector<uint8_t>& out, double value)
{
    static_assert(sizeof(double) == 8, "IEEE-754 binary64 required");
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xffu));
}

bool hasBytes(const uint8_t* cursor, const uint8_t* end, size_t count) noexcept
{
    return cursor != nullptr && end != nullptr && cursor <= end
        && static_cast<size_t>(end - cursor) >= count;
}

bool readU16(const uint8_t*& cursor, const uint8_t* end, uint16_t& value)
{
    if (! hasBytes(cursor, end, 2))
        return false;
    value = static_cast<uint16_t>(cursor[0] | (static_cast<uint16_t>(cursor[1]) << 8));
    cursor += 2;
    return true;
}

bool readI16(const uint8_t*& cursor, const uint8_t* end, int16_t& value)
{
    uint16_t raw = 0;
    if (! readU16(cursor, end, raw))
        return false;
    value = static_cast<int16_t>(raw);
    return true;
}

bool readF64(const uint8_t*& cursor, const uint8_t* end, double& value)
{
    if (! hasBytes(cursor, end, 8))
        return false;
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i)
        bits |= static_cast<uint64_t>(cursor[i]) << (8 * i);
    std::memcpy(&value, &bits, sizeof(value));
    cursor += 8;
    return true;
}

struct PitchLine {
    double cents = 0.0;
};

std::string_view stripScalaComment(std::string_view text) noexcept
{
    const auto comment = text.find('!');
    if (comment != std::string_view::npos)
        text = text.substr(0, comment);
    while (! text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (! text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

bool parseFullInteger(std::string_view text, long long& value) noexcept
{
    text = stripScalaComment(text);
    if (text.empty())
        return false;
    if (text.front() == '+')
    {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '-' || text.front() == '+')
            return false;
    }
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc() && parsed.ptr == last;
}

bool historicalCommaFraction(const char* cursor, const char* last) noexcept
{
    // The built-in EDO scales place the fractional digits after a comma.
    // The previous converter kept only the number before that comma.
    if (cursor == last || *cursor != ',')
        return false;
    ++cursor;
    if (cursor == last)
        return false;
    for (; cursor != last; ++cursor)
    {
        if (*cursor < '0' || *cursor > '9')
            return false;
    }
    return true;
}

bool parseFullReal(std::string_view text, double& value) noexcept
{
    text = stripScalaComment(text);
    if (text.empty())
        return false;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto parsed = std::from_chars(first, last, value, std::chars_format::general);
    if (parsed.ec != std::errc() || ! std::isfinite(value))
        return false;
    return parsed.ptr == last || historicalCommaFraction(parsed.ptr, last);
}

bool parsePitchLine(const std::string& line, PitchLine& pitch, ParseError& error)
{
    const auto slash = line.find('/');
    if (slash == std::string::npos)
    {
        double cents = 0.0;
        if (! parseFullReal(line, cents))
        {
            setError(error, "pitch is not a finite cent value");
            return false;
        }
        pitch.cents = cents;
        return true;
    }
    const auto numerator = line.substr(0, slash);
    const auto denominator = line.substr(slash + 1);
    if (denominator.find('/') != std::string::npos)
    {
        setError(error, "ratio must be a positive finite fraction");
        return false;
    }
    double num = 0.0;
    double den = 0.0;
    if (! parseFullReal(numerator, num) || ! parseFullReal(denominator, den)
        || ! finitePositive(num) || ! finitePositive(den))
    {
        setError(error, "ratio must be a positive finite fraction");
        return false;
    }
    pitch.cents = 1200.0 * std::log2(num / den);
    if (! std::isfinite(pitch.cents))
    {
        setError(error, "pitch line is malformed");
        return false;
    }
    return true;
}

std::vector<std::string> contentLines(const char* text)
{
    std::vector<std::string> lines;
    if (text == nullptr)
        return lines;
    std::string current;
    for (const char* cursor = text;; ++cursor)
    {
        const char ch = *cursor;
        if (ch == '\0' || ch == '\n')
        {
            while (! current.empty() && (current.back() == '\r' || current.back() == ' ' || current.back() == '\t'))
                current.pop_back();
            size_t start = 0;
            while (start < current.size() && (current[start] == ' ' || current[start] == '\t'))
                ++start;
            if (start < current.size() && current[start] != '!')
                lines.push_back(current.substr(start));
            current.clear();
            if (ch == '\0')
                break;
        }
        else
        {
            current.push_back(ch);
        }
    }
    return lines;
}

double centsForDegree(const TuningSnapshot& snapshot, int degree) noexcept
{
    const int period = snapshot.formalPeriod > 0 ? static_cast<int>(snapshot.formalPeriod) : 1;
    int periodIndex = 0;
    int inside = degree;
    if (inside >= 0)
    {
        periodIndex = inside / period;
        inside %= period;
    }
    else
    {
        periodIndex = -(((-inside) + period - 1) / period);
        inside = degree - periodIndex * period;
    }
    double cents = static_cast<double>(periodIndex) * snapshot.periodCents;
    if (inside > 0)
    {
        const auto index = static_cast<size_t>(inside - 1);
        if (index < snapshot.degreeCents.size())
            cents += snapshot.degreeCents[index];
    }
    return cents;
}

bool rebuildFrequencies(TuningSnapshot& snapshot, ParseError& error)
{
    if (! finitePositive(snapshot.referenceHz) || ! finitePositive(snapshot.periodCents))
    {
        setError(error, "reference frequency and period must be positive");
        return false;
    }
    if (snapshot.formalPeriod == 0 || snapshot.degreeCount == 0
        || snapshot.degreeCents.size() != snapshot.degreeCount)
    {
        setError(error, "scale degree table is empty or inconsistent");
        return false;
    }
    const int ref = snapshot.referenceNote;
    if (ref < 0 || ref >= kMidiNotes || snapshot.midiDegree[ref] < 0)
    {
        setError(error, "reference note is unmapped");
        return false;
    }
    const double refCents = centsForDegree(snapshot, snapshot.midiDegree[ref]);
    for (int note = 0; note < kMidiNotes; ++note)
    {
        snapshot.midiHz[note] = 0.0;
        if (snapshot.midiDegree[note] < 0)
            continue;
        const double cents = centsForDegree(snapshot, snapshot.midiDegree[note]) - refCents;
        const double hz = snapshot.referenceHz * std::pow(2.0, cents / 1200.0);
        if (! finitePositive(hz))
        {
            setError(error, "mapped frequency is not positive");
            return false;
        }
        snapshot.midiHz[note] = hz;
    }
    return true;
}

}  // namespace

double TuningSnapshot::hzForDegree(int degree) const noexcept
{
    if (formalPeriod == 0 || referenceHz <= 0.0 || periodCents <= 0.0)
        return 0.0;
    if (referenceNote < 0 || referenceNote >= kMidiNotes || midiDegree[referenceNote] < 0)
        return 0.0;
    const double cents = centsForDegree(*this, degree) - centsForDegree(*this, midiDegree[referenceNote]);
    return referenceHz * std::pow(2.0, cents / 1200.0);
}

double TuningSnapshot::equalHzFromCvSemitones(double semitonesFromMidi0, double referenceNote, double referenceHz) noexcept
{
    if (! std::isfinite(semitonesFromMidi0) || ! std::isfinite(referenceNote) || ! finitePositive(referenceHz))
        return 0.0;
    return referenceHz * std::pow(2.0, (semitonesFromMidi0 - referenceNote) / 12.0);
}

int TuningSnapshot::nearestMappedMidi(double hz) const noexcept
{
    if (! finitePositive(hz))
        return -1;
    const double target = std::log2(hz);
    int best = -1;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (int note = 0; note < kMidiNotes; ++note)
    {
        if (! mapped(note))
            continue;
        const double distance = std::abs(std::log2(midiHz[note]) - target);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = note;
        }
    }
    return best;
}

bool parseScalaAndKeyboard(const char* sclText, const char* kbmText, TuningSnapshot& out, ParseError& error)
{
    out = {};
    const auto scl = contentLines(sclText);
    if (scl.size() < 2)
    {
        setError(error, "scala file is missing a description or degree count");
        return false;
    }
    std::snprintf(out.name, sizeof(out.name), "%s", scl[0].c_str());
    long long countValue = 0;
    if (! parseFullInteger(scl[1], countValue))
    {
        setError(error, "scala degree count is malformed");
        return false;
    }
    if (countValue < 1 || countValue > kMaxDegrees)
    {
        setError(error, "scala degree count is out of range");
        return false;
    }
    const int count = static_cast<int>(countValue);
    if (scl.size() < static_cast<size_t>(count) + 2)
    {
        setError(error, "scala file has fewer pitches than its count");
        return false;
    }
    out.degreeCents.reserve(static_cast<size_t>(count - 1));
    for (int i = 0; i < count; ++i)
    {
        PitchLine pitch;
        if (! parsePitchLine(scl[static_cast<size_t>(2 + i)], pitch, error))
            return false;
        if (i + 1 == count)
            out.periodCents = pitch.cents;
        else
            out.degreeCents.push_back(pitch.cents);
    }
    out.degreeCount = static_cast<uint16_t>(out.degreeCents.size());
    out.formalPeriod = static_cast<uint16_t>(count);
    if (! finitePositive(out.periodCents))
    {
        setError(error, "scale period must be a positive interval");
        return false;
    }
    for (int i = 1; i < static_cast<int>(out.degreeCents.size()); ++i)
    {
        if (! (out.degreeCents[static_cast<size_t>(i)] > out.degreeCents[static_cast<size_t>(i - 1)]))
        {
            setError(error, "scala pitches must be strictly increasing");
            return false;
        }
    }
    if (! out.degreeCents.empty() && ! (out.periodCents > out.degreeCents.back()))
    {
        setError(error, "scala period must be above the last interior pitch");
        return false;
    }

    for (int note = 0; note < kMidiNotes; ++note)
        out.midiDegree[note] = -1;
    out.referenceNote = 69;
    out.referenceHz = 440.0;
    int middle = 60;
    if (kbmText == nullptr || kbmText[0] == '\0')
    {
        for (int note = 0; note < kMidiNotes; ++note)
            out.midiDegree[note] = static_cast<int16_t>(note - middle);
    }
    else
    {
        const auto kbm = contentLines(kbmText);
        if (kbm.size() < 7)
        {
            setError(error, "keyboard map is incomplete");
            return false;
        }
        long long mapSize = 0;
        long long mapFirstValue = 0;
        long long mapLastValue = 0;
        long long middleValue = 0;
        long long referenceValue = 0;
        long long formal = count;
        double frequency = 440.0;
        if (! parseFullInteger(kbm[0], mapSize) || ! parseFullInteger(kbm[1], mapFirstValue)
            || ! parseFullInteger(kbm[2], mapLastValue) || ! parseFullInteger(kbm[3], middleValue)
            || ! parseFullInteger(kbm[4], referenceValue) || ! parseFullReal(kbm[5], frequency)
            || ! parseFullInteger(kbm[6], formal))
        {
            setError(error, "keyboard map header is malformed");
            return false;
        }
        if (mapSize < 0 || mapFirstValue < 0 || mapFirstValue > 127 || mapLastValue < 0
            || mapLastValue > 127 || mapFirstValue > mapLastValue || middleValue < 0
            || middleValue > 127 || referenceValue < 0 || referenceValue > 127
            || formal < 1 || formal > std::numeric_limits<uint16_t>::max())
        {
            setError(error, "keyboard map header is out of range");
            return false;
        }
        if (! finitePositive(frequency))
        {
            setError(error, "keyboard reference frequency must be positive");
            return false;
        }
        middle = static_cast<int>(middleValue);
        out.referenceNote = static_cast<uint16_t>(referenceValue);
        out.referenceHz = frequency;
        out.formalPeriod = static_cast<uint16_t>(formal);
        const long long entries = mapSize == 0 ? (mapLastValue - mapFirstValue + 1) : mapSize;
        if (entries < 0
            || static_cast<unsigned long long>(entries) > kbm.size() - 7)
        {
            setError(error, "keyboard map is missing entries");
            return false;
        }
        for (long long i = 0; i < entries; ++i)
        {
            const auto& entry = kbm[static_cast<size_t>(7 + i)];
            const long long note = mapFirstValue + i;
            if (note < 0 || note >= kMidiNotes)
                continue;
            const auto token = stripScalaComment(entry);
            if (token == "x" || token == "X")
            {
                out.midiDegree[static_cast<int>(note)] = -1;
                continue;
            }
            long long degree = 0;
            if (! parseFullInteger(entry, degree)
                || degree < std::numeric_limits<int16_t>::min()
                || degree > std::numeric_limits<int16_t>::max())
            {
                setError(error, "keyboard map entry is neither a degree nor x");
                return false;
            }
            out.midiDegree[static_cast<int>(note)] = static_cast<int16_t>(degree);
        }
    }
    return rebuildFrequencies(out, error);
}

bool writeTuningTable(const TuningSnapshot& snapshot, std::vector<uint8_t>& bytes, ParseError& error)
{
    TuningSnapshot check = snapshot;
    if (! rebuildFrequencies(check, error))
        return false;
    bytes.clear();
    bytes.insert(bytes.end(), kMagic, kMagic + 4);
    writeU16(bytes, kFormatVersion);
    writeU16(bytes, 0);
    writeU16(bytes, check.degreeCount);
    writeU16(bytes, check.formalPeriod);
    writeU16(bytes, check.referenceNote);
    writeU16(bytes, 0);
    writeF64(bytes, check.referenceHz);
    writeF64(bytes, check.periodCents);
    for (int i = 0; i < kNameBytes; ++i)
        bytes.push_back(static_cast<uint8_t>(check.name[i]));
    for (double cents : check.degreeCents)
        writeF64(bytes, cents);
    for (int note = 0; note < kMidiNotes; ++note)
        writeI16(bytes, check.midiDegree[note]);
    for (int note = 0; note < kMidiNotes; ++note)
        writeF64(bytes, check.midiHz[note]);
    return true;
}

bool readTuningTable(const uint8_t* data, size_t size, TuningSnapshot& out, ParseError& error)
{
    out = {};
    if (data == nullptr || size < 4 || std::memcmp(data, kMagic, 4) != 0)
    {
        setError(error, "tuning table magic is invalid");
        return false;
    }
    const uint8_t* cursor = data + 4;
    const uint8_t* end = data + size;
    uint16_t version = 0;
    uint16_t flags = 0;
    uint16_t reserved = 0;
    if (! readU16(cursor, end, version) || ! readU16(cursor, end, flags)
        || ! readU16(cursor, end, out.degreeCount) || ! readU16(cursor, end, out.formalPeriod)
        || ! readU16(cursor, end, out.referenceNote) || ! readU16(cursor, end, reserved))
    {
        setError(error, "tuning table header is truncated");
        return false;
    }
    if (version != kFormatVersion)
    {
        setError(error, "tuning table version is unsupported");
        return false;
    }
    if (! readF64(cursor, end, out.referenceHz) || ! readF64(cursor, end, out.periodCents))
    {
        setError(error, "tuning table header is truncated");
        return false;
    }
    if (! hasBytes(cursor, end, kNameBytes))
    {
        setError(error, "tuning table name is truncated");
        return false;
    }
    std::memcpy(out.name, cursor, kNameBytes);
    out.name[kNameBytes - 1] = '\0';
    cursor += kNameBytes;
    if (out.degreeCount > kMaxDegrees)
    {
        setError(error, "tuning table degree count is out of range");
        return false;
    }
    out.degreeCents.resize(out.degreeCount);
    for (uint16_t i = 0; i < out.degreeCount; ++i)
    {
        if (! readF64(cursor, end, out.degreeCents[i]) || ! std::isfinite(out.degreeCents[i]))
        {
            setError(error, "tuning table degree is invalid");
            return false;
        }
    }
    for (int note = 0; note < kMidiNotes; ++note)
    {
        if (! readI16(cursor, end, out.midiDegree[note]))
        {
            setError(error, "tuning table map is truncated");
            return false;
        }
    }
    for (int note = 0; note < kMidiNotes; ++note)
    {
        if (! readF64(cursor, end, out.midiHz[note]) || ! std::isfinite(out.midiHz[note]) || out.midiHz[note] < 0.0)
        {
            setError(error, "tuning table frequency is invalid");
            return false;
        }
    }
    if (cursor != end)
    {
        setError(error, "tuning table has trailing bytes");
        return false;
    }
    TuningSnapshot recomputed = out;
    if (! rebuildFrequencies(recomputed, error))
        return false;
    for (int note = 0; note < kMidiNotes; ++note)
    {
        if (out.midiDegree[note] < 0)
        {
            if (out.midiHz[note] != 0.0)
            {
                setError(error, "unmapped note has a frequency");
                return false;
            }
            continue;
        }
        const double expected = recomputed.midiHz[note];
        const double scale = std::max(1.0, expected);
        if (std::abs(out.midiHz[note] - expected) > 1.0e-9 * scale)
        {
            setError(error, "stored frequency does not match the scale");
            return false;
        }
    }
    return true;
}

}  // namespace swaraxt::tuning
