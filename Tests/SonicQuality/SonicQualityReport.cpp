// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SonicQuality/SonicQualityReport.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace swaraxt::sonic {
namespace {

std::string csvEscape(const std::string& text)
{
    if (text.find_first_of(",\"\n\r") == std::string::npos)
        return text;
    std::string escaped = "\"";
    for (char character : text)
    {
        if (character == '"')
            escaped += "\"\"";
        else
            escaped += character;
    }
    escaped += '"';
    return escaped;
}

std::string jsonEscape(const std::string& text)
{
    std::string escaped;
    for (unsigned char character : text)
    {
        switch (character)
        {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            default:
                if (character < 0x20)
                    escaped += ' ';
                else
                    escaped += static_cast<char>(character);
                break;
        }
    }
    return escaped;
}

}  // namespace

void SonicReport::setContext(std::string commit, std::string profile, std::string build)
{
    commit_ = std::move(commit);
    profile_ = std::move(profile);
    build_ = std::move(build);
}

void SonicReport::add(Measurement row)
{
    if (row.result == Classification::fail)
        ++failures_;
    if (row.commit.empty())
        row.commit = commit_;
    rows_.push_back(std::move(row));
}

void SonicReport::addTableRow(const std::string& table, const std::string& header, const std::string& row)
{
    auto& entry = tables_[table];
    if (entry.first.empty())
        entry.first = header;
    entry.second.push_back(row);
}

int SonicReport::count(Classification classification) const
{
    int total = 0;
    for (const auto& row : rows_)
        if (row.result == classification)
            ++total;
    return total;
}

bool SonicReport::write(const std::string& directory) const
{
    namespace fs = std::filesystem;
    std::error_code error;
    fs::create_directories(directory, error);
    if (error)
        return false;

    {
        std::ofstream metrics(fs::path(directory) / "sonic_quality_metrics.csv");
        if (!metrics)
            return false;
        metrics << "test_case,family,source_classification,input,parameters,quality,host_rate,"
                   "block_size,commit,reference,observed,tolerance,result,notes\n";
        for (const auto& row : rows_)
        {
            metrics << csvEscape(row.testCase) << ','
                    << csvEscape(row.family) << ','
                    << csvEscape(row.sourceClass) << ','
                    << csvEscape(row.input) << ','
                    << csvEscape(row.parameters) << ','
                    << csvEscape(row.quality) << ','
                    << row.hostRate << ','
                    << row.blockSize << ','
                    << csvEscape(row.commit) << ','
                    << csvEscape(row.reference) << ','
                    << csvEscape(row.observed) << ','
                    << csvEscape(row.tolerance) << ','
                    << classificationName(row.result) << ','
                    << csvEscape(row.notes) << '\n';
        }
    }

    for (const auto& [name, table] : tables_)
    {
        std::ofstream out(fs::path(directory) / name);
        if (!out)
            return false;
        out << table.first << '\n';
        for (const auto& row : table.second)
            out << row << '\n';
    }

    {
        std::ofstream json(fs::path(directory) / "sonic_quality_summary.json");
        if (!json)
            return false;
        json << "{\n"
             << "  \"commit\": \"" << jsonEscape(commit_) << "\",\n"
             << "  \"profile\": \"" << jsonEscape(profile_) << "\",\n"
             << "  \"build\": \"" << jsonEscape(build_) << "\",\n"
             << "  \"pass\": " << count(Classification::pass) << ",\n"
             << "  \"fail\": " << count(Classification::fail) << ",\n"
             << "  \"observed\": " << count(Classification::observed) << ",\n"
             << "  \"needs_review\": " << count(Classification::needsReview) << ",\n"
             << "  \"not_applicable\": " << count(Classification::notApplicable) << ",\n"
             << "  \"rows\": [\n";
        for (size_t i = 0; i < rows_.size(); ++i)
        {
            const auto& row = rows_[i];
            json << "    {\"test_case\":\"" << jsonEscape(row.testCase)
                 << "\",\"family\":\"" << jsonEscape(row.family)
                 << "\",\"result\":\"" << classificationName(row.result)
                 << "\",\"observed\":\"" << jsonEscape(row.observed)
                 << "\",\"reference\":\"" << jsonEscape(row.reference)
                 << "\",\"notes\":\"" << jsonEscape(row.notes) << "\"}";
            if (i + 1 < rows_.size())
                json << ',';
            json << '\n';
        }
        json << "  ]\n}\n";
    }

    std::ofstream markdown(fs::path(directory) / "SONIC_QUALITY_REPORT.md");
    if (!markdown)
        return false;
    markdown << "# SWARA XT sonic quality report\n\n"
             << "Production commit `" << commit_ << "`.\n\n"
             << "Profile: " << profile_ << ". Build: " << build_ << ".\n\n"
             << "This report records measurements. `OBSERVED` and `NEEDS REVIEW` are not confirmed defects. "
             << "A `PASS` is only an explicit invariant or an analytic self-test.\n\n"
             << "| Result | Count |\n|---|---:|\n"
             << "| PASS | " << count(Classification::pass) << " |\n"
             << "| FAIL | " << count(Classification::fail) << " |\n"
             << "| OBSERVED | " << count(Classification::observed) << " |\n"
             << "| NEEDS REVIEW | " << count(Classification::needsReview) << " |\n"
             << "| NOT APPLICABLE | " << count(Classification::notApplicable) << " |\n\n";
    if (count(Classification::fail) > 0)
    {
        markdown << "## Failures\n\n";
        for (const auto& row : rows_)
            if (row.result == Classification::fail)
                markdown << "- `" << row.testCase << "`: " << row.observed
                         << " (reference " << row.reference << "). " << row.notes << "\n";
        markdown << '\n';
    }
    if (count(Classification::needsReview) > 0)
    {
        markdown << "## Needs review\n\n";
        for (const auto& row : rows_)
            if (row.result == Classification::needsReview)
                markdown << "- `" << row.testCase << "`: " << row.observed << ". " << row.notes << "\n";
        markdown << '\n';
    }
    markdown << "## Classification meanings\n\n"
             << "- PASS: analytic self-test or a documented processing invariant.\n"
             << "- FAIL: that self-test or invariant did not hold.\n"
             << "- OBSERVED: measured behavior without an approved sonic limit.\n"
             << "- NEEDS REVIEW: screening bound exceeded, or the reference is missing.\n"
             << "- NOT APPLICABLE: the comparison has no meaningful dry or flat equivalent.\n\n"
             << "Intentional Shruthi integer oscillators, IR3109 OTA harmonics, 12-bit FX "
             << "quantization and the post-DAC Chebyshev stage are reported as observations.\n";
    return true;
}

}  // namespace swaraxt::sonic
