#include "ConcreteMiddleware.h"
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <cassert>
#include <cstdlib>
#include "../../PresentMonUtils/NamedPipeHelper.h"
#include "../../PresentMonUtils/QPCUtils.h"
#include "../../PresentMonAPI2/source/Internal.h"
#include "../../PresentMonAPIWrapperCommon/source/Introspection.h"
// TODO: don't need transfer if we can somehow get the PM_ struct generation working without inheritance
// needed right now because even if we forward declare, we don't have the inheritance info
#include "../../Interprocess/source/IntrospectionTransfer.h"
#include "../../Interprocess/source/IntrospectionHelpers.h"
#include "../../Interprocess/source/IntrospectionCloneAllocators.h"
//#include "MockCommon.h"
#include "DynamicQuery.h"
#include "../../ControlLib/PresentMonPowerTelemetry.h"
#include "../../ControlLib/CpuTelemetryInfo.h"
#include "../../PresentMonService/GlobalIdentifiers.h"

namespace pmon::mid
{
    using namespace ipc::intro;

    static const uint32_t kMaxRespBufferSize = 4096;
	static const uint64_t kClientFrameDeltaQPCThreshold = 50000000;
	ConcreteMiddleware::ConcreteMiddleware(std::optional<std::string> pipeNameOverride, std::optional<std::string> introNsmOverride)
	{
        const auto pipeName = pipeNameOverride.transform(&std::string::c_str)
            .value_or(pmon::gid::defaultControlPipeName);

        HANDLE namedPipeHandle;
        // Try to open a named pipe; wait for it, if necessary.
        while (1) {
            namedPipeHandle = CreateFileA(
                pipeName,
                GENERIC_READ | GENERIC_WRITE,
                0,              
                NULL,           
                OPEN_EXISTING,  
                0,              
                NULL);          

            // Break if the pipe handle is valid.
            if (namedPipeHandle != INVALID_HANDLE_VALUE) {
                break;
            }

            // Exit if an error other than ERROR_PIPE_BUSY occurs.
            if (const auto hr = GetLastError(); hr != ERROR_PIPE_BUSY) {
                throw std::runtime_error{ "Service not found" };
            }

            // All pipe instances are busy, so wait for 20 seconds.
            if (!WaitNamedPipeA(pipeName, 20000)) {
                throw std::runtime_error{ "Pipe sessions full" };
            }
        }
        // The pipe connected; change to message-read mode.
        DWORD mode = PIPE_READMODE_MESSAGE;
        BOOL success = SetNamedPipeHandleState(namedPipeHandle,
            &mode,
            NULL,
            NULL);
        if (!success) {
            throw std::runtime_error{ "Pipe error" };
        }
        pNamedPipeHandle.reset(namedPipeHandle);
        clientProcessId = GetCurrentProcessId();
        // connect to the introspection nsm
        pComms = ipc::MakeMiddlewareComms(std::move(introNsmOverride));
	}
    
    const PM_INTROSPECTION_ROOT* ConcreteMiddleware::GetIntrospectionData()
    {
        return pComms->GetIntrospectionRoot();
    }

    void ConcreteMiddleware::FreeIntrospectionData(const PM_INTROSPECTION_ROOT* pRoot)
    {
        free(const_cast<PM_INTROSPECTION_ROOT*>(pRoot));
    }

	void ConcreteMiddleware::Speak(char* buffer) const
	{
		strcpy_s(buffer, 256, "concrete-middle");
	}

    PM_STATUS ConcreteMiddleware::SendRequest(MemBuffer* requestBuffer) {
        DWORD bytesWritten;
        BOOL success = WriteFile(
            pNamedPipeHandle.get(),
            requestBuffer->AccessMem(),
            static_cast<DWORD>(requestBuffer->GetCurrentSize()),
            &bytesWritten,
            NULL);

        if (success && requestBuffer->GetCurrentSize() == bytesWritten) {
            return PM_STATUS::PM_STATUS_SUCCESS;
        }
        else {
            return PM_STATUS::PM_STATUS_FAILURE;
        }
    }

