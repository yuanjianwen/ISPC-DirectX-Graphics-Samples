//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2017, Intel Corporation
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated 
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of 
// the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
// SOFTWARE.
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////// 

#include "stdafx.h"
#include "D3D12nBodyGravity.h"
#include <sstream>

// Concurrency
#include <ppl.h>

// Add the auto generated ISPC kernel header
#include "nBodyGravity_ispc.h"

// InterlockedCompareExchange returns the object's value if the 
// comparison fails.  If it is already 0, then its value won't 
// change and 0 will be returned.
#define InterlockedGetValue(object) InterlockedCompareExchange(object, 0, 0)

const float D3D12nBodyGravity::ParticleSpread = 400.0f;

D3D12nBodyGravity::D3D12nBodyGravity(UINT width, UINT height, std::wstring name) :
    DXSample(width, height, name),
    m_frameIndex(0),
    m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
    m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
    m_rtvDescriptorSize(0),
    m_srvUavDescriptorSize(0),
    m_pConstantBufferGSData(nullptr),
    m_renderContextFenceValue(0),
    m_computeContextFenceValue(0),
    m_srvIndex{},
    m_frameFenceValues{},
    m_bReset(false),
    m_processingType(e_CPU_Vector)
    {
    }

void D3D12nBodyGravity::OnInit()
    {
    m_hardwareThreads = std::thread::hardware_concurrency();
    if (m_hardwareThreads == 0)
        m_hardwareThreads = 4;

    m_camera.Init({ 0.0f, 0.0f, 1500.0f });
    m_camera.SetMoveSpeed(250.0f);

    LoadPipeline();
    LoadAssets();
    CreateComputeContexts();
}

// Load the rendering pipeline dependencies.
void D3D12nBodyGravity::LoadPipeline()
{
    UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
    // Enable the debug layer (requires the Graphics Tools "optional feature").
    // NOTE: Enabling the debug layer after device creation will invalidate the active device.
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();

            // Enable additional debug layers.
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    if (m_useWarpDevice)
    {
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

        ThrowIfFailed(D3D12CreateDevice(
            warpAdapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&m_device)
            ));
    }
    else
    {
        ComPtr<IDXGIAdapter1> hardwareAdapter;
        GetHardwareAdapter(factory.Get(), &hardwareAdapter);

        ThrowIfFailed(D3D12CreateDevice(
            hardwareAdapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&m_device)
            ));
    }

    // Describe and create the command queue.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
    NAME_D3D12_OBJECT(m_commandQueue);

    // Describe and create the swap chain.
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),		// Swap chain needs the queue so that it can force a flush on it.
        Win32Application::GetHwnd(),
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain
        ));

    // This sample does not support fullscreen transitions.
    ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

    ThrowIfFailed(swapChain.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_swapChainEvent = m_swapChain->GetFrameLatencyWaitableObject();

    // Create descriptor heaps.
    {
        // Describe and create a render target view (RTV) descriptor heap.
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = FrameCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

        // Describe and create a shader resource view (SRV) and unordered
        // access view (UAV) descriptor heap.
        D3D12_DESCRIPTOR_HEAP_DESC srvUavHeapDesc = {};
        srvUavHeapDesc.NumDescriptors = DescriptorCount;
        srvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&srvUavHeapDesc, IID_PPV_ARGS(&m_srvUavHeap)));
        NAME_D3D12_OBJECT(m_srvUavHeap);

        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        m_srvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // Create frame resources.
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

        // Create a RTV and a command allocator for each frame.
        for (UINT n = 0; n < FrameCount; n++)
        {
            ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
            m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, m_rtvDescriptorSize);

            NAME_D3D12_OBJECT_INDEXED(m_renderTargets, n);

            ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[n])));
        }
    }
}

