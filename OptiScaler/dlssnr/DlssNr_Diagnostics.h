#pragma once

#include <SysUtils.h>
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>

// Diagnostic branch only. DRED is enabled before device creation; this does not enable
// the D3D12 validation layer, wait for the GPU, or alter NR's resource retirement policy.
namespace DlssNr::Diagnostics
{
inline void EnableDred()
{
    static std::once_flag once;
    std::call_once(once, [] {
        ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
        const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
        if (FAILED(hr) || settings == nullptr)
        {
            LOG_ERROR("NR-DIAG DRED settings unavailable: 0x{:08X}", (UINT) hr);
            return;
        }
        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        ID3D12DeviceRemovedExtendedDataSettings1* contexts = nullptr;
        if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&contexts))))
        {
            contexts->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            contexts->Release();
        }
        settings->Release();
        LOG_INFO("NR-DIAG dred-v1: DRED enabled before device creation; heap slots=512; "
                 "D3D12 validation layer is not enabled by this diagnostic");
    });
}

// Fixed-size CPU recording history, dumped only after device loss. These events are
// NOT GPU completion markers. Keep pointers as values: never dereference them at dump time.
struct Event
{
    UINT64 sequence = 0;
    ULONGLONG time = 0;
    DWORD thread = 0;
    const char* stage = ""; // Callers must use string literals.
    const void* cmd = nullptr;
    const void* a = nullptr;
    const void* b = nullptr;
    const void* c = nullptr;
    const void* d = nullptr;
    UINT64 detail = 0;
};
inline std::mutex eventMutex;
inline std::array<Event, 128> events {};
inline UINT64 eventSequence = 0;

inline void Record(const char* stage, const void* cmd = nullptr, const void* a = nullptr,
                   const void* b = nullptr, const void* c = nullptr, const void* d = nullptr,
                   UINT64 detail = 0)
{
    std::lock_guard lock(eventMutex);
    const UINT64 sequence = ++eventSequence;
    events[(sequence - 1) % events.size()] =
        { sequence, GetTickCount64(), GetCurrentThreadId(), stage, cmd, a, b, c, d, detail };
}

inline void DumpHistory()
{
    std::array<Event, 128> snapshot;
    UINT64 count;
    {
        std::lock_guard lock(eventMutex);
        snapshot = events;
        count = eventSequence;
    }
    LOG_ERROR("NR-DIAG CPU recording history (not evidence of GPU completion):");
    const UINT64 first = count > snapshot.size() ? count - snapshot.size() : 0;
    for (UINT64 i = first; i < count; ++i)
    {
        const auto& e = snapshot[i % snapshot.size()];
        LOG_ERROR("NR-DIAG CPU #{} ms={} tid={} {} cmd={} a={} b={} c={} d={} detail={}",
                  e.sequence, e.time, e.thread, e.stage, e.cmd, e.a, e.b, e.c, e.d, e.detail);
    }
}

inline void Name(ID3D12Object* object, const std::wstring& label)
{
    if (object == nullptr)
        return;
    static std::atomic<UINT64> serial { 0 };
    const auto name = L"DLSS-NR/" + label + L"/#" + std::to_wstring(++serial);
    object->SetName(name.c_str());
}

inline std::string DebugName(const char* ansi, const wchar_t* wide)
{
    if (ansi != nullptr)
        return ansi;
    if (wide == nullptr)
        return "<unnamed>";
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return "<unreadable name>";
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

inline const char* OpName(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op)
    {
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINQUERY: return "BeginQuery";
    case D3D12_AUTO_BREADCRUMB_OP_ENDQUERY: return "EndQuery";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND: return "ExecuteMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
    default: return "other (see numeric D3D12_AUTO_BREADCRUMB_OP)";
    }
}

inline void DumpAllocations(const char* category, const D3D12_DRED_ALLOCATION_NODE1* node)
{
    UINT count = 0;
    for (; node != nullptr && count < 128; node = node->pNext, ++count)
        LOG_ERROR("NR-DIAG allocation {} type={} object={} name={}", category,
                  (UINT) node->AllocationType, (const void*) node->pObject,
                  DebugName(node->ObjectNameA, node->ObjectNameW));
    LOG_ERROR("NR-DIAG {} allocation matches={} truncated={}", category, count, node != nullptr);
}

