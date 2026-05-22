#ifndef UPLOAD_BUFFER_HPP
#define UPLOAD_BUFFER_HPP

#include <wrl.h>
#include <d3d12.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "Dx12Common.hpp"

template<typename T>
class UploadBuffer
{
public:
    UploadBuffer(ID3D12Device* device, UINT elementCount, bool isConstantBuffer)
        : m_isConstantBuffer(isConstantBuffer)
    {
        m_elementByteSize = sizeof(T);

        if (isConstantBuffer)
            m_elementByteSize = CalcConstantBufferByteSize(m_elementByteSize);

        const UINT64 bufferSize = (UINT64)m_elementByteSize * elementCount;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = bufferSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(m_uploadBuffer.GetAddressOf())
        ));

        ThrowIfFailed(m_uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedData)));
    }

    UploadBuffer(const UploadBuffer&) = delete;
    UploadBuffer& operator=(const UploadBuffer&) = delete;

    ~UploadBuffer()
    {
        if (m_uploadBuffer)
            m_uploadBuffer->Unmap(0, nullptr);
        m_mappedData = nullptr;
    }

    ID3D12Resource* Resource() const { return m_uploadBuffer.Get(); }

    void CopyData(int elementIndex, const T& data)
    {
        std::memcpy(m_mappedData + (size_t)elementIndex * m_elementByteSize, &data, sizeof(T));
    }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuffer;
    uint8_t* m_mappedData = nullptr;

    UINT m_elementByteSize = 0;
    bool m_isConstantBuffer = false;
};

#endif // !UPLOAD_BUFFER_HPP