// Load the sample assets.
void D3D12nBodyGravity::LoadAssets()
{
    // Create the root signatures.
    {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};

        // This is the highest version the sample supports. If CheckFeatureSupport succeeds, the HighestVersion returned will not be greater than this.
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

        if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
        {
            featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        }

        // Graphics root signature.
        {
            CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
            ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);

            CD3DX12_ROOT_PARAMETER1 rootParameters[GraphicsRootParametersCount];
            rootParameters[GraphicsRootCBV].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_ALL);
            rootParameters[GraphicsRootSRVTable].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_VERTEX);

            CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
            rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

            ComPtr<ID3DBlob> signature;
            ComPtr<ID3DBlob> error;
            ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error));
            ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
            NAME_D3D12_OBJECT(m_rootSignature);
        }

        // Compute root signature.
        {
            CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
            ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
            ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

            CD3DX12_ROOT_PARAMETER1 rootParameters[ComputeRootParametersCount];
            rootParameters[ComputeRootCBV].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_ALL);
            rootParameters[ComputeRootSRVTable].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);
            rootParameters[ComputeRootUAVTable].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_ALL);

            CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
            computeRootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr);

            ComPtr<ID3DBlob> signature;
            ComPtr<ID3DBlob> error;
            ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&computeRootSignatureDesc, featureData.HighestVersion, &signature, &error));
            ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_computeRootSignature)));
            NAME_D3D12_OBJECT(m_computeRootSignature);
        }
    }

    // Create the pipeline states, which includes compiling and loading shaders.
    {
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> geometryShader;
        ComPtr<ID3DBlob> pixelShader;
        ComPtr<ID3DBlob> computeShader;

#if defined(_DEBUG)
        // Enable better shader debugging with the graphics debugging tools.
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        UINT compileFlags = 0;
#endif

        // Load and compile shaders.
        ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"ParticleDraw.hlsl").c_str(), nullptr, nullptr, "VSParticleDraw", "vs_5_0", compileFlags, 0, &vertexShader, nullptr));
        ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"ParticleDraw.hlsl").c_str(), nullptr, nullptr, "GSParticleDraw", "gs_5_0", compileFlags, 0, &geometryShader, nullptr));
        ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"ParticleDraw.hlsl").c_str(), nullptr, nullptr, "PSParticleDraw", "ps_5_0", compileFlags, 0, &pixelShader, nullptr));
        ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"NBodyGravityCS.hlsl").c_str(), nullptr, nullptr, "CSMain", "cs_5_0", compileFlags, 0, &computeShader, nullptr));

        D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
        {
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        // Describe the blend and depth states.
        CD3DX12_BLEND_DESC blendDesc(D3D12_DEFAULT);
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;

        CD3DX12_DEPTH_STENCIL_DESC depthStencilDesc(D3D12_DEFAULT);
        depthStencilDesc.DepthEnable = FALSE;
        depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        // Describe and create the graphics pipeline state object (PSO).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
        psoDesc.GS = CD3DX12_SHADER_BYTECODE(geometryShader.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = blendDesc;
        psoDesc.DepthStencilState = depthStencilDesc;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        psoDesc.SampleDesc.Count = 1;

        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));
        NAME_D3D12_OBJECT(m_pipelineState);

        // Describe and create the compute pipeline state object (PSO).
        D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
        computePsoDesc.pRootSignature = m_computeRootSignature.Get();
        computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.Get());

        ThrowIfFailed(m_device->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&m_computeState)));
        NAME_D3D12_OBJECT(m_computeState);
    }

    // Create the command list.
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[m_frameIndex].Get(), m_pipelineState.Get(), IID_PPV_ARGS(&m_commandList)));
    NAME_D3D12_OBJECT(m_commandList);

    CreateVertexBuffer();
    CreateParticleBuffers();

    // Note: ComPtr's are CPU objects but this resource needs to stay in scope until
    // the command list that references it has finished executing on the GPU.
    // We will flush the GPU at the end of this method to ensure the resource is not
    // prematurely destroyed.
    ComPtr<ID3D12Resource> constantBufferCSUpload;

    // Create the compute shader's constant buffer.
    {
        const UINT bufferSize = sizeof(ConstantBufferCS);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_constantBufferCS)));

        ThrowIfFailed(m_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&constantBufferCSUpload)));

        NAME_D3D12_OBJECT(m_constantBufferCS);

        ConstantBufferCS constantBufferCS = {};
        constantBufferCS.param[0] = ParticleCount;
        constantBufferCS.param[1] = int(ceil(ParticleCount / 128.0f));
        constantBufferCS.paramf[0] = 0.1f;
        constantBufferCS.paramf[1] = 1.0f;

        D3D12_SUBRESOURCE_DATA computeCBData = {};
        computeCBData.pData = reinterpret_cast<UINT8*>(&constantBufferCS);
        computeCBData.RowPitch = bufferSize;
        computeCBData.SlicePitch = computeCBData.RowPitch;

        UpdateSubresources<1>(m_commandList.Get(), m_constantBufferCS.Get(), constantBufferCSUpload.Get(), 0, 0, 1, &computeCBData);
        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_constantBufferCS.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
    }

    // Create the geometry shader's constant buffer.
    {
        const UINT constantBufferGSSize = sizeof(ConstantBufferGS) * FrameCount;

        ThrowIfFailed(m_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(constantBufferGSSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_constantBufferGS)
            ));

        NAME_D3D12_OBJECT(m_constantBufferGS);

        CD3DX12_RANGE readRange(0, 0);		// We do not intend to read from this resource on the CPU.
        ThrowIfFailed(m_constantBufferGS->Map(0, &readRange, reinterpret_cast<void**>(&m_pConstantBufferGSData)));
        ZeroMemory(m_pConstantBufferGSData, constantBufferGSSize);
    }

    // Close the command list and execute it to begin the initial GPU setup.
    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // Create synchronization objects and wait until assets have been uploaded to the GPU.
    {
        ThrowIfFailed(m_device->CreateFence(m_renderContextFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_renderContextFence)));
        m_renderContextFenceValue++;

        m_renderContextFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_renderContextFenceEvent == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }

        WaitForRenderContext();
    }
}

