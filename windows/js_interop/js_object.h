#pragma once
#include "../webview/tv_webview.h"
#include <string>
#include <functional>

class JSObject {
public:
    JSObject(const std::wstring& jsObjectName, TVWebView* webView);
    virtual ~JSObject();

protected:
    void call(const std::wstring& method, 
             const std::vector<std::wstring>& args,
             std::function<void(void*, std::string)> completionHandler);
             
    void addEventListener(const std::wstring& event,
                        std::function<void(void*, std::string)> completionHandler);
                        
    void removeEventListener(const std::wstring& event);

private:
    std::wstring jsObjectName_;
    TVWebView* webView_;
};
