#pragma once

#include "common.h"

#include <ocidl.h>

#include <functional>
#include <utility>

namespace ep {

// Minimal owning COM pointer.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr& o) : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    ComPtr(ComPtr&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    ~ComPtr() { Reset(); }
    ComPtr& operator=(ComPtr o) noexcept {
        std::swap(p_, o.p_);
        return *this;
    }
    void Reset() {
        if (p_) std::exchange(p_, nullptr)->Release();
    }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** Put() {
        Reset();
        return &p_;
    }
    void** PutVoid() { return reinterpret_cast<void**>(Put()); }
    template <typename U>
    ComPtr<U> As() const {
        ComPtr<U> r;
        if (p_) p_->QueryInterface(__uuidof(U), r.PutVoid());
        return r;
    }

private:
    T* p_ = nullptr;
};

// IDispatch event sink that forwards every DISPID to a callback.
// Calls arrive on the agent's STA thread; the callback must not make
// outgoing cross-process calls synchronously (it should only schedule work).
class DispatchSink : public IDispatch {
public:
    DispatchSink(REFIID eventsIid, std::function<void(DISPID)> callback)
        : iid_(eventsIid), callback_(std::move(callback)) {}
    virtual ~DispatchSink() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP GetTypeInfoCount(UINT* count) override {
        *count = 0;
        return S_OK;
    }
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override;

    // Stops forwarding (the remote side may still hold a reference for a while).
    void Disconnect() { callback_ = nullptr; }

private:
    LONG ref_ = 1;
    IID iid_;
    std::function<void(DISPID)> callback_;
};

// An advised connection point; unadvises on Reset/destruction.
class EventConnection {
public:
    EventConnection() = default;
    EventConnection(const EventConnection&) = delete;
    EventConnection& operator=(const EventConnection&) = delete;
    ~EventConnection() { Reset(); }

    bool Connect(IUnknown* source, REFIID eventsIid, std::function<void(DISPID)> callback);
    void Reset();
    bool IsConnectedTo(IUnknown* identity) const { return identity && identity == sourceIdentity_.Get(); }

private:
    ComPtr<IConnectionPoint> point_;
    ComPtr<IUnknown> sourceIdentity_;
    DispatchSink* sink_ = nullptr;
    DWORD cookie_ = 0;
};

ComPtr<IUnknown> Identity(IUnknown* p);

}  // namespace ep