// Create the particle vertex buffer.
void D3D12nBodyGravity::CreateVertexBuffer()
{
    std::vector<ParticleVertex> vertices;
    vertices.resize(ParticleCount);
    for (UINT i = 0; i < ParticleCount; i++)
    {
        vertices[i].color = XMFLOAT4(1.0f, 1.0f, 0.2f, 1.0f);
    }
    const UINT bufferSize = ParticleCount * sizeof(ParticleVertex);

    ThrowIfFailed(m_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_vertexBuffer)));

    ThrowIfFailed(m_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_vertexBufferUpload)));

    NAME_D3D12_OBJECT(m_vertexBuffer);

    D3D12_SUBRESOURCE_DATA vertexData = {};
    vertexData.pData = reinterpret_cast<UINT8*>(&vertices[0]);
    vertexData.RowPitch = bufferSize;
    vertexData.SlicePitch = vertexData.RowPitch;

    UpdateSubresources<1>(m_commandList.Get(), m_vertexBuffer.Get(), m_vertexBufferUpload.Get(), 0, 0, 1, &vertexData);
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.SizeInBytes = static_cast<UINT>(bufferSize);
    m_vertexBufferView.StrideInBytes = sizeof(ParticleVertex);
}

// Random percent value, from -1 to 1.
float D3D12nBodyGravity::RandomPercent()
{
    float ret = static_cast<float>((rand() % 10000) - 5000);
    return ret / 5000.0f;
}

void D3D12nBodyGravity::LoadParticles(_Out_writes_(numParticles) Particle* pParticles, const XMFLOAT3& center, const XMFLOAT4& velocity, float spread, UINT numParticles)
{
    srand(0);
    for (UINT i = 0; i < numParticles; i++)
    {
        XMFLOAT3 delta(spread, spread, spread);

        while (XMVectorGetX(XMVector3LengthSq(XMLoadFloat3(&delta))) > spread * spread)
        {
            delta.x = RandomPercent() * spread;
            delta.y = RandomPercent() * spread;
            delta.z = RandomPercent() * spread;
        }

        pParticles[i].position.x = center.x + delta.x;
        pParticles[i].position.y = center.y + delta.y;
        pParticles[i].position.z = center.z + delta.z;
        pParticles[i].position.w = 10000.0f * 10000.0f;

        pParticles[i].velocity = velocity;
    }
}

