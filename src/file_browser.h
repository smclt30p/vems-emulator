#pragma once
#include "imgui.h"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Minimal modal file picker. Call open(), then draw() each frame; returns true once
// when the user picks a file, with the path in out_path.
class FileBrowser {
public:
    void open(const std::string& start_dir = "", const std::string& title = "Open firmware") {
        if (!start_dir.empty() && fs::exists(start_dir)) cwd_ = fs::path(start_dir);
        else if (cwd_.empty()) cwd_ = fs::current_path();
        title_ = title;
        selected_.clear();
        refresh_ = true;
        request_open_ = true;
    }

    bool draw(std::string& out_path) {
        bool picked = false;
        if (request_open_) { ImGui::OpenPopup(title_.c_str()); request_open_ = false; }

        ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal(title_.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            if (refresh_) { scan(); refresh_ = false; }

            ImGui::TextUnformatted(cwd_.string().c_str());
            ImGui::Separator();

            ImGui::BeginChild("list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2), true);
            if (cwd_.has_parent_path() && cwd_ != cwd_.root_path()) {
                if (ImGui::Selectable("..", false, ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::IsMouseDoubleClicked(0)) { cwd_ = cwd_.parent_path(); refresh_ = true; selected_.clear(); }
                }
            }
            for (auto& e : entries_) {
                std::string label = (e.is_dir ? "[D] " : "    ") + e.name;
                bool sel = (!e.is_dir && e.name == selected_);
                if (ImGui::Selectable(label.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (e.is_dir) {
                        if (ImGui::IsMouseDoubleClicked(0)) { cwd_ /= e.name; refresh_ = true; selected_.clear(); }
                    } else {
                        selected_ = e.name;
                        if (ImGui::IsMouseDoubleClicked(0)) { out_path = (cwd_ / selected_).string(); picked = true; ImGui::CloseCurrentPopup(); }
                    }
                }
            }
            ImGui::EndChild();

            ImGui::Checkbox("Only .hex / .bin", &filter_); ImGui::SameLine();
            if (ImGui::Button("Refresh")) refresh_ = true;

            ImGui::Text("Selected: %s", selected_.empty() ? "(none)" : selected_.c_str());
            ImGui::BeginDisabled(selected_.empty());
            if (ImGui::Button("Open", ImVec2(120, 0))) {
                out_path = (cwd_ / selected_).string(); picked = true; ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }
        return picked;
    }

private:
    struct Entry { std::string name; bool is_dir; };

    void scan() {
        entries_.clear();
        std::error_code ec;
        for (auto& it : fs::directory_iterator(cwd_, fs::directory_options::skip_permission_denied, ec)) {
            std::string name = it.path().filename().string();
            if (!name.empty() && name[0] == '.') continue;
            bool dir = it.is_directory(ec);
            if (!dir && filter_) {
                std::string ext = it.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".hex" && ext != ".bin" && ext != ".uhex") continue;
            }
            entries_.push_back({name, dir});
        }
        std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            return a.name < b.name;
        });
    }

    fs::path cwd_;
    std::vector<Entry> entries_;
    std::string selected_;
    bool refresh_ = false;
    bool request_open_ = false;
    bool filter_ = true;
    std::string title_ = "Open firmware";
};
