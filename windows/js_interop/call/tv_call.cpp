#include "tv_call.h"
#include "tv_error.h"
#include "tv_call_status.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

TVCall::TVCall(const std::wstring& jsObjectName, TVWebView* webView)
    : JSObject(jsObjectName, webView), callDelegate_(nullptr) {
}

TVCall::~TVCall() {
    detachEventListeners();
}

void TVCall::isMuted(std::function<void(bool, std::string)> completionHandler) {
    call(L"isMuted", {}, [completionHandler](void* result, std::string error) {
        if (!error.empty()) {
            completionHandler(false, error);
            return;
        }
        bool* muted = static_cast<bool*>(result);
        completionHandler(*muted, "");
    });
}

void TVCall::mute(bool shouldMute, std::function<void(std::string)> completionHandler) {
    call(L"mute", {shouldMute ? L"true" : L"false"}, 
        [completionHandler](void*, std::string error) {
            completionHandler(error);
        });
}

void TVCall::attachEventListeners() {
    const std::vector<std::wstring> events = {
        L"accept", L"cancel", L"disconnect", L"error",
        L"reconnecting", L"reconnected", L"reject", L"status"
    };
    
    for (const auto& event : events) {
        auto callback = [this](void* data, std::string error) {
            if (!error.empty() || !data) {
                return;
            }
            const wchar_t* wstr = static_cast<const wchar_t*>(data);
            handleEventCallback(std::wstring(wstr));
        };
        
        addEventListener(event, callback);
    }
}

void TVCall::detachEventListeners() {
    const std::vector<std::wstring> events = {
        L"accept", L"cancel", L"disconnect", L"error",
        L"reconnecting", L"reconnected", L"reject", L"status"
    };
    
    for (const auto& event : events) {
        removeEventListener(event);
    }
}

void TVCall::handleEventCallback(const std::wstring& eventData) {
    try {
        // Convert wstring to string properly
        std::string utf8Data;
        utf8Data.reserve(eventData.length());
        for (wchar_t wc : eventData) {
            utf8Data += static_cast<char>(wc & 0xFF);
        }
        
        json j = json::parse(utf8Data);
        if (j.contains("event") && j.contains("data")) {
            std::string eventType = j["event"].get<std::string>();
            auto& data = j["data"];
            
            if (eventType == "error" || eventType == "reconnecting") {
                TVError error;
                error.code = data["code"].get<std::string>();
                error.message = data["message"].get<std::string>();
                error.description = data["description"].get<std::string>();
                
                if (eventType == "error") {
                    onCallError(error);
                } else {
                    onCallReconnecting(error);
                }
            } else if (eventType == "status") {
                TVCallStatus status;
                status.status = data["status"].get<std::string>();
                status.callSid = data["callSid"].get<std::string>();
                status.isMuted = data["isMuted"].get<bool>();
                onCallStatus(status);
            }
            // ...handle other events...
        }
    } catch(const json::exception&) {
        // Handle JSON parse error
    }
}

void TVCall::onCallAccept(TVCall* call) {
    if (callDelegate_) callDelegate_->onCallAccept(call);
}

void TVCall::onCallCancel(TVCall* call) {
    if (callDelegate_) callDelegate_->onCallCancel(call);
}

void TVCall::onCallDisconnect(TVCall* call) {
    if (callDelegate_) callDelegate_->onCallDisconnect(call);
}

void TVCall::onCallError(const TVError& error) {
    if (callDelegate_) callDelegate_->onCallError(error);
}

void TVCall::onCallReconnecting(const TVError& error) {
    if (callDelegate_) callDelegate_->onCallReconnecting(error);
}

void TVCall::onCallReconnected() {
    if (callDelegate_) callDelegate_->onCallReconnected();
}

void TVCall::onCallReject() {
    if (callDelegate_) callDelegate_->onCallReject();
}

void TVCall::onCallStatus(const TVCallStatus& status) {
    if (callDelegate_) callDelegate_->onCallStatus(status);
}

// Implement other TVCallDelegate methods similarly...
