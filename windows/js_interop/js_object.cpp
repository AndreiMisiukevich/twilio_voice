#include "js_object.h"
#include <sstream>

JSObject::JSObject(const std::wstring& jsObjectName, TVWebView* webView)
    : jsObjectName_(jsObjectName), webView_(webView) {
}

JSObject::~JSObject() = default;

void JSObject::call(const std::wstring& method,
                   const std::vector<std::wstring>& args,
                   std::function<void(void*, std::string)> completionHandler) {
    std::wstringstream ss;
    ss << jsObjectName_ << "." << method << "(";
    
    for (size_t i = 0; i < args.size(); ++i) {
        ss << args[i];
        if (i < args.size() - 1) ss << ", ";
    }
    ss << ")";
    
    webView_->evaluateJavaScript(ss.str(), completionHandler);
}

void JSObject::addEventListener(const std::wstring& event,
                             std::function<void(void*, std::string)> completionHandler) {
    std::wstringstream ss;
    ss << jsObjectName_ << ".on('" << event << "', function(data) { "
       << "window.chrome.webview.postMessage({event: '" << event << "', data: data}); })";
    
    webView_->evaluateJavaScript(ss.str(), completionHandler);
}

void JSObject::removeEventListener(const std::wstring& event) {
    std::wstringstream ss;
    ss << jsObjectName_ << ".off('" << event << "')";
    
    webView_->evaluateJavaScript(ss.str(), [](void*, std::string){});
}