    PM_STATUS ConcreteMiddleware::ReadResponse(MemBuffer* responseBuffer) {
        BOOL success;
        DWORD bytesRead;
        BYTE inBuffer[kMaxRespBufferSize];
        ZeroMemory(&inBuffer, sizeof(inBuffer));

        do {
            // Read from the pipe using a nonoverlapped read
            success = ReadFile(pNamedPipeHandle.get(),
                inBuffer,
                sizeof(inBuffer),
                &bytesRead,
                NULL);

            // If the call was not successful AND there was
            // no more data to read bail out
            if (!success && GetLastError() != ERROR_MORE_DATA) {
                break;
            }

            // Either the call was successful or there was more
            // data in the pipe. In both cases add the response data
            // to the memory buffer
            responseBuffer->AddItem(inBuffer, bytesRead);
        } while (!success);  // repeat loop if ERROR_MORE_DATA

        if (success) {
            return PM_STATUS::PM_STATUS_SUCCESS;
        }
        else {
            return PM_STATUS::PM_STATUS_FAILURE;
        }
    }

    PM_STATUS ConcreteMiddleware::CallPmService(MemBuffer* requestBuffer, MemBuffer* responseBuffer)
    {
        PM_STATUS status;

        status = SendRequest(requestBuffer);
        if (status != PM_STATUS::PM_STATUS_SUCCESS) {
            return status;
        }

        status = ReadResponse(responseBuffer);
        if (status != PM_STATUS::PM_STATUS_SUCCESS) {
            return status;
        }

        return status;
    }

    PM_STATUS ConcreteMiddleware::StartStreaming(uint32_t processId)
    {
        MemBuffer requestBuffer;
        MemBuffer responseBuffer;

        NamedPipeHelper::EncodeStartStreamingRequest(&requestBuffer, clientProcessId,
            processId, nullptr);

        PM_STATUS status = CallPmService(&requestBuffer, &responseBuffer);
        if (status != PM_STATUS::PM_STATUS_SUCCESS) {
            return status;
        }

        IPMSMStartStreamResponse startStreamResponse{};

        status = NamedPipeHelper::DecodeStartStreamingResponse(
            &responseBuffer, &startStreamResponse);
        if (status != PM_STATUS::PM_STATUS_SUCCESS) {
            return status;
        }

        // Get the NSM file name from 
        std::string mapFileName(startStreamResponse.fileName);

        // Initialize client with returned mapfile name
        auto iter = presentMonStreamClients.find(processId);
        if (iter == presentMonStreamClients.end()) {
            try {
                std::unique_ptr<StreamClient> client =
                    std::make_unique<StreamClient>(std::move(mapFileName), false);
                presentMonStreamClients.emplace(processId, std::move(client));
            }
            catch (...) {
                return PM_STATUS::PM_STATUS_FAILURE;
            }
        }

        // TODO: Where will the client caches reside? As part of the dynamic query?
        //if (!SetupClientCaches(process_id)) {
        //    return PM_STATUS::PM_STATUS_FAILURE;
        //}

        return PM_STATUS_SUCCESS;
    }
    
    PM_STATUS ConcreteMiddleware::StopStreaming(uint32_t processId)
    {
        MemBuffer requestBuffer;
        MemBuffer responseBuffer;

        NamedPipeHelper::EncodeStopStreamingRequest(&requestBuffer,
            clientProcessId,
            processId);

        PM_STATUS status = CallPmService(&requestBuffer, &responseBuffer);
        if (status != PM_STATUS::PM_STATUS_SUCCESS) {
            return status;
        }

        status = NamedPipeHelper::DecodeStopStreamingResponse(&responseBuffer);
        if (status != PM_STATUS::PM_STATUS_SUCCESS) {
            return status;
        }

        // Remove client
        auto iter = presentMonStreamClients.find(processId);
        if (iter != presentMonStreamClients.end()) {
            presentMonStreamClients.erase(std::move(iter));
        }

        // TODO: If cached data is part of query maybe we can
        // remove this code
        //RemoveClientCaches(process_id);

        return status;
    }