// Create the position and velocity buffer shader resources.
void D3D12nBodyGravity::CreateParticleBuffers()
{
    const UINT dataSize = ParticleCount * sizeof(Particle);

    D3D12_HEAP_PROPERTIES defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize);

    // Initialize the data in the buffers.
    m_particlesISPC0.resize(ParticleCount);
    m_particlesISPC1.resize(ParticleCount);

    // Split the particles into two groups.
    float centerSpread = ParticleSpread * 0.50f;
    LoadParticles(&m_particlesISPC0[0], XMFLOAT3(centerSpread, 0, 0), XMFLOAT4(0, 0, -20, 1 / 100000000.0f), ParticleSpread, ParticleCount / 2);
    LoadParticles(&m_particlesISPC0[ParticleCount / 2], XMFLOAT3(-centerSpread, 0, 0), XMFLOAT4(0, 0, 20, 1 / 100000000.0f), ParticleSpread, ParticleCount / 2);

        // Create two buffers in the GPU, each with a copy of the particles data.
        // The compute shader will update one of them while the rendering thread 
        // renders the other. When rendering completes, the threads will swap 
        // which buffer they work on.
        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
        IID_PPV_ARGS(&m_particleBuffer0)));

        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
        IID_PPV_ARGS(&m_particleBuffer1)));

        ThrowIfFailed(m_device->CreateCommittedResource(
            &uploadHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &uploadBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
        IID_PPV_ARGS(&m_particleBuffer0Upload)));

        ThrowIfFailed(m_device->CreateCommittedResource(
            &uploadHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &uploadBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
        IID_PPV_ARGS(&m_particleBuffer1Upload)));

    NAME_D3D12_OBJECT(m_particleBuffer0);
    NAME_D3D12_OBJECT(m_particleBuffer1);

        D3D12_SUBRESOURCE_DATA particleData = {};
    particleData.pData = reinterpret_cast<UINT8*>(&m_particlesISPC0[0]);
        particleData.RowPitch = dataSize;
        particleData.SlicePitch = particleData.RowPitch;

    UpdateSubresources<1>(m_commandList.Get(), m_particleBuffer0.Get(), m_particleBuffer0Upload.Get(), 0, 0, 1, &particleData);
    UpdateSubresources<1>(m_commandList.Get(), m_particleBuffer1.Get(), m_particleBuffer1Upload.Get(), 0, 0, 1, &particleData);
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_particleBuffer0.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_particleBuffer1.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = ParticleCount;
        srvDesc.Buffer.StructureByteStride = sizeof(Particle);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle0(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), SrvParticlePosVelo0, m_srvUavDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle1(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), SrvParticlePosVelo1, m_srvUavDescriptorSize);
    m_device->CreateShaderResourceView(m_particleBuffer0.Get(), &srvDesc, srvHandle0);
    m_device->CreateShaderResourceView(m_particleBuffer1.Get(), &srvDesc, srvHandle1);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = ParticleCount;
        uavDesc.Buffer.StructureByteStride = sizeof(Particle);
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle0(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), UavParticlePosVelo0, m_srvUavDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle1(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), UavParticlePosVelo1, m_srvUavDescriptorSize);
    m_device->CreateUnorderedAccessView(m_particleBuffer0.Get(), nullptr, &uavDesc, uavHandle0);
    m_device->CreateUnorderedAccessView(m_particleBuffer1.Get(), nullptr, &uavDesc, uavHandle1);
    }

//
// When changing compute type, we reload the buffers for cleanliness
//
void D3D12nBodyGravity::ReloadParticleBuffers()
{
    const UINT dataSize = ParticleCount * sizeof(Particle);

    // Reset the main command allocator/list
    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), m_pipelineState.Get()));

    // Split the particles into two groups.
    float centerSpread = ParticleSpread * 0.50f;
    LoadParticles(&m_particlesISPC0[0], XMFLOAT3(centerSpread, 0, 0), XMFLOAT4(0, 0, -20, 1 / 100000000.0f), ParticleSpread, ParticleCount / 2);
    LoadParticles(&m_particlesISPC0[ParticleCount / 2], XMFLOAT3(-centerSpread, 0, 0), XMFLOAT4(0, 0, 20, 1 / 100000000.0f), ParticleSpread, ParticleCount / 2);

    D3D12_SUBRESOURCE_DATA particleData = {};
    particleData.pData = reinterpret_cast<UINT8*>(&m_particlesISPC0[0]);
    particleData.RowPitch = dataSize;
    particleData.SlicePitch = particleData.RowPitch;

    // upload
    UpdateSubresources<1>(m_commandList.Get(), m_particleBuffer0.Get(), m_particleBuffer0Upload.Get(), 0, 0, 1, &particleData);
    UpdateSubresources<1>(m_commandList.Get(), m_particleBuffer1.Get(), m_particleBuffer1Upload.Get(), 0, 0, 1, &particleData);
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_particleBuffer0.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_particleBuffer1.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

    // Close the command list and execute.
    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // Block the CPU until done
    WaitForRenderContext();

    // reset the srvIndex;
    m_srvIndex = 0;
}

