#define CT_LOG_CATEGORY "image"
#include <ct/media/image_io.h>

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>

#include <wincodec.h>

#include <string>

using Microsoft::WRL::ComPtr;

namespace ct::media {

namespace {

HRESULT Factory(ComPtr<IWICImagingFactory>& f) {
    return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.GetAddressOf()));
}

HRESULT Save(std::string_view path, const GUID& container, const GUID& want_format, const uint8_t* bgra, int w, int h,
             int stride, float jpeg_quality) {
    if (!bgra || w <= 0 || h <= 0 || stride < w * 4) return E_INVALIDARG;
    ComPtr<IWICImagingFactory> factory;
    CT_RETURN_IF_FAILED(Factory(factory));
    ComPtr<IWICStream> stream;
    CT_RETURN_IF_FAILED(factory->CreateStream(stream.GetAddressOf()));
    const std::wstring wpath = Utf8ToWide(path);
    CT_RETURN_IF_FAILED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    CT_RETURN_IF_FAILED(factory->CreateEncoder(container, nullptr, encoder.GetAddressOf()));
    CT_RETURN_IF_FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    CT_RETURN_IF_FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), props.GetAddressOf()));
    if (jpeg_quality >= 0 && props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<wchar_t*>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = jpeg_quality < 0 ? 0 : (jpeg_quality > 1 ? 1 : jpeg_quality);
        CT_LOG_IF_FAILED(props->Write(1, &opt, &v));
    }
    CT_RETURN_IF_FAILED(frame->Initialize(props.Get()));
    CT_RETURN_IF_FAILED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)));
    WICPixelFormatGUID format = want_format;
    CT_RETURN_IF_FAILED(frame->SetPixelFormat(&format));
    ComPtr<IWICBitmap> bitmap;
    CT_RETURN_IF_FAILED(factory->CreateBitmapFromMemory(static_cast<UINT>(w), static_cast<UINT>(h), GUID_WICPixelFormat32bppBGRA,
                                                        static_cast<UINT>(stride), static_cast<UINT>(stride) * static_cast<UINT>(h),
                                                        const_cast<BYTE*>(bgra), bitmap.GetAddressOf()));
    if (format == GUID_WICPixelFormat32bppBGRA) {
        CT_RETURN_IF_FAILED(frame->WriteSource(bitmap.Get(), nullptr));
    } else {
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

HRESULT ImageIo::LoadRgba(std::string_view path, std::vector<uint8_t>& rgba, SizeI& size) {
    ComPtr<IWICImagingFactory> factory;
    CT_RETURN_IF_FAILED(Factory(factory));
    ComPtr<IWICBitmapDecoder> decoder;
    const std::wstring wpath = Utf8ToWide(path);
    HRESULT hr = factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                    decoder.GetAddressOf());
    if (FAILED(hr)) {
        CT_LOG_WARN("ImageIo: cannot open %.*s: %s", static_cast<int>(path.size()), path.data(), HrToString(hr).c_str());
        return hr;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    CT_RETURN_IF_FAILED(decoder->GetFrame(0, frame.GetAddressOf()));
    ComPtr<IWICFormatConverter> conv;
    CT_RETURN_IF_FAILED(factory->CreateFormatConverter(conv.GetAddressOf()));
    CT_RETURN_IF_FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom));
    UINT w = 0, h = 0;
    CT_RETURN_IF_FAILED(conv->GetSize(&w, &h));
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    rgba.resize(static_cast<size_t>(w) * h * 4);
    CT_RETURN_IF_FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()), rgba.data()));
    size = {static_cast<int32_t>(w), static_cast<int32_t>(h)};
    return S_OK;
}

HRESULT ImageIo::LoadToTexture(platform::D3DDevice& dev, std::string_view path, ComPtr<ID3D11Texture2D>& tex, SizeI& size) {
    std::vector<uint8_t> px;
    CT_RETURN_IF_FAILED(LoadRgba(path, px, size));
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(size.w);
    d.Height = static_cast<UINT>(size.h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px.data(), static_cast<UINT>(size.w) * 4, 0};
    tex.Reset();
    CT_RETURN_IF_FAILED(dev.Device()->CreateTexture2D(&d, &init, tex.GetAddressOf()));
    return S_OK;
}

HRESULT ImageIo::SavePng(std::string_view path, const uint8_t* bgra, int w, int h, int stride) {
    const HRESULT hr = Save(path, GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA, bgra, w, h, stride, -1.0f);
    if (FAILED(hr)) CT_LOG_WARN("ImageIo::SavePng(%.*s): %s", static_cast<int>(path.size()), path.data(), HrToString(hr).c_str());
    return hr;
}

HRESULT ImageIo::SaveJpeg(std::string_view path, const uint8_t* bgra, int w, int h, int stride, float quality) {
    const HRESULT hr = Save(path, GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, bgra, w, h, stride, quality);
    if (FAILED(hr)) CT_LOG_WARN("ImageIo::SaveJpeg(%.*s): %s", static_cast<int>(path.size()), path.data(), HrToString(hr).c_str());
    return hr;
}

} // namespace ct::media