    PM_DYNAMIC_QUERY* ConcreteMiddleware::RegisterDynamicQuery(std::span<PM_QUERY_ELEMENT> queryElements, uint32_t processId, double windowSizeMs, double metricOffsetMs)
    { 
        // get introspection data for reference
        // TODO: cache this data so it's not required to be generated every time
        pmapi::intro::Dataset ispec{ GetIntrospectionData(), [this](auto p) {FreeIntrospectionData(p); } };

        // make the query object that will be managed by the handle
        auto pQuery = std::make_unique<PM_DYNAMIC_QUERY>();

        uint64_t offset = 0u;
        for (auto& qe : queryElements) {
            auto metricView = ispec.FindMetric(qe.metric);
            if (metricView.GetType().GetValue() != int(PM_METRIC_TYPE_DYNAMIC)) {
                // TODO: specific exception here
                throw std::runtime_error{ "Static metric in dynamic metric query specification" };
            }
            switch (qe.metric) {
            case PM_METRIC_PRESENTED_FPS:
            case PM_METRIC_DISPLAYED_FPS:
            case PM_METRIC_FRAME_TIME:
            case PM_METRIC_GPU_BUSY_TIME:
            case PM_METRIC_CPU_BUSY_TIME:
            case PM_METRIC_CPU_WAIT_TIME:
            case PM_METRIC_DISPLAY_BUSY_TIME:
                pQuery->accumFpsData = true;
                break;
            case PM_METRIC_GPU_POWER:
                pQuery->accumGpuBits.set(static_cast<size_t>(GpuTelemetryCapBits::gpu_power));
                break;
            case PM_METRIC_GPU_FAN_SPEED:
                switch (qe.arrayIndex)
                {
                case 0:
                    pQuery->accumGpuBits.set(static_cast<size_t>(GpuTelemetryCapBits::fan_speed_0));
                    break;
                case 1:
                    pQuery->accumGpuBits.set(static_cast<size_t>(GpuTelemetryCapBits::fan_speed_1));
                    break;
                case 2:
                    pQuery->accumGpuBits.set(static_cast<size_t>(GpuTelemetryCapBits::fan_speed_2));
                    break;
                case 3:
                    pQuery->accumGpuBits.set(static_cast<size_t>(GpuTelemetryCapBits::fan_speed_3));
                    break;
                case 4:
                    pQuery->accumGpuBits.set(static_cast<size_t>(GpuTelemetryCapBits::fan_speed_4));
                    break;
                default:
                    // Unknown fan speed index
                    throw std::runtime_error{ "Invalid fan speed index" };
                }
                break;
            default:
                break;
            }

            auto result = pQuery->compiledMetrics.emplace(qe.metric, CompiledStats());
            auto stats = &result.first->second;
            switch (qe.stat)
            {
            case PM_STAT_AVG:
                stats->calcAvg = true;
                break;
            case PM_STAT_PERCENTILE_99:
                stats->calcPercentile99 = true;
                break;
            case PM_STAT_PERCENTILE_95:
                stats->calcPercentile95 = true;
                break;
            case PM_STAT_PERCENTILE_90:
                stats->calcPercentile90 = true;
                break;
            case PM_STAT_MAX:
                stats->calcMax = true;
                break;
            case PM_STAT_MIN:
                stats->calcMin = true;
                break;
            case PM_STAT_RAW:
                stats->calcRaw = true;
                break;
            default:
                // Invalid stat enum
                throw std::runtime_error{ "Invalid stat enum" };
            }
            // TODO: validate device id
            // TODO: validate array index
            qe.dataOffset = offset;
            //qe.dataSize = GetDataTypeSize(metricView.GetDataTypeInfo().GetBasePtr()->type);
            qe.dataSize = size_t(8);
            offset += qe.dataSize;
        }

        queryFrameDataDeltas.emplace(pQuery.get(), uint64_t());
        pQuery->dynamicQueryHandle = pQuery.get();
        pQuery->metricOffsetMs = metricOffsetMs;
        pQuery->windowSizeMs = windowSizeMs;
        pQuery->processId = processId;
        pQuery->elements = std::vector<PM_QUERY_ELEMENT>{ queryElements.begin(), queryElements.end() };

        return pQuery.release();
    }