void D3D12nBodyGravity::CreateComputeContexts()
    {
        // Create compute resources.
        D3D12_COMMAND_QUEUE_DESC queueDesc = { D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE };
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_computeCommandQueue)));
    for (int ii = 0; ii < FrameCount; ii++)
        {
        ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&m_computeAllocator[ii])));
        ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, m_computeAllocator[ii].Get(), nullptr, IID_PPV_ARGS(&m_computeCommandList)));
        }
    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_computeContextFence)));
}

// Update frame-based values.
void D3D12nBodyGravity::OnUpdate()
{
    // Wait for the previous Present to complete.
    WaitForSingleObjectEx(m_swapChainEvent, 100, FALSE);

    static double old_second = -2;
    static double elapsed_seconds = 0.0;
    static int elapsed_frames = 0;

    elapsed_seconds += m_timer.GetElapsedSeconds();
    elapsed_frames++;

    //
    // Update title/frame rate every second
    //
    if (m_timer.GetTotalSeconds() > (old_second + 1))
    {

        double ms = elapsed_seconds * 1000.0 / (double)elapsed_frames;
        double fps = (double)elapsed_frames / elapsed_seconds;

        std::wstringstream title;
        switch (m_processingType)
        {
        case e_CPU_Vector:
            title << "(CPU ISPC Compute Kernel, " << m_hardwareThreads << " threads) : ";
            break;
        case e_CPU_Scalar:
            title << "(CPU Scalar C++ Code, " << m_hardwareThreads << " threads) : ";
            break;
        case e_GPU:
            title << "(GPU Async Compute) : ";
            break;
        }

        //
        // Note that the timer has a max time delta of 1000ms, so if
        // the application runs slower than 1fps, the reported frame times
        // will be incorrect.
        //
        title << ms << " ms, " << fps << " fps.  [press SPACE to change compute type]";

        SetCustomWindowText(title.str().c_str());

        old_second = m_timer.GetTotalSeconds();
        elapsed_seconds = 0.0;
        elapsed_frames = 0;
    }

    m_timer.Tick(NULL);
    m_camera.Update(static_cast<float>(m_timer.GetElapsedSeconds()));

    ConstantBufferGS constantBufferGS = {};
    XMStoreFloat4x4(&constantBufferGS.worldViewProjection, XMMatrixMultiply(m_camera.GetViewMatrix(), m_camera.GetProjectionMatrix(0.8f, m_aspectRatio, 1.0f, 5000.0f)));
    XMStoreFloat4x4(&constantBufferGS.inverseView, XMMatrixInverse(nullptr, m_camera.GetViewMatrix()));

    UINT8* destination = m_pConstantBufferGSData + sizeof(ConstantBufferGS) * m_frameIndex;
    memcpy(destination, &constantBufferGS, sizeof(ConstantBufferGS));
}

// Render the scene.
void D3D12nBodyGravity::OnRender()
{
    PIXBeginEvent(m_commandQueue.Get(), 0, L"Simulate");

    //
    // If changing compute types, reload the particles
    //
    if (m_bReset)
    {
        ReloadParticleBuffers();
        m_bReset = false;
    }

    //
    // Run the particle simulation.
    //
    switch (m_processingType)
    {
    case e_CPU_Scalar:
    case e_CPU_Vector:
        SimulateCPU();
        break;
    case e_GPU:
        SimulateGPU();
        break;
        }

    // Close and execute the command list.
    ThrowIfFailed(m_computeCommandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_computeCommandList.Get() };

    PIXBeginEvent(m_computeCommandQueue.Get(), 0, L"Iterate on the particle simulation");
    m_computeCommandQueue->ExecuteCommandLists(1, ppCommandLists);
    PIXEndEvent(m_computeCommandQueue.Get());

    // Wait for the compute shader to complete the simulation.
    m_computeContextFenceValue++;
    ThrowIfFailed(m_computeCommandQueue->Signal(m_computeContextFence.Get(), m_computeContextFenceValue));

    // Swap the indices to the SRV and UAV.
    m_srvIndex = 1 - m_srvIndex;

    // Prepare for the next frame.
    ThrowIfFailed(m_computeAllocator[m_frameIndex]->Reset());
    ThrowIfFailed(m_computeCommandList->Reset(m_computeAllocator[m_frameIndex].Get(), m_computeState.Get()));

    PIXBeginEvent(m_commandQueue.Get(), 0, L"Render");

    // Instruct the rendering command queue to wait for the current 
    // compute work to complete.
    ThrowIfFailed(m_commandQueue->Wait(m_computeContextFence.Get(), m_computeContextFenceValue));

    // Record all the commands we need to render the scene into the command list.
    PopulateCommandList();

    // Execute the command list.
    ID3D12CommandList* ppCommandLists2[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists2), ppCommandLists2);

    PIXEndEvent(m_commandQueue.Get());

    // Present the frame.
    // Ignore VSync
    ThrowIfFailed(m_swapChain->Present(0, 0));

    MoveToNextFrame();
}