inline void DumpDeviceLoss(ID3D12Device* device)
{
    if (device == nullptr)
        return;
    const HRESULT reason = device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason))
        return;
    static std::mutex dumpMutex;
    static std::unordered_set<ID3D12Device*> reported;
    // A second rendering thread must not wait behind the crash-report writer.
    std::unique_lock lock(dumpMutex, std::try_to_lock);
    if (!lock.owns_lock() || !reported.insert(device).second)
        return;

    LOG_ERROR("NR-DIAG BEGIN DEVICE LOSS device={} reason=0x{:08X}", (void*) device, (UINT) reason);
    DumpHistory();
    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    const HRESULT query = device->QueryInterface(IID_PPV_ARGS(&dred));
    if (FAILED(query) || dred == nullptr)
    {
        LOG_ERROR("NR-DIAG DRED interface unavailable: 0x{:08X}", (UINT) query);
    }
    else
    {
        D3D12_DRED_PAGE_FAULT_OUTPUT1 fault {};
        const HRESULT faultResult = dred->GetPageFaultAllocationOutput1(&fault);
        LOG_ERROR("NR-DIAG page-fault query=0x{:08X} VA=0x{:016X}", (UINT) faultResult, fault.PageFaultVA);
        if (SUCCEEDED(faultResult))
        {
            DumpAllocations("existing", fault.pHeadExistingAllocationNode);
            DumpAllocations("recently-freed", fault.pHeadRecentFreedAllocationNode);
        }

        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs {};
        const HRESULT crumbResult = dred->GetAutoBreadcrumbsOutput1(&crumbs);
        LOG_ERROR("NR-DIAG breadcrumb query=0x{:08X}", (UINT) crumbResult);
        UINT visited = 0, incomplete = 0;
        auto* node = crumbs.pHeadAutoBreadcrumbNode;
        for (; SUCCEEDED(crumbResult) && node != nullptr && visited < 4096 && incomplete < 64;
             node = node->pNext, ++visited)
        {
            if (node->pLastBreadcrumbValue == nullptr)
                continue;
            const UINT completed = *node->pLastBreadcrumbValue;
            const UINT total = node->BreadcrumbCount;
            if (completed == total)
                continue;
            ++incomplete;
            LOG_ERROR("NR-DIAG list={} name={} queue={} name={} completed={} recorded={}",
                      (void*) node->pCommandList,
                      DebugName(node->pCommandListDebugNameA, node->pCommandListDebugNameW),
                      (void*) node->pCommandQueue,
                      DebugName(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW),
                      completed, total);
            if (completed > total)
                continue;
            // DRED retains only the newest 65536 operations. Do not read overwritten history.
            const UINT retainedStart = total > 65536 ? total - 65536 : 0;
            const UINT start = std::max(retainedStart, completed > 6 ? completed - 6 : 0);
            const UINT end = completed + std::min<UINT>(total - completed, 7);
            if (node->pCommandHistory != nullptr)
                for (UINT i = start; i < end; ++i)
                {
                    const auto op = node->pCommandHistory[i % 65536];
                    LOG_ERROR("NR-DIAG op[{}] {} ({}) {}", i, OpName(op), (UINT) op,
                              i < completed ? "breadcrumb passed" : "breadcrumb not passed");
                }
            UINT shown = 0;
            if (node->pBreadcrumbContexts != nullptr)
                for (UINT i = 0; i < node->BreadcrumbContextsCount && shown < 24; ++i)
                {
                    const auto& ctx = node->pBreadcrumbContexts[i];
                    if (ctx.BreadcrumbIndex >= start && ctx.BreadcrumbIndex < end)
                    {
                        LOG_ERROR("NR-DIAG context[{}] {}", ctx.BreadcrumbIndex,
                                  DebugName(nullptr, ctx.pContextString));
                        ++shown;
                    }
                }
        }
        LOG_ERROR("NR-DIAG breadcrumb lists visited={} incomplete={} truncated={}",
                  visited, incomplete, node != nullptr);
        dred->Release();
    }
    LOG_ERROR("NR-DIAG END DEVICE LOSS");
    if (auto logger = spdlog::default_logger())
        logger->flush();
}
} // namespace DlssNr::Diagnostics
