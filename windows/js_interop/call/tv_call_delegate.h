#pragma once

class TVCall;
struct TVError;
struct TVCallStatus;

class TVCallDelegate {
public:
    virtual ~TVCallDelegate() = default;
    
    virtual void onCallAccept(TVCall* call) = 0;
    virtual void onCallCancel(TVCall* call) = 0;
    virtual void onCallDisconnect(TVCall* call) = 0;
    virtual void onCallError(const TVError& error) = 0;
    virtual void onCallReconnecting(const TVError& error) = 0;
    virtual void onCallReconnected() = 0;
    virtual void onCallReject() = 0;
    virtual void onCallStatus(const TVCallStatus& status) = 0;
};