// Fill the command list with all the render commands and dependent state.
void D3D12nBodyGravity::PopulateCommandList()
{
    // Command list allocators can only be reset when the associated
    // command lists have finished execution on the GPU; apps should use
    // fences to determine GPU execution progress.
    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());

    // However, when ExecuteCommandList() is called on a particular command
    // list, that command list can then be reset at any time and must be before
    // re-recording.
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), m_pipelineState.Get()));

    // Set necessary state.
    m_commandList->SetPipelineState(m_pipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    m_commandList->SetGraphicsRootConstantBufferView(GraphicsRootCBV, m_constantBufferGS->GetGPUVirtualAddress() + m_frameIndex * sizeof(ConstantBufferGS));

    ID3D12DescriptorHeap* ppHeaps[] = { m_srvUavHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);

    // Indicate that the back buffer will be used as a render target.
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescriptorSize);
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Record commands.
    const float clearColor[] = { 0.0f, 0.0f, 0.1f, 0.0f };
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // Render the particles.
    const UINT srvIndex = m_srvIndex == 0 ? SrvParticlePosVelo0 : SrvParticlePosVelo1;

        CD3DX12_VIEWPORT viewport(
        0.0f, 
        0.0f,
        m_viewport.Width,
        m_viewport.Height);

        m_commandList->RSSetViewports(1, &viewport);

        CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(m_srvUavHeap->GetGPUDescriptorHandleForHeapStart(), srvIndex, m_srvUavDescriptorSize);
        m_commandList->SetGraphicsRootDescriptorTable(GraphicsRootSRVTable, srvHandle);

    PIXBeginEvent(m_commandList.Get(), 0, L"Draw particles");
        m_commandList->DrawInstanced(ParticleCount, 1, 0, 0);
        PIXEndEvent(m_commandList.Get());
    

    m_commandList->RSSetViewports(1, &m_viewport);

    // Indicate that the back buffer will now be used to present.
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    ThrowIfFailed(m_commandList->Close());
}