    struct fps_swap_chain_data {
        std::vector<double> displayed_fps;
        std::vector<double> frame_times_ms;
        std::vector<double> gpu_sum_ms;
        std::vector<double> dropped;
        uint64_t present_start_0 = 0;             // The first frame's PresentStartTime (qpc)
        uint64_t present_start_n = 0;             // The last frame's PresentStartTime (qpc)
        uint64_t present_stop_0 = 0;             // The first frame's PresentStopTime (qpc)
        uint64_t present_stop_n = 0;             // The last frame's PresentStopTime (qpc)
        uint64_t gpu_duration_0 = 0;             // The first frame's GPUDuration (qpc)
        uint64_t display_n_screen_time = 0;       // The last presented frame's ScreenTime (qpc)
        uint64_t display_0_screen_time = 0;       // The first presented frame's ScreenTime (qpc)
        uint64_t display_1_screen_time = 0;       // The second presented frame's ScreenTime (qpc)
        uint32_t display_count = 0;               // The number of presented frames
        uint32_t num_presents = 0;                // The number of frames
        bool     displayed_0 = false;             // Whether the first frame was displayed

        // Properties of the most-recent processed frame:
        int32_t sync_interval = 0;
        PM_PRESENT_MODE present_mode = PM_PRESENT_MODE_UNKNOWN;
        int32_t allows_tearing = 0;

        // Only used by GetGfxLatencyData():
        std::vector<double> render_latency_ms;
        std::vector<double> display_latency_ms;
        uint64_t render_latency_sum = 0;
        uint64_t display_latency_sum = 0;
    };

