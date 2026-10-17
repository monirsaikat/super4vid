#define CT_LOG_CATEGORY "image"
#include <ct/platform/image_io.h>

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/platform/win.h>

#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace ct::platform {

namespace {

HRESULT WritePng(const wchar_t* path, const uint8_t* rgba, int width, int height, int stride) {
    ComPtr<IWICImagingFactory> factory;
    CT_RETURN_IF_FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())));
    ComPtr<IWICStream> stream;
    CT_RETURN_IF_FAILED(factory->CreateStream(stream.GetAddressOf()));
    CT_RETURN_IF_FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    CT_RETURN_IF_FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()));
    CT_RETURN_IF_FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    CT_RETURN_IF_FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr));
    CT_RETURN_IF_FAILED(frame->Initialize(nullptr));
    CT_RETURN_IF_FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    CT_RETURN_IF_FAILED(frame->SetPixelFormat(&format));
    if (format == GUID_WICPixelFormat32bppRGBA) {
        CT_RETURN_IF_FAILED(frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(stride),
                                               static_cast<UINT>(stride) * static_cast<UINT>(height), const_cast<BYTE*>(rgba)));
    } else {
        // The encoder wants another layout: convert through a WIC bitmap.
        ComPtr<IWICBitmap> bitmap;
        CT_RETURN_IF_FAILED(factory->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height),
                                                            GUID_WICPixelFormat32bppRGBA, static_cast<UINT>(stride),
                                                            static_cast<UINT>(stride) * static_cast<UINT>(height),
                                                            const_cast<BYTE*>(rgba), bitmap.GetAddressOf()));
        ComPtr<IWICFormatConverter> conv;
        CT_RETURN_IF_FAILED(factory->CreateFormatConverter(conv.GetAddressOf()));
        CT_RETURN_IF_FAILED(conv->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
        CT_RETURN_IF_FAILED(frame->WriteSource(conv.Get(), nullptr));
    }
    CT_RETURN_IF_FAILED(frame->Commit());
    CT_RETURN_IF_FAILED(encoder->Commit());
    return S_OK;
}

} // namespace

bool ImageIo::WritePngRgba(const char* path_utf8, const uint8_t* rgba, int width, int height, int stride) {
    if (!path_utf8 || !rgba || width <= 0 || height <= 0 || stride < width * 4) return false;
    const HRESULT hr = WritePng(Utf8ToWide(path_utf8).c_str(), rgba, width, height, stride);
    if (FAILED(hr)) CT_LOG_ERROR("WritePngRgba(%s) failed: %s", path_utf8, HrToString(hr).c_str());
    return SUCCEEDED(hr);
}

} // namespace ct::platform
