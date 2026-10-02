#include "comutil.h"

namespace ep {

STDMETHODIMP DispatchSink::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IDispatch || riid == iid_) {
        *ppv = static_cast<IDispatch*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DispatchSink::Release() {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP DispatchSink::Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) {
    if (callback_) callback_(id);
    return S_OK;
}

bool EventConnection::Connect(IUnknown* source, REFIID eventsIid, std::function<void(DISPID)> callback) {
    Reset();
    if (!source) return false;
    ComPtr<IConnectionPointContainer> container;
    if (FAILED(source->QueryInterface(IID_PPV_ARGS(container.Put())))) return false;
    ComPtr<IConnectionPoint> point;
    if (FAILED(container->FindConnectionPoint(eventsIid, point.Put()))) return false;
    auto* sink = new DispatchSink(eventsIid, std::move(callback));
    DWORD cookie = 0;
    HRESULT hr = point->Advise(sink, &cookie);
    if (FAILED(hr)) {
        sink->Disconnect();
        sink->Release();
        return false;
    }
    point_ = point;
    sink_ = sink;
    cookie_ = cookie;
    sourceIdentity_ = Identity(source);
    return true;
}

void EventConnection::Reset() {
    if (point_ && cookie_) point_->Unadvise(cookie_);  // fails harmlessly if the source is gone
    if (sink_) {
        sink_->Disconnect();
        sink_->Release();
        sink_ = nullptr;
    }
    cookie_ = 0;
    point_.Reset();
    sourceIdentity_.Reset();
}

ComPtr<IUnknown> Identity(IUnknown* p) {
    ComPtr<IUnknown> id;
    if (p) p->QueryInterface(IID_PPV_ARGS(id.Put()));
    return id;
}

}  // namespace ep