    void ConcreteMiddleware::PollDynamicQuery(const PM_DYNAMIC_QUERY* pQuery, uint8_t* pBlob, uint32_t* numSwapChains)
    {
        std::unordered_map<uint64_t, fps_swap_chain_data> swap_chain_data;
        LARGE_INTEGER api_qpc;
        QueryPerformanceCounter(&api_qpc);

        if (*numSwapChains == 0) {
            return;
        }

        // Check to stream client associated with the process id saved in the dynamic query
        auto iter = presentMonStreamClients.find(pQuery->processId);
        if (iter == presentMonStreamClients.end()) {
            return;
        }

        // Get the named shared memory associated with the stream client
        StreamClient* client = iter->second.get();
        auto nsm_view = client->GetNamedSharedMemView();
        auto nsm_hdr = nsm_view->GetHeader();
        if (!nsm_hdr->process_active) {
            // TODO: Do we want to inform the client if the server has destroyed the
            // named shared memory?
            // Server destroyed the named shared memory due to process exit. Destroy the
            // mapped view from client side.
            //StopStreamProcess(process_id);
            //return PM_STATUS::PM_STATUS_PROCESS_NOT_EXIST;
            return;
        }

        uint64_t index = 0;
        double adjusted_window_size_in_ms = pQuery->metricOffsetMs;

        auto result = queryFrameDataDeltas.emplace(pQuery->dynamicQueryHandle, uint64_t());
        auto queryToFrameDataDelta = &result.first->second;
        
        PmNsmFrameData* frame_data = GetFrameDataStart(client, index, SecondsDeltaToQpc(pQuery->metricOffsetMs/1000., client->GetQpcFrequency()), *queryToFrameDataDelta, adjusted_window_size_in_ms);
        if (frame_data == nullptr) {
            //if (CopyCacheData(fps_data, num_swapchains, metric_cache->cached_fps_data_)) {
            //    return PM_STATUS::PM_STATUS_SUCCESS;
            //}
            //else {
            //    return PM_STATUS::PM_STATUS_NO_DATA;
            //}
            return;
        }

        // Calculate the end qpc based on the current frame's qpc and
        // requested window size coverted to a qpc
        uint64_t end_qpc =
            frame_data->present_event.PresentStartTime -
            SecondsDeltaToQpc(adjusted_window_size_in_ms/1000., client->GetQpcFrequency());

        // These are only used for logging:
        uint64_t last_checked_qpc = frame_data->present_event.PresentStartTime;
        bool decrement_failed = false;
        bool read_frame_failed = false;

        // Loop from the most recent frame data until we either run out of data or
        // we meet the window size requirements sent in by the client
        while (frame_data->present_event.PresentStartTime > end_qpc) {
            auto result = swap_chain_data.emplace(
                frame_data->present_event.SwapChainAddress, fps_swap_chain_data());
            auto swap_chain = &result.first->second;

            // Copy swap_chain data for the previous first frame needed for calculations below
            auto nextFramePresentStartTime = swap_chain->present_start_0;
            auto nextFramePresentStopTime = swap_chain->present_stop_0;
            auto nextFrameGPUDuration = swap_chain->gpu_duration_0;

            // Save current frame's properties into swap_chain (the new first frame)
            swap_chain->displayed_0 = frame_data->present_event.FinalState == PresentResult::Presented;
            swap_chain->present_start_0 = frame_data->present_event.PresentStartTime;
            swap_chain->present_stop_0 = frame_data->present_event.PresentStopTime;
            swap_chain->gpu_duration_0 = frame_data->present_event.GPUDuration;
            swap_chain->num_presents += 1;

            if (swap_chain->displayed_0) {
                swap_chain->display_1_screen_time = swap_chain->display_0_screen_time;
                swap_chain->display_0_screen_time = frame_data->present_event.ScreenTime;
                swap_chain->display_count += 1;
                if (swap_chain->display_count == 1) {
                    swap_chain->display_n_screen_time = frame_data->present_event.ScreenTime;
                }
            }

            // These are only saved for the last frame:
            if (swap_chain->num_presents == 1) {
                swap_chain->present_stop_n = frame_data->present_event.PresentStopTime;
                swap_chain->sync_interval = frame_data->present_event.SyncInterval;
                //swap_chain->present_mode = TranslatePresentMode(frame_data->present_event.PresentMode);
                swap_chain->allows_tearing = static_cast<int32_t>(frame_data->present_event.SupportsTearing);
            }

            // Compute metrics for this frame if we've seen enough subsequent frames to have all the
            // required data
            //
            // frame_data:      PresentStart--PresentStop--GPUDuration--ScreenTime
            // nextFrame:                                   PresentStart--PresentStop--GPUDuration--ScreenTime
            // nextNextFrame:                                                           PresentStart--PresentStop--GPUDuration--ScreenTime
            //                                CPUStart
            //                                CPUBusy------>CPUWait----------------->  GPUBusy--->  DisplayBusy---------------->
            if (swap_chain->num_presents > 1) {
                auto cpuStart = frame_data->present_event.PresentStopTime;
                auto cpuBusy = nextFramePresentStartTime - cpuStart;
                auto cpuWait = nextFramePresentStopTime - nextFramePresentStartTime;
                auto gpuBusy = nextFrameGPUDuration;
                auto displayBusy = swap_chain->display_1_screen_time - swap_chain->display_0_screen_time;;

                auto frameTime_ms = QpcDeltaToMs(cpuBusy + cpuWait, client->GetQpcFrequency());
                auto gpuBusy_ms = QpcDeltaToMs(gpuBusy, client->GetQpcFrequency());
                auto displayBusy_ms = QpcDeltaToMs(displayBusy, client->GetQpcFrequency());
                auto cpuBusy_ms = QpcDeltaToMs(cpuBusy, client->GetQpcFrequency());
                auto cpuWait_ms = QpcDeltaToMs(cpuWait, client->GetQpcFrequency());

                swap_chain->frame_times_ms.push_back(frameTime_ms);
                swap_chain->gpu_sum_ms.push_back(gpuBusy_ms);
                swap_chain->dropped.push_back(swap_chain->displayed_0 ? 0. : 1.);

                if (swap_chain->displayed_0 && swap_chain->display_count >= 2 && displayBusy > 0) {
                    swap_chain->displayed_fps.push_back(1000. / displayBusy_ms);
                }
            }

            // Get the index of the next frame
            if (DecrementIndex(nsm_view, index) == false) {
                // We have run out of data to process, time to go
                decrement_failed = true;
                break;
            }
            frame_data = client->ReadFrameByIdx(index);
            if (frame_data == nullptr) {
                read_frame_failed = true;
                break;
            }
        }
    }

