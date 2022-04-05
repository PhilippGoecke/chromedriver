// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/test/chromedriver/chrome/frame_tracker.h"

#include <utility>

#include "base/json/json_writer.h"
#include "base/values.h"
#include "chrome/test/chromedriver/chrome/browser_info.h"
#include "chrome/test/chromedriver/chrome/devtools_client.h"
#include "chrome/test/chromedriver/chrome/status.h"
#include "chrome/test/chromedriver/chrome/web_view_impl.h"

FrameTracker::FrameTracker(DevToolsClient* client,
                           WebView* web_view,
                           const BrowserInfo* browser_info)
    : web_view_(web_view) {
  client->AddListener(this);
}

FrameTracker::~FrameTracker() {}

Status FrameTracker::GetContextIdForFrame(
    const std::string& frame_id, int* context_id) {
  if (frame_to_context_map_.count(frame_id) == 0) {
    return Status(kNoSuchExecutionContext,
                  "frame does not have execution context");
  }
  *context_id = frame_to_context_map_[frame_id];
  return Status(kOk);
}

WebView* FrameTracker::GetTargetForFrame(const std::string& frame_id) {
  // Context in the current target, return current target.
  if (frame_to_context_map_.count(frame_id) != 0)
    return web_view_;
  // Child target of the current target, return that child target.
  if (frame_to_target_map_.count(frame_id) != 0)
    return frame_to_target_map_[frame_id].get();
  // Frame unknown, recursively search all child targets.
  for (auto it = frame_to_target_map_.begin(); it != frame_to_target_map_.end();
       ++it) {
    FrameTracker* child = it->second->GetFrameTracker();
    if (child != nullptr) {
      WebView* child_result = child->GetTargetForFrame(frame_id);
      if (child_result != nullptr)
        return child_result;
    }
  }
  return nullptr;
}

bool FrameTracker::IsKnownFrame(const std::string& frame_id) const {
  if (attached_frames_.count(frame_id) != 0 ||
      frame_to_context_map_.count(frame_id) != 0 ||
      frame_to_target_map_.count(frame_id) != 0) {
    return true;
  }
  // Frame unknown to this tracker, recursively search all child targets.
  for (const auto& it : frame_to_target_map_) {
    FrameTracker* child = it.second->GetFrameTracker();
    if (child != nullptr && child->IsKnownFrame(frame_id))
      return true;
  }
  return false;
}

void FrameTracker::DeleteTargetForFrame(const std::string& frame_id) {
  frame_to_target_map_.erase(frame_id);
}

Status FrameTracker::OnConnected(DevToolsClient* client) {
  frame_to_context_map_.clear();
  frame_to_target_map_.clear();
  attached_frames_.clear();
  // Enable target events to allow tracking iframe targets creation.
  base::DictionaryValue params;
  base::Value::Dict& dict = params.GetDict();
  dict.Set("autoAttach", true);
  dict.Set("flatten", true);
  dict.Set("waitForDebuggerOnStart", false);
  Status status = client->SendCommand("Target.setAutoAttach", params);
  if (status.IsError())
    return status;
  // Enable runtime events to allow tracking execution context creation.
  params.DictClear();
  status = client->SendCommand("Runtime.enable", params);
  if (status.IsError())
    return status;
  return client->SendCommand("Page.enable", params);
}