//
// Run the particle simulation using the compute shader.
//
void D3D12nBodyGravity::SimulateGPU()
{
    ID3D12GraphicsCommandList* pCommandList = m_computeCommandList.Get();

    UINT srvIndex;
    UINT uavIndex;
    ID3D12Resource *pUavResource;
    if (m_srvIndex == 0)
    {
        srvIndex = SrvParticlePosVelo0;
        uavIndex = UavParticlePosVelo1;
        pUavResource = m_particleBuffer1.Get();
    }
    else
    {
        srvIndex = SrvParticlePosVelo1;
        uavIndex = UavParticlePosVelo0;
        pUavResource = m_particleBuffer0.Get();
    }

    pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(pUavResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

    pCommandList->SetPipelineState(m_computeState.Get());
    pCommandList->SetComputeRootSignature(m_computeRootSignature.Get());

    ID3D12DescriptorHeap* ppHeaps[] = { m_srvUavHeap.Get() };
    pCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(m_srvUavHeap->GetGPUDescriptorHandleForHeapStart(), srvIndex, m_srvUavDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE uavHandle(m_srvUavHeap->GetGPUDescriptorHandleForHeapStart(), uavIndex, m_srvUavDescriptorSize);

    pCommandList->SetComputeRootConstantBufferView(ComputeRootCBV, m_constantBufferCS->GetGPUVirtualAddress());
    pCommandList->SetComputeRootDescriptorTable(ComputeRootSRVTable, srvHandle);
    pCommandList->SetComputeRootDescriptorTable(ComputeRootUAVTable, uavHandle);

    pCommandList->Dispatch(static_cast<int>(ceil(ParticleCount / 128.0f)), 1, 1);

    pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(pUavResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
}

//
// Run the simulation on the CPU - either vectorised using ISPC or scalar using straight C/C++ code.
//
void D3D12nBodyGravity::SimulateCPU()
{
    //
    // ISPC and the Scalar code both assume the particle count divides by 8.
    // This is because 1 of the loops is unrolled. 
    // Simpler to keep this constraint than worry about mopping up excess particles.
    //
    assert((ParticleCount % 8) == 0);

    ID3D12GraphicsCommandList* pCommandList = m_computeCommandList.Get();

    UINT srvIndex;
    UINT uavIndex;
    std::vector<Particle> * pReadParticles;
    std::vector<Particle> * pWriteParticles;
    ID3D12Resource *pUavResource;
    ID3D12Resource *pUploadResource;
    if (m_srvIndex == 0)
    {
        srvIndex = SrvParticlePosVelo0;
        uavIndex = UavParticlePosVelo1;
        pReadParticles = &(m_particlesISPC0);
        pWriteParticles = &(m_particlesISPC1);
        pUavResource = m_particleBuffer1.Get();
        pUploadResource = m_particleBuffer1Upload.Get();
    }
    else
    {
        srvIndex = SrvParticlePosVelo1;
        uavIndex = UavParticlePosVelo0;
        pReadParticles = &(m_particlesISPC1);
        pWriteParticles = &(m_particlesISPC0);
        pUavResource = m_particleBuffer0.Get();
        pUploadResource = m_particleBuffer0Upload.Get();
    }

    int parallelParticleCount = ParticleCount / m_hardwareThreads;
    ispc::Particle * pRead = (ispc::Particle *)&(*pReadParticles)[0];
    ispc::Particle * pWrite = (ispc::Particle *)&(*pWriteParticles)[0];

    // 
    // Keep a copy of the particle data in system memory, double buffered to work on.
    // Process this data and upload to the render buffer once finished.
    //
    concurrency::parallel_for<uint32_t>(0, m_hardwareThreads, [&](uint32_t parallelThreadID)
    {
        switch (m_processingType)
        {
        case e_CPU_Scalar:
            ProcessParticles(parallelThreadID * parallelParticleCount, parallelParticleCount, pReadParticles, pWriteParticles);
            break;
        case e_CPU_Vector:
            ispc::ProcessParticles(parallelThreadID * parallelParticleCount, parallelParticleCount, pRead, pWrite, ParticleCount);
            break;
        }
    });

    //
    // Upload to the render buffer
    //
    D3D12_SUBRESOURCE_DATA particleData = {};
    particleData.pData = reinterpret_cast<UINT8*>(&(*pWriteParticles)[0]);
    particleData.RowPitch = ParticleCount * sizeof(Particle);
    particleData.SlicePitch = particleData.RowPitch;

    pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(pUavResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
    UpdateSubresources<1>(pCommandList, pUavResource, pUploadResource, 0, 0, 1, &particleData);
    pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(pUavResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

}

// 
// Fast Recip Sqrt
//
// https://en.wikipedia.org/wiki/Fast_inverse_square_root
//
__inline float Q_rsqrt(float number)
{
    long i;
    float x2, y;
    const float threehalfs = 1.5F;

    x2 = number * 0.5F;
    y = number;
    i = *(long *)&y;                       // evil floating point bit level hacking
    i = 0x5f3759df - (i >> 1);
    y = *(float *)&i;
    y = y * (threehalfs - (x2 * y * y));   // 1st iteration
    //	y  = y * ( threehalfs - ( x2 * y * y ) );   // 2nd iteration, this can be removed
    return y;
}

__inline void bodyBodyInteraction(
    XMFLOAT3 &accel, 
    XMFLOAT4 thatPos, 
    XMFLOAT4 thisPos)
{
    const float softeningSquared = 0.0000015625f;
    const float g_fParticleMass = 66.73f;
        
    XMFLOAT3 r;
    r.x = thatPos.x - thisPos.x;
    r.y = thatPos.y - thisPos.y;
    r.z = thatPos.z - thisPos.z;

    float distSqr = (r.x * r.x) + (r.y * r.y) + (r.z * r.z);
    distSqr += softeningSquared;

    float invDist = Q_rsqrt(distSqr);
    float invDistCube = invDist * invDist * invDist;

    float s = g_fParticleMass * invDistCube;

    accel.x += r.x * s;
    accel.y += r.y * s;
    accel.z += r.z * s;
}

//
// Run the full simulation in C/C++ scalar code
//
void D3D12nBodyGravity::ProcessParticles(uint32_t particleStart, uint32_t particleCount, std::vector<Particle> * pReadParticles, std::vector<Particle> * pWriteParticles)
{
    const float timeStepDelta = 0.1f;
    const float g_fG = 6.67300e-11f * 10000.0f;
    const float g_fParticleMass = g_fG * 10000.0f * 10000.0f;

    uint32_t particleEnd = particleStart + particleCount;
    if (particleEnd > static_cast<uint32_t>(ParticleCount))
        particleEnd = ParticleCount;

    for (uint32_t ii = particleStart; ii < particleEnd; ii++)
    {
        XMFLOAT3 accel = { 0.0f, 0.0f, 0.0f };
        XMFLOAT4 pos = (*pReadParticles)[ii].position;

        // Better performance by not unrolling this loop
        for (uint32_t jj = 0; jj < static_cast<uint32_t>(ParticleCount); jj ++)
        {
            bodyBodyInteraction(accel, (*pReadParticles)[jj + 0].position, pos);
        }

        XMFLOAT4 vel = (*pReadParticles)[ii].velocity;

        // Update the velocity and position of current particle using the 
        // acceleration computed above.
        vel.x += accel.x * timeStepDelta;
        vel.y += accel.y * timeStepDelta;
        vel.z += accel.z * timeStepDelta;
        vel.w = 1.0f / Q_rsqrt((accel.x * accel.x) + (accel.y * accel.y) + (accel.z * accel.z));

        pos.x += vel.x * timeStepDelta;
        pos.y += vel.y * timeStepDelta;
        pos.z += vel.z * timeStepDelta;

        (*pWriteParticles)[ii].position = pos;
        (*pWriteParticles)[ii].velocity = vel;
    }
}

void D3D12nBodyGravity::OnDestroy()
{
    // Ensure that the GPU is no longer referencing resources that are about to be
    // cleaned up by the destructor.
    WaitForRenderContext();

    // Close handles to fence events and threads.
    CloseHandle(m_renderContextFenceEvent);
}

void D3D12nBodyGravity::OnKeyDown(UINT8 key)
{
    m_camera.OnKeyDown(key);

    switch (key)
    {
    case VK_SPACE:
        // toggle the modes
        m_bReset = true;
        m_processingType = (ProcessingType)(((int)m_processingType + 1) % e_MAX_ProcessingType);
        break;
    }

}

void D3D12nBodyGravity::OnKeyUp(UINT8 key)
{
    m_camera.OnKeyUp(key);
}

void D3D12nBodyGravity::WaitForRenderContext()
{
    // Add a signal command to the queue.
    ThrowIfFailed(m_commandQueue->Signal(m_renderContextFence.Get(), m_renderContextFenceValue));

    // Instruct the fence to set the event object when the signal command completes.
    ThrowIfFailed(m_renderContextFence->SetEventOnCompletion(m_renderContextFenceValue, m_renderContextFenceEvent));
    m_renderContextFenceValue++;

    // Wait until the signal command has been processed.
    WaitForSingleObject(m_renderContextFenceEvent, INFINITE);
}

// Cycle through the frame resources. This method blocks execution if the 
// next frame resource in the queue has not yet had its previous contents 
// processed by the GPU.
void D3D12nBodyGravity::MoveToNextFrame()
{
    // Assign the current fence value to the current frame.
    m_frameFenceValues[m_frameIndex] = m_renderContextFenceValue;

    // Signal and increment the fence value.
    ThrowIfFailed(m_commandQueue->Signal(m_renderContextFence.Get(), m_renderContextFenceValue));
    m_renderContextFenceValue++;

    // Update the frame index.
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // If the next frame is not ready to be rendered yet, wait until it is ready.
    if (m_renderContextFence->GetCompletedValue() < m_frameFenceValues[m_frameIndex])
    {
        ThrowIfFailed(m_renderContextFence->SetEventOnCompletion(m_frameFenceValues[m_frameIndex], m_renderContextFenceEvent));
        WaitForSingleObject(m_renderContextFenceEvent, INFINITE);
    }
}