    PmNsmFrameData* ConcreteMiddleware::GetFrameDataStart(StreamClient* client, uint64_t& index, uint64_t queryMetricsDataOffset, uint64_t& queryFrameDataDelta, double& window_sample_size_in_ms)
    {

        PmNsmFrameData* frame_data = nullptr;
        index = 0;
        if (client == nullptr) {
            return nullptr;
        }

        auto nsm_view = client->GetNamedSharedMemView();
        auto nsm_hdr = nsm_view->GetHeader();
        if (!nsm_hdr->process_active) {
            return nullptr;
        }

        index = client->GetLatestFrameIndex();
        frame_data = client->ReadFrameByIdx(index);
        if (frame_data == nullptr) {
            index = 0;
            return nullptr;
        }

        if (queryMetricsDataOffset == 0) {
            // Client has not specified a metric offset. Return back the most
            // most recent frame data
            return frame_data;
        }

        LARGE_INTEGER client_qpc = {};
        QueryPerformanceCounter(&client_qpc);
        uint64_t adjusted_qpc = GetAdjustedQpc(
            client_qpc.QuadPart, frame_data->present_event.PresentStartTime,
            queryMetricsDataOffset, client->GetQpcFrequency(), queryFrameDataDelta);

        if (adjusted_qpc > frame_data->present_event.PresentStartTime) {
            // Need to adjust the size of the window sample size
            double ms_adjustment =
                QpcDeltaToMs(adjusted_qpc - frame_data->present_event.PresentStartTime,
                    client->GetQpcFrequency());
            window_sample_size_in_ms = window_sample_size_in_ms - ms_adjustment;
            if (window_sample_size_in_ms <= 0.0) {
                return nullptr;
            }
        }
        else {
            // Find the frame with the appropriate time based on the adjusted
            // qpc
            for (;;) {

                if (DecrementIndex(nsm_view, index) == false) {
                    // Increment index to match up with the frame_data read below
                    index++;
                    break;
                }
                frame_data = client->ReadFrameByIdx(index);
                if (frame_data == nullptr) {
                    return nullptr;
                }
                if (adjusted_qpc >= frame_data->present_event.PresentStartTime) {
                    break;
                }
            }
        }

        return frame_data;
    }

    uint64_t ConcreteMiddleware::GetAdjustedQpc(uint64_t current_qpc, uint64_t frame_data_qpc, uint64_t queryMetricsOffset, LARGE_INTEGER frequency, uint64_t& queryFrameDataDelta) {
        // Calculate how far behind the frame data qpc is compared
        // to the client qpc
        uint64_t current_qpc_delta = current_qpc - frame_data_qpc;
        if (queryFrameDataDelta == 0) {
            queryFrameDataDelta = current_qpc_delta;
        }
        else {
            if (_abs64(queryFrameDataDelta - current_qpc_delta) >
                kClientFrameDeltaQPCThreshold) {
                queryFrameDataDelta = current_qpc_delta;
            }
        }

        // Add in the client set metric offset in qpc ticks
        return current_qpc -
            (queryFrameDataDelta + queryMetricsOffset);
    }

    bool ConcreteMiddleware::DecrementIndex(NamedSharedMem* nsm_view, uint64_t& index) {

        if (nsm_view == nullptr) {
            return false;
        }

        auto nsm_hdr = nsm_view->GetHeader();
        if (!nsm_hdr->process_active) {
            return false;
        }

        uint64_t current_max_entries =
            (nsm_view->IsFull()) ? nsm_hdr->max_entries - 1 : nsm_hdr->tail_idx;
        index = (index == 0) ? current_max_entries : index - 1;
        if (index == nsm_hdr->head_idx) {
            return false;
        }

        return true;
    }
}