Status FrameTracker::OnEvent(DevToolsClient* client,
                             const std::string& method,
                             const base::DictionaryValue& params) {
  if (method == "Runtime.executionContextCreated") {
    const base::DictionaryValue* context;
    if (!params.GetDictionary("context", &context)) {
      return Status(kUnknownError,
                    "Runtime.executionContextCreated missing dict 'context'");
    }

    std::string frame_id;
    bool is_default = true;

    absl::optional<int> context_id = context->GetDict().FindInt("id");
    if (!context_id) {
      std::string json;
      base::JSONWriter::Write(*context, &json);
      return Status(kUnknownError, method + " has invalid 'context': " + json);
    }

    if (const base::Value* auxData = context->GetDict().Find("auxData")) {
      if (!auxData->is_dict()) {
        return Status(kUnknownError, method + " has invalid 'auxData' value");
      }
      if (absl::optional<bool> b = auxData->GetDict().FindBool("isDefault")) {
        is_default = *b;
      } else {
        return Status(kUnknownError, method + " has invalid 'isDefault' value");
      }
      if (const std::string* s = auxData->GetDict().FindString("frameId")) {
        frame_id = *s;
      } else {
        return Status(kUnknownError, method + " has invalid 'frameId' value");
      }
    }

    if (is_default && !frame_id.empty())
      frame_to_context_map_[frame_id] = *context_id;
  } else if (method == "Runtime.executionContextDestroyed") {
    absl::optional<int> execution_context_id =
        params.GetDict().FindInt("executionContextId");
    if (!execution_context_id)
      return Status(kUnknownError, method + " missing 'executionContextId'");
    for (auto entry : frame_to_context_map_) {
      if (entry.second == *execution_context_id) {
        frame_to_context_map_.erase(entry.first);
        break;
      }
    }
  } else if (method == "Runtime.executionContextsCleared") {
    frame_to_context_map_.clear();
  } else if (method == "Page.frameAttached") {
    if (const std::string* frame_id = params.GetDict().FindString("frameId")) {
      attached_frames_.insert(*frame_id);
    } else {
      return Status(kUnknownError,
                    "missing frameId in Page.frameAttached event");
    }
  } else if (method == "Page.frameDetached") {
    if (const std::string* frame_id = params.GetDict().FindString("frameId")) {
      attached_frames_.erase(*frame_id);
    } else {
      return Status(kUnknownError,
                    "missing frameId in Page.frameDetached event");
    }
  } else if (method == "Page.frameNavigated") {
    if (!params.FindPath("frame.parentId"))
      frame_to_context_map_.clear();
  } else if (method == "Target.attachedToTarget") {
    const std::string* type = params.FindStringPath("targetInfo.type");
    if (!type)
      return Status(kUnknownError,
                    "missing target type in Target.attachedToTarget event");
    if (*type == "iframe" || *type == "page") {
      const std::string* target_id =
          params.FindStringPath("targetInfo.targetId");
      if (!target_id)
        return Status(kUnknownError,
                      "missing target ID in Target.attachedToTarget event");
      const std::string* session_id = params.GetDict().FindString("sessionId");
      if (!session_id)
        return Status(kUnknownError,
                      "missing session ID in Target.attachedToTarget event");
      if (frame_to_target_map_.count(*target_id) > 0) {
        // Since chrome 70 we are seeing multiple Target.attachedToTarget events
        // for the same target_id.  This is causing crashes because:
        // - replacing the value in frame_to_target_map_ is causing the
        //   pre-existing one to be disposed
        // - if there are in-flight requests for the disposed DevToolsClient
        //   then chromedriver is crashing in the ProcessNextMessage processing
        // - the in-flight messages observed were DOM.getDocument requests from
        //   DomTracker.
        // The fix is to not replace an pre-existing frame_to_target_map_ entry.
      } else {
        std::unique_ptr<WebViewImpl> child(
            static_cast<WebViewImpl*>(web_view_)->CreateChild(*session_id,
                                                              *target_id));
        WebViewImplHolder child_holder(child.get());
        frame_to_target_map_[*target_id] = std::move(child);
        frame_to_target_map_[*target_id]->SetUpDevTools();
      }
    }
  } else if (method == "Target.detachedFromTarget") {
    const std::string* target_id = params.GetDict().FindString("targetId");
    if (!target_id)
      // Some types of Target.detachedFromTarget events do not have targetId.
      // We are not interested in those types of targets.
      return Status(kOk);
    auto target_iter = frame_to_target_map_.find(*target_id);
    if (target_iter == frame_to_target_map_.end())
      // There are some target types that we're not keeping track of, thus not
      // finding the target in frame_to_target_map_ is OK.
      return Status(kOk);
    WebViewImpl* target = static_cast<WebViewImpl*>(target_iter->second.get());
    if (target->IsLocked())
      target->SetDetached();
    else
      frame_to_target_map_.erase(*target_id);
  }
  return Status(kOk);
}
