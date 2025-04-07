#pragma once
#include "../js_object.h"
#include "tv_call_delegate.h"
#include <wrl.h>
#include <WebView2.h>

class TVWebView;

class TVCall : public JSObject, public TVCallDelegate {
public:
    TVCall(const std::wstring& jsObjectName, TVWebView* webView);
    virtual ~TVCall();

    void isMuted(std::function<void(bool, std::string)> completionHandler);
    void mute(bool shouldMute, std::function<void(std::string)> completionHandler);
    void sendDigits(const std::string& digits, std::function<void(std::string)> completionHandler);
    void accept(std::function<void(std::string)> completionHandler);
    void disconnect(std::function<void(std::string)> completionHandler);
    
    void attachEventListeners();
    void detachEventListeners();

    // TVCallDelegate implementation
    void onCallAccept(TVCall* call);
    void onCallCancel(TVCall* call);
    void onCallDisconnect(TVCall* call);
    void onCallError(const TVError& error);
    void onCallReconnecting(const TVError& error);
    void onCallReconnected();
    void onCallReject();
    void onCallStatus(const TVCallStatus& status);

    void setDelegate(TVCallDelegate* delegate) { callDelegate_ = delegate; }
    TVCallDelegate* getDelegate() const { return callDelegate_; }

private:
    void handleEventCallback(const std::wstring& eventData);
    TVCallDelegate* callDelegate_;
};
