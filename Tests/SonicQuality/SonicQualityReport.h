// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "SonicQuality/SonicQualityThresholds.h"

#include <map>
#include <string>
#include <vector>

namespace swaraxt::sonic {

class SonicReport
{
public:
    void setContext(std::string commit, std::string profile, std::string build) ;
    void add(Measurement row);
    void addTableRow(const std::string& table, const std::string& header, const std::string& row);
    int failures() const noexcept { return failures_; }
    int count(Classification classification) const;
    bool write(const std::string& directory) const;
    const std::vector<Measurement>& rows() const noexcept { return rows_; }

private:
    std::string commit_;
    std::string profile_;
    std::string build_;
    std::vector<Measurement> rows_;
    std::map<std::string, std::pair<std::string, std::vector<std::string>>> tables_;
    int failures_ = 0;
};

}  // namespace swaraxt::sonic
