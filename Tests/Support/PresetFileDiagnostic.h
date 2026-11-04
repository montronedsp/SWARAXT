// SPDX-FileCopyrightText: 2026 MontroneDSP
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if JUCE_WINDOWS && !defined(SWARA_CONTROL_BASELINE)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>

namespace presetFileDiagnostic {
inline bool enabled() { return swaraxt::test::environment("SWARA_PRESET_FILE_DIAGNOSTIC") != nullptr; }
inline juce::var file(const juce::File& path) {
    auto* value = new juce::DynamicObject;
    value->setProperty("path", path.getFullPathName());
    SetLastError(ERROR_SUCCESS);
    const auto attributes = GetFileAttributesW(path.getFullPathName().toWideCharPointer());
    const auto error = GetLastError();
    value->setProperty("attributes", static_cast<juce::int64>(attributes));
    value->setProperty("attributes_error", static_cast<int>(error));
    value->setProperty("reparse_point", attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
    WIN32_FIND_DATAW info{};
    const auto handle = FindFirstFileW(path.getFullPathName().toWideCharPointer(), &info);
    if (handle != INVALID_HANDLE_VALUE) { value->setProperty("reparse_tag", static_cast<juce::int64>(info.dwReserved0)); FindClose(handle); }
    value->setProperty("exists", path.exists());
    value->setProperty("is_symbolic_link", path.isSymbolicLink());
    value->setProperty("linked_target", path.getLinkedTarget().getFullPathName());
    juce::MemoryBlock bytes;
    value->setProperty("bytes_read", path.loadFileAsData(bytes));
    value->setProperty("bytes_base64", bytes.toBase64Encoding());
    value->setProperty("byte_count", static_cast<int>(bytes.getSize()));
    return juce::var(value);
}
inline void emit(const char* phase, const juce::var& value) {
    std::printf("PRESET_FILE_DIAGNOSTIC %s %s\n", phase, juce::JSON::toString(value, true).toRawUTF8());
}
struct Capture {
    juce::File directory;
    juce::Array<juce::File> temporaries;
    static void hook(void* context, SwaraXtAudioProcessor::StatePublicationPoint point) {
        if (point != SwaraXtAudioProcessor::StatePublicationPoint::userPresetTemporaryWritten) return;
        auto& capture = *static_cast<Capture*>(context);
        capture.directory.findChildFiles(capture.temporaries, juce::File::findFiles, false, "*.tmp");
        for (const auto& temporary : capture.temporaries) emit("temporary_before_publication", file(temporary));
    }
};
inline void probe(const juce::File& directory, const char* operation) {
    const auto target = directory.getChildFile(juce::String(operation) + "-outside");
    const auto link = directory.getChildFile(juce::String(operation) + "-link");
    const auto temporary = directory.getChildFile(juce::String(operation) + "-complete.tmp");
    if (!target.replaceWithText("outside sentinel") || !temporary.replaceWithText("complete replacement payload") || !target.createSymbolicLink(link, false)) {
        emit("probe_setup_failed", operation); return;
    }
    emit("probe_link_before", file(link)); emit("probe_outside_before", file(target)); emit("probe_temporary_before", file(temporary));
    SetLastError(ERROR_SUCCESS);
    bool result = false;
    std::error_code ec;
    if (juce::String(operation) == "ReplaceFileW")
        result = ReplaceFileW(link.getFullPathName().toWideCharPointer(), temporary.getFullPathName().toWideCharPointer(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS | 4, nullptr, nullptr) != FALSE;
    else if (juce::String(operation) == "MoveFileExW")
        result = MoveFileExW(temporary.getFullPathName().toWideCharPointer(), link.getFullPathName().toWideCharPointer(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    else {
        std::filesystem::rename(std::filesystem::u8path(temporary.getFullPathName().toRawUTF8()), std::filesystem::u8path(link.getFullPathName().toRawUTF8()), ec);
        result = !ec;
    }
    const auto error = GetLastError();
    auto* record = new juce::DynamicObject;
    record->setProperty("operation", operation); record->setProperty("result", result);
    record->setProperty("get_last_error", static_cast<int>(error));
    record->setProperty("error_code", ec.value()); record->setProperty("error_message", juce::String(ec.message()));
    emit("operation", juce::var(record));
    emit("probe_link_after", file(link)); emit("probe_outside_after", file(target)); emit("probe_temporary_after", file(temporary));
}
}
#endif
