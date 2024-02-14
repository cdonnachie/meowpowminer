/*
This file is part of ethminer.

ethminer is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

ethminer is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with ethminer.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <fstream>
#include <iostream>

#include <nvrtc.h>

#include <libethcore/Farm.h>
#include <libcrypto/ethash.hpp>
#include <libcrypto/progpow.hpp>

#include "CUDAMiner.h"
#include "CUDAMiner_kernel.h"

using namespace std;
using namespace dev;
using namespace eth;

struct CUDAChannel : public LogChannel
{
    static const char* name() { return EthOrange "cu"; }
    static const int verbosity = 2;
};
#define cudalog clog(CUDAChannel)

CUDAMiner::CUDAMiner(unsigned _index, CUSettings _settings, DeviceDescriptor& _device)
  : Miner("cuda-", _index),
    m_settings(_settings),
    m_batch_size(_settings.gridSize * _settings.blockSize),
    m_streams_batch_size(_settings.gridSize * _settings.blockSize * _settings.streams)
{
    m_deviceDescriptor = _device;
}

CUDAMiner::~CUDAMiner()
{
    stopWorking();
    kick_miner();
}

bool CUDAMiner::initDevice()
{
    cudalog << "Using Pci Id : " << m_deviceDescriptor.uniqueId << " " << m_deviceDescriptor.cuName
            << " (Compute " + m_deviceDescriptor.cuCompute + ") Memory : "
            << dev::getFormattedMemory((double)m_deviceDescriptor.totalMemory);

    // Set Hardware Monitor Info
    m_hwmoninfo.deviceType = HwMonitorInfoType::NVIDIA;
    m_hwmoninfo.devicePciId = m_deviceDescriptor.uniqueId;
    m_hwmoninfo.deviceIndex = -1;  // Will be later on mapped by nvml (see Farm() constructor)

    try
    {
        CU_SAFE_CALL(cuDeviceGet(&m_device, m_deviceDescriptor.cuDeviceIndex));

        try
        {
            CU_SAFE_CALL(cuDevicePrimaryCtxRelease(m_device));
        }
        catch (const cuda_runtime_error& ec)
        {
            (void)ec;
            cudalog << "Releasing a primary context that has not been previously retained will "
                       "fail with CUDA_ERROR_INVALID_CONTEXT, this is normal";
            //            cudalog << " Error : " << ec.what();
        }
        CU_SAFE_CALL(cuDevicePrimaryCtxSetFlags(m_device, m_settings.schedule));
        CU_SAFE_CALL(cuDevicePrimaryCtxRetain(&m_context, m_device));
        CU_SAFE_CALL(cuCtxSetCurrent(m_context));

        // Create mining buffers
        for (unsigned i = 0; i != m_settings.streams; ++i)
        {
            CUDA_SAFE_CALL(cudaMallocHost(&m_search_buf[i], sizeof(Search_results)));
            CUDA_SAFE_CALL(cudaStreamCreateWithFlags(&m_streams[i], cudaStreamNonBlocking));
        }
    }
    catch (const cuda_runtime_error& ec)
    {
        cudalog << "Could not set CUDA device on Pci Id " << m_deviceDescriptor.uniqueId << " Error : " << ec.what();
        cudalog << "Mining aborted on this device.";
        return false;
    }
    return true;
}

bool CUDAMiner::initEpoch_internal()
{
    // If we get here it means epoch has changed so it's not necessary
    // to check again dag sizes. They're changed for sure
    bool retVar = false;
    m_current_target = 0;
    auto startInit = std::chrono::steady_clock::now();
    size_t RequiredMemory = (m_epochContext->full_dataset_size + m_epochContext->light_cache_size);

    size_t FreeMemory = m_deviceDescriptor.freeMemory;
    FreeMemory += m_allocated_memory_dag;
    FreeMemory += m_allocated_memory_light_cache;

    // Release the pause flag if any
    resume(MinerPauseEnum::PauseDueToInsufficientMemory);
    resume(MinerPauseEnum::PauseDueToInitEpochError);

    if (FreeMemory < RequiredMemory)
    {
        cudalog << "Epoch " << m_epochContext->epoch_number << " requires "
                << dev::getFormattedMemory((double)RequiredMemory) << " memory.";
        cudalog << "Only " << dev::getFormattedMemory((double)FreeMemory)
                << " available. Mining suspended on device ...";
        pause(MinerPauseEnum::PauseDueToInsufficientMemory);
        return true;  // This will prevent to exit the thread and
                      // Eventually resume mining when changing coin or epoch (NiceHash)
    }

    try
    {
        // If we have already enough memory allocated, we just have to
        // copy light_cache and regenerate the DAG
        if (m_allocated_memory_dag < m_epochContext->full_dataset_size ||
            m_allocated_memory_light_cache < m_epochContext->light_cache_size)
        {
            // Release previously allocated memory for dag and light
            if (m_device_light)
                CUDA_SAFE_CALL(cudaFree(reinterpret_cast<void*>(m_device_light)));
            if (m_device_dag)
                CUDA_SAFE_CALL(cudaFree(reinterpret_cast<void*>(m_device_dag)));

            cudalog << "Generating DAG + Light : " << dev::getFormattedMemory((double)RequiredMemory);

            // create buffer for cache
            CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&m_device_light), m_epochContext->light_cache_size));
            m_allocated_memory_light_cache = m_epochContext->light_cache_size;
            CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&m_device_dag), m_epochContext->full_dataset_size));
            m_allocated_memory_dag = m_epochContext->full_dataset_size;
        }
        else
        {
            cudalog << "Generating DAG + Light (reusing buffers): " << dev::getFormattedMemory((double)RequiredMemory);
        }

        CUDA_SAFE_CALL(cudaMemcpy(reinterpret_cast<void*>(m_device_light), m_epochContext->light_cache,
            m_epochContext->light_cache_size, cudaMemcpyHostToDevice));

        set_constants(m_device_dag, m_epochContext->light_cache_num_items, m_device_light,
            m_epochContext->light_cache_num_items);  // in ethash_cuda_miner_kernel.cu

        ethash_generate_dag(m_device_dag, m_epochContext->full_dataset_size, m_device_light,
            m_epochContext->light_cache_num_items, m_settings.gridSize, m_settings.blockSize, m_streams[0],
            m_deviceDescriptor.cuDeviceIndex);

        cudalog << "Generated DAG + Light in "
                << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startInit)
                       .count()
                << " ms. " << dev::getFormattedMemory((double)(m_deviceDescriptor.totalMemory - RequiredMemory))
                << " left.";

        retVar = true;
    }
    catch (const cuda_runtime_error& ec)
    {
        cudalog << "Unexpected error " << ec.what() << " on CUDA device " << m_deviceDescriptor.uniqueId;
        cudalog << "Mining suspended ...";
        pause(MinerPauseEnum::PauseDueToInitEpochError);
        retVar = true;
    }
    catch (std::runtime_error const& _e)
    {
        cwarn << "Fatal GPU error: " << _e.what();
        cwarn << "Terminating.";
        exit(-1);
    }

    return retVar;
}

void CUDAMiner::workLoop()
{
    WorkPackage current;
    current.header = h256();
    uint64_t old_period_seed = -1;
    int old_epoch = -1;

    m_search_buf.resize(m_settings.streams);
    m_streams.resize(m_settings.streams);

    if (!initDevice())
        return;

    try
    {
        while (!shouldStop())
        {
            // Wait for work
            bool new_work_expected{true};
            if (!m_new_work.compare_exchange_strong(new_work_expected, false))
            {
                std::unique_lock l(x_work);
                m_new_work_signal.wait_for(l, std::chrono::milliseconds(50));
                continue;
            }

            const WorkPackage w = work();
            if (!w)
            {
                continue;
            }
            if (w.epoch.has_value() && old_epoch != static_cast<int>(w.epoch.value()))
            {
                if (!initEpoch())
                {
                    break;  // This will simply exit the thread
                }
                old_epoch = static_cast<int>(w.epoch.value());
                if (m_new_work.load())
                {
                    continue;
                }
            }
            uint64_t period_seed = w.block.value() / progpow::kPeriodLength;
            if (m_nextProgpowPeriod == 0)
            {
                m_nextProgpowPeriod = period_seed;
                if (m_compileThread)
                {
                    m_compileThread->join();
                }

                m_compileThread.reset(new std::thread([&] {
                    try
                    {
                        asyncCompile();
                    }
                    catch (const std::exception& ex)
                    {
                        cudalog << "Failed to compile MeowPoW kernal : " << ex.what();
                    }
                }));
            }

            if (old_period_seed != period_seed)
            {
                if (m_compileThread)
                {
                    m_compileThread->join();
                }

                // sanity check the next kernel
                if (period_seed != m_nextProgpowPeriod)
                {
                    // This shouldn't happen!!! Try to recover
                    m_nextProgpowPeriod = period_seed;
                    m_compileThread.reset(new std::thread([&] {
                        try
                        {
                            asyncCompile();
                        }
                        catch (const std::exception& ex)
                        {
                            cudalog << "Failed to compile MeowPoW kernal : " << ex.what();
                        }
                    }));
                    m_compileThread->join();
                }
                old_period_seed = period_seed;
                m_kernelExecIx ^= 1;
                cudalog << "Launching period " << period_seed << " MeowPoW kernal";
                m_nextProgpowPeriod = period_seed + 1;
                m_compileThread.reset(new std::thread([&] {
                    try
                    {
                        asyncCompile();
                    }
                    catch (const std::exception& ex)
                    {
                        cudalog << "Failed to compile MeowPoW kernal : " << ex.what();
                    }
                }));
            }

            // Persist most recent job.
            // Job's differences should be handled at higher level
            current = w;
            uint64_t upper64OfBoundary = (uint64_t)(u64)((u256)w.get_boundary() >> 192);

            // Eventually start searching
            search(current.header.data(), upper64OfBoundary, current.startNonce, w);
        }

        // Reset miner and stop working
        CUDA_SAFE_CALL(cudaDeviceReset());
    }
    catch (cuda_runtime_error const& _e)
    {
        string _what = "GPU error: ";
        _what.append(_e.what());
        throw std::runtime_error(_what);
    }
}

void CUDAMiner::kick_miner()
{
    m_new_work.store(true, std::memory_order_relaxed);
    m_new_work_signal.notify_one();
}

int CUDAMiner::getNumDevices()
{
    int deviceCount;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err == cudaSuccess)
        return deviceCount;

    if (err == cudaErrorInsufficientDriver)
    {
        int driverVersion = 0;
        cudaDriverGetVersion(&driverVersion);
        if (driverVersion == 0)
            std::cerr << "CUDA Error : No CUDA driver found" << std::endl;
        else
            std::cerr << "CUDA Error : Insufficient CUDA driver " << std::to_string(driverVersion) << std::endl;
    }
    else
    {
        std::cerr << "CUDA Error : " << cudaGetErrorString(err) << std::endl;
    }

    return 0;
}

void CUDAMiner::enumDevices(std::map<string, DeviceDescriptor>& _DevicesCollection)
{
    int numDevices = getNumDevices();

    for (int i = 0; i < numDevices; i++)
    {
        string uniqueId;
        ostringstream s;
        DeviceDescriptor deviceDescriptor;
        cudaDeviceProp props;

        try
        {
            CUDA_SAFE_CALL(cudaGetDeviceProperties(&props, i));
            CUDA_SAFE_CALL(cudaSetDevice(i));
            s << setw(2) << setfill('0') << hex << props.pciBusID << ":" << setw(2) << props.pciDeviceID << ".0";
            uniqueId = s.str();

            if (_DevicesCollection.find(uniqueId) != _DevicesCollection.end())
                deviceDescriptor = _DevicesCollection[uniqueId];
            else
                deviceDescriptor = DeviceDescriptor();

            deviceDescriptor.name = string(props.name);
            deviceDescriptor.cuDetected = true;
            deviceDescriptor.uniqueId = uniqueId;
            deviceDescriptor.type = DeviceTypeEnum::Gpu;
            deviceDescriptor.cuDeviceIndex = i;
            deviceDescriptor.cuDeviceOrdinal = i;
            deviceDescriptor.cuName = string(props.name);
            deviceDescriptor.totalMemory = props.totalGlobalMem;
            deviceDescriptor.cuCompute = (to_string(props.major) + "." + to_string(props.minor));
            deviceDescriptor.cuComputeMajor = props.major;
            deviceDescriptor.cuComputeMinor = props.minor;
            CUDA_SAFE_CALL(cudaMemGetInfo(&deviceDescriptor.freeMemory, &deviceDescriptor.totalMemory));
            _DevicesCollection[uniqueId] = deviceDescriptor;
        }
        catch (const cuda_runtime_error& _e)
        {
            std::cerr << _e.what() << std::endl;
        }
    }
}

void CUDAMiner::asyncCompile()
{
    auto saveName = getThreadName();
    setThreadName(name().c_str());

    if (!dropThreadPriority())
        cudalog << "Unable to lower compiler priority.";

    cuCtxSetCurrent(m_context);

    compileKernel(m_nextProgpowPeriod, m_epochContext->full_dataset_num_items / 2, m_kernel[m_kernelCompIx]);

    setThreadName(saveName.c_str());

    m_kernelCompIx ^= 1;
}

void CUDAMiner::compileKernel(uint64_t period_seed, uint64_t dag_elms, CUfunction& kernel)
{
    const char* name = "meowpow_search";

    std::string text = progpow::getKern(period_seed, progpow::kernel_type::Cuda);
    text += std::string(CUDAMiner_kernel);

    std::string tmpDir;
#ifdef _WIN32
    tmpDir = getenv("TEMP");
#else
    tmpDir = "/tmp";
#endif
    tmpDir.append("/kernel.");
    tmpDir.append(std::to_string(Index()));
    tmpDir.append(".cu");
#ifdef DEV_BUILD
    cudalog << "Dumping " << tmpDir;
#endif
    ofstream write;
    write.open(tmpDir);
    write << text;
    write.close();

    nvrtcProgram prog;
    NVRTC_SAFE_CALL(nvrtcCreateProgram(&prog,  // prog
        text.c_str(),                          // buffer
        tmpDir.c_str(),                        // name
        0,                                     // numHeaders
        NULL,                                  // headers
        NULL));                                // includeNames

    NVRTC_SAFE_CALL(nvrtcAddNameExpression(prog, name));
    std::string op_arch = "--gpu-architecture=compute_" + to_string(m_deviceDescriptor.cuComputeMajor) +
                          to_string(m_deviceDescriptor.cuComputeMinor);
    std::string op_dag = "-DPROGPOW_DAG_ELEMENTS=" + to_string(dag_elms);

    const char* opts[] = {op_arch.c_str(), op_dag.c_str(), "-lineinfo"};
    nvrtcResult compileResult = nvrtcCompileProgram(prog,  // prog
        sizeof(opts) / sizeof(opts[0]),                    // numOptions
        opts);                                             // options
#ifdef DEV_BUILD
    if (g_logOptions & LOG_COMPILE)
    {
        // Obtain compilation log from the program.
        size_t logSize;
        NVRTC_SAFE_CALL(nvrtcGetProgramLogSize(prog, &logSize));
        char* log = new char[logSize];
        NVRTC_SAFE_CALL(nvrtcGetProgramLog(prog, log));
        cudalog << "Compile log: " << log;
        delete[] log;
    }
#endif
    NVRTC_SAFE_CALL(compileResult);
    // Obtain PTX from the program.
    size_t ptxSize;
    NVRTC_SAFE_CALL(nvrtcGetPTXSize(prog, &ptxSize));
    char* ptx = new char[ptxSize];
    NVRTC_SAFE_CALL(nvrtcGetPTX(prog, ptx));
    // Load the generated PTX and get a handle to the kernel.
    char* jitInfo = new char[32 * 1024];
    char* jitErr = new char[32 * 1024];
    CUjit_option jitOpt[] = {CU_JIT_INFO_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER, CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES,
        CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES, CU_JIT_LOG_VERBOSE, CU_JIT_GENERATE_LINE_INFO};
    void* jitOptVal[] = {jitInfo, jitErr, (void*)(32 * 1024), (void*)(32 * 1024), (void*)(1), (void*)(1)};
    CUmodule module;
    CU_SAFE_CALL(cuModuleLoadDataEx(&module, ptx, 6, jitOpt, jitOptVal));
#ifdef DEV_BUILD
    if (g_logOptions & LOG_COMPILE)
    {
        cudalog << "JIT info: \n" << jitInfo;
        cudalog << "JIT err: \n" << jitErr;
    }
#endif
    delete[] jitInfo;
    delete[] jitErr;
    delete[] ptx;
    // Find the mangled name
    const char* mangledName;
    NVRTC_SAFE_CALL(nvrtcGetLoweredName(prog, name, &mangledName));
#ifdef DEV_BUILD
    if (g_logOptions & LOG_COMPILE)
    {
        cudalog << "Mangled name: " << mangledName;
    }
#endif
    CU_SAFE_CALL(cuModuleGetFunction(&kernel, module, mangledName));

    // Destroy the program.
    NVRTC_SAFE_CALL(nvrtcDestroyProgram(&prog));

    cudalog << "Pre-compiled period " << period_seed << " CUDA MeowPoW kernal for arch "
            << to_string(m_deviceDescriptor.cuComputeMajor) << '.' << to_string(m_deviceDescriptor.cuComputeMinor);
}

void CUDAMiner::search(uint8_t const* header, uint64_t target, uint64_t start_nonce, const dev::eth::WorkPackage& w)
{
    set_header(*reinterpret_cast<hash32_t const*>(header));
    if (m_current_target != target)
    {
        set_target(target);
        m_current_target = target;
    }

    // If upper 64 bits of target are 0xffffffffffffffff then any nonce would
    // be considered valid by GPU. Skip job.
    if (target == UINT64_MAX)
    {
        cudalog << "Difficulty too low for GPU. Skipping job";
        return;
    }

    hash32_t current_header = *reinterpret_cast<hash32_t const*>(header);
    hash64_t* dag;
    get_constants(&dag, NULL, NULL, NULL);

    auto search_start = std::chrono::steady_clock::now();

    // prime each stream, clear search result buffers and start the search
    uint32_t current_index;
    for (current_index = 0; current_index < m_settings.streams; current_index++, start_nonce += m_batch_size)
    {
        cudaStream_t stream = m_streams[current_index];
        volatile Search_results& buffer(*m_search_buf[current_index]);
        buffer.count = 0;

        // Run the batch for this stream
        volatile Search_results* Buffer = &buffer;
        bool hack_false = false;
        void* args[] = {&start_nonce, &current_header, &m_current_target, &dag, &Buffer, &hack_false};
        CU_SAFE_CALL(cuLaunchKernel(m_kernel[m_kernelExecIx],  //
            m_settings.gridSize, 1, 1,                         // grid dim
            m_settings.blockSize, 1, 1,                        // block dim
            0,                                                 // shared mem
            stream,                                            // stream
            args, 0));                                         // arguments
    }

    // process stream batches until we get new work.
    bool done = false;

    uint32_t gids[MAX_SEARCH_RESULTS];
    h256 mixHashes[MAX_SEARCH_RESULTS];


    while (!done)
    {
        // Exit next time around if there's new work awaiting
        done = (done || m_new_work.load() || paused());

        //// Check on every batch if we need to suspend mining
        // if (!done)
        //    done = paused();

        // This inner loop will process each cuda stream individually
        for (current_index = 0; current_index < m_settings.streams; current_index++, start_nonce += m_batch_size)
        {
            // Each pass of this loop will wait for a stream to exit,
            // save any found solutions, then restart the stream
            // on the next group of nonces.
            cudaStream_t stream = m_streams[current_index];

            // Wait for the stream complete
            CUDA_SAFE_CALL(cudaStreamSynchronize(stream));

            if (shouldStop())
            {
                m_new_work.store(false, std::memory_order_relaxed);
                done = true;
            }

            // Detect solutions in current stream's solution buffer
            volatile Search_results& buffer(*m_search_buf[current_index]);
            uint32_t found_count = std::min((unsigned)buffer.count, MAX_SEARCH_RESULTS);

            if (found_count)
            {
                buffer.count = 0;

                // Extract solution and pass to higer level
                // using io_service as dispatcher

                for (uint32_t i = 0; i < found_count; i++)
                {
                    gids[i] = buffer.result[i].gid;
                    memcpy(mixHashes[i].data(), (void*)&buffer.result[i].mix, sizeof(buffer.result[i].mix));
                }
            }

            // restart the stream on the next batch of nonces
            // unless we are done for this round.
            if (!done)
            {
                volatile Search_results* Buffer = &buffer;
                bool hack_false = false;
                void* args[] = {&start_nonce, &current_header, &m_current_target, &dag, &Buffer, &hack_false};
                CU_SAFE_CALL(cuLaunchKernel(m_kernel[m_kernelExecIx],  //
                    m_settings.gridSize, 1, 1,                         // grid dim
                    m_settings.blockSize, 1, 1,                        // block dim
                    0,                                                 // shared mem
                    stream,                                            // stream
                    args, 0));                                         // arguments
            }
            if (found_count)
            {
                uint64_t nonce_base = start_nonce - m_streams_batch_size;
                for (uint32_t i = 0; i < found_count; i++)
                {
                    uint64_t nonce = nonce_base + gids[i];
                    Farm::f().submitProof(Solution{nonce, mixHashes[i], w, std::chrono::steady_clock::now(), m_index});

                    double d = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - search_start)
                                   .count();

                    cudalog << EthWhite << "Job: " << w.header.abridged() << " Sol: 0x" << toHex(nonce)
                            << EthLime " found in " << dev::getFormattedElapsed(d) << EthReset;
                }
            }
        }

        // Update the hash rate
        updateHashRate(m_batch_size, m_settings.streams);

        // Bail out if it's shutdown time
        if (shouldStop())
        {
            m_new_work.store(false, std::memory_order_relaxed);
            break;
        }
    }

#ifdef DEV_BUILD
    // Optionally log job switch time
    if (!shouldStop() && (g_logOptions & LOG_SWITCH))
        cudalog << "Switch time: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - m_workSwitchStart)
                       .count()
                << " ms.";
#endif
}
