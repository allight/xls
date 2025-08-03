// Copyright 2025 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/passes/tools/passes_profile.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/log/log.h"
#include "absl/time/time.h"
#include "cppitertools/reversed.hpp"
#include "xls/common/file/filesystem.h"
#include "xls/common/module_initializer.h"
#include "xls/common/profile.pb.h"

ABSL_FLAG(std::optional<std::string>, passes_profile, std::nullopt,
          "If set the file to write a passes pprof file to. This is only "
          "written on a normal exit"
          " of the process (the file is created in an atexit(3) handler).");
namespace xls {

namespace {
constexpr int64_t kSampleTy = 0;

class ProfileState;
class ProfileEntry {
 public:
  ProfileEntry(std::string_view name, ProfileEntry* owner)
      : name_(name), owner_(owner), changed_(false), start_(absl::Now()) {}

  ProfileEntry* Enter(std::string_view name) {
    return &children_.emplace_back(name, this);
  }
  ProfileEntry* Exit(bool changed) {
    changed_ = changed;
    end_ = absl::Now();
    finished_ = true;
    return owner_;
  }
  void RecordAnnotation(std::string_view key,
                        std::variant<std::string_view, int64_t> contents) {
    std::variant<std::string, int64_t> owned;
    if (std::holds_alternative<std::string_view>(contents)) {
      owned = std::string(std::get<std::string_view>(contents));
    } else {
      owned = std::get<int64_t>(contents);
    }
    annotations_[key] = std::move(owned);
  }
  // Add this entry and children into the profile. The reverse_location_stack
  // must have the same entries at return as it had on call. It contains the
  // indexes for the locations we already visited. The id is exactly the same as
  // the string id for the short name.
  template <typename InternStr, typename InternLoc>
    requires(std::is_invocable_r_v<int64_t, InternStr, std::string_view> &&
             std::is_invocable_r_v<int64_t, InternLoc, std::string_view>)
  void RecordToProto(perftools::profiles::Profile& prof,
                     std::vector<int64_t>& reverse_location_stack,
                     InternStr str_id, InternLoc loc_id) const {
    int64_t thiz_id = loc_id(name_);
    perftools::profiles::Sample* samp = prof.add_sample();
    auto add_label = [&](std::string_view name,
                         const std::variant<std::string, int64_t>& val) {
      auto* label = samp->add_label();
      label->set_key(str_id(name));
      if (std::holds_alternative<int64_t>(val)) {
        label->set_num(std::get<int64_t>(val));
      } else {
        label->set_str(str_id(std::get<std::string>(val)));
      }
    };
    add_label("changed", changed_ ? "true" : "false");
    auto* changed_label = samp->add_label();
    changed_label->set_key(str_id("changed"));
    changed_label->set_str(str_id(changed_ ? "true" : "false"));
    // count sample
    samp->add_value(1);
    // calculate time sample.
    auto tot = end_ - start_;
    for (const auto& child : children_) {
      tot -= (child.end_ - child.start_);
    }

    // add time sample
    samp->add_value(absl::ToInt64Nanoseconds(tot));

    // If leaf add changes and unchanged.
    if (children_.empty()) {
      samp->add_value(changed_ ? 1 : 0);
      samp->add_value(changed_ ? 0 : 1);
    } else {
      samp->add_value(0);
      samp->add_value(0);
    }


    samp->add_location_id(thiz_id);
    for (auto loc : iter::reversed(reverse_location_stack)) {
      samp->add_location_id(loc);
    }
    if (!annotations_.contains(pass_profile::kCompound)) {
      add_label(pass_profile::kCompound, "false");
      add_label(pass_profile::kFixedpoint, "false");
    }
    add_label("finished", finished_ ? "true" : "false");
    for (const auto& [key, val] : annotations_) {
      add_label(key, val);
    }
    // Invalidates pointers so do last.
    reverse_location_stack.push_back(thiz_id);
    for (const auto& child : children_) {
      child.RecordToProto(prof, reverse_location_stack, str_id, loc_id);
    }
    reverse_location_stack.pop_back();
  }

 private:
  std::string name_;
  ProfileEntry* owner_;
  bool changed_;
  absl::Time start_;
  absl::Time end_;
  std::vector<ProfileEntry> children_;
  absl::flat_hash_map<std::string, std::variant<std::string, int64_t>>
      annotations_;
  bool finished_ = false;

  friend class ProfileState;
};
class ProfileState {
 public:
  void RecordPassEntry(std::string_view name) {
    if (!absl::GetFlag(FLAGS_passes_profile)) {
      return;
    }
    bottom_entry_ = bottom_entry_->Enter(name);
  }

  void ExitPass(bool changed) {
    if (!absl::GetFlag(FLAGS_passes_profile)) {
      return;
    }
    bottom_entry_ = bottom_entry_->Exit(changed);
  }
  void RecordPassAnnotation(std::string_view key,
                            std::variant<std::string_view, int64_t> contents) {
    bottom_entry_->RecordAnnotation(key, contents);
  }
  perftools::profiles::Profile Serialize() {
    perftools::profiles::Profile res;
    absl::flat_hash_map<std::string, int64_t> str_table;
    absl::flat_hash_set<std::string> seen_names;
    auto str_id = [&](std::string_view sv) -> int64_t {
      auto entry = str_table.try_emplace(sv, res.string_table_size());
      *res.add_string_table() = std::string(sv);
      return entry.first->second;
    };
    auto loc_id = [&](std::string_view sv) -> int64_t {
      if (seen_names.contains(sv)) {
        return str_id(sv);
      }
      seen_names.insert(std::string(sv));
      // Basically use the short-name (as the str-id) for everything.
      auto* loc = res.add_location();
      auto id = str_id(sv);
      loc->set_id(id);
      auto line = loc->add_line();
      line->set_function_id(id);
      auto func = res.add_function();
      func->set_id(id);
      func->set_name(id);
      return id;
    };
    // ensure "" is 0.
    str_id("");
    // 0 sample type is pass invocations.
    {
      auto* st = res.add_sample_type();
      st->set_type(str_id("pass_invocations"));
      st->set_type(str_id("count"));
    }
    // 1 sample type duration
    {
      auto* st = res.add_sample_type();
      st->set_type(str_id("cpu"));
      st->set_type(str_id("nanoseconds"));
    }
    // 2 leaves with changes.
    {
      auto* st = res.add_sample_type();
      st->set_type(str_id("changed"));
      st->set_type(str_id("changed_runs"));
    }
    // 2 leaves without changes.
    {
      auto* st = res.add_sample_type();
      st->set_type(str_id("unchanged"));
      st->set_type(str_id("unchanged_runs"));
    }
    res.set_default_sample_type(kSampleTy);
    res.set_period(1);
    res.add_comment(str_id(
        R"explanation(This profile counts the number and duration of pass invocations.)explanation"));
    res.set_doc_url(str_id("<todo github url>"));
    if (top_entry_->children_.empty()) {
      return res;
    }
    res.set_time_nanos(absl::ToUnixNanos(top_entry_->children_.front().start_));
    res.set_duration_nanos(
        absl::ToInt64Nanoseconds(top_entry_->children_.front().end_ -
                                 top_entry_->children_.front().start_));
    std::vector<int64_t> locs;
    top_entry_->children_.front().RecordToProto(res, locs, str_id, loc_id);
    return res;
  }

 private:
  std::unique_ptr<ProfileEntry> top_entry_ =
      std::make_unique<ProfileEntry>("profile-start", nullptr);
  ProfileEntry* bottom_entry_ = top_entry_.get();
};

ProfileState the_state;

void WritePprofFile() {
  if (!absl::GetFlag(FLAGS_passes_profile)) {
    return;
  }
  auto out = the_state.Serialize();
  std::string serialized;
  bool changed = out.SerializeToString(&serialized);
  if (!changed) {
    return;
  }
  auto res =
      SetFileContents(*absl::GetFlag(FLAGS_passes_profile), serialized);
  if (!res.ok()) {
    LOG(ERROR) << "Failed to write file " << res.ToString();
  }
}

}  // namespace

void RecordPassEntry(std::string_view short_name) {
  the_state.RecordPassEntry(short_name);
}

void RecordPassAnnotation(std::string_view key,
                          std::variant<std::string_view, int64_t> contents) {
  the_state.RecordPassAnnotation(key, contents);
}
void ExitPass(bool changed) { the_state.ExitPass(changed); }

XLS_REGISTER_MODULE_INITIALIZER(pass_profile_saver, { atexit(WritePprofFile); })

}  // namespace xls