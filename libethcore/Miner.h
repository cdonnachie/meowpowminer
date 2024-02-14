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

#pragma once
#ifndef LIBCRYPTO_MINER_H_
#define LIBCRYPTO_MINER_H_


#include <bitset>
#include <condition_variable>
#include <list>
#include <numeric>
#include <optional>
#include <string>

//#include "EthashAux.h"
#include <libdevcore/Common.h>
#include <libdevcore/Log.h>
#include <libdevcore/Worker.h>

#include <boost/asio.hpp>
#include <boost/format.hpp>

#include <libcrypto/ethash.hpp>

#define DAG_LOAD_MODE_PARALLEL 0
#define DAG_LOAD_MODE_SEQUENTIAL 1


extern boost::asio::io_service g_io_service;

namespace dev::eth
{
enum class DeviceTypeEnum
{
    Unknown,
    Cpu,
    Gpu,
    Accelerator
};

enum class DeviceSubscriptionTypeEnum
{
    None,
    OpenCL,
    Cuda,
    Cpu

};

enum class MinerType
{
    Mixed,
    CL,
    CUDA,
    CPU
};

enum class HwMonitorInfoType
{
    UNKNOWN,
    NVIDIA,
    AMD,
    CPU
};

enum class ClPlatformTypeEnum
{
    Unknown,
    Amd,
    Clover,
    Nvidia
};

enum class SolutionAccountingEnum
{
    Accepted,
    Rejected,
    Wasted,
    Failed
};

struct MinerSettings
{
    std::vector<unsigned> devices;
};

// Holds settings for CUDA Miner
struct CUSettings : public MinerSettings
{
    unsigned streams = 2;
    unsigned schedule = 4;
    unsigned gridSize = 256;
    unsigned blockSize = 512;
    unsigned parallelHash = 4;
};

// Holds settings for OpenCL Miner
struct CLSettings : public MinerSettings
{
    unsigned globalWorkSize = 0;
    unsigned globalWorkSizeMultiplier = 32768;
    unsigned localWorkSize = 256;
};

// Holds settings for CPU Miner
struct CPSettings : public MinerSettings
{
};

struct SolutionAccountType
{
    unsigned accepted = 0;
    unsigned rejected = 0;
    unsigned wasted = 0;
    unsigned failed = 0;
    std::chrono::steady_clock::time_point tstamp = std::chrono::steady_clock::now();
    std::string str()
    {
        std::string _ret = "A" + std::to_string(accepted);
        if (wasted)
            _ret.append(":W" + std::to_string(wasted));
        if (rejected)
            _ret.append(":R" + std::to_string(rejected));
        if (failed)
            _ret.append(":F" + std::to_string(failed));
        return _ret;
    };
};

struct HwSensorsType
{
    int tempC = 0;
    int fanP = 0;
    double powerW = 0.0;
    std::string str()
    {
        std::string _ret = std::to_string(tempC) + "C " + std::to_string(fanP) + "%";
        if (powerW)
            _ret.append(boost::str(boost::format("%f") % powerW));
        return _ret;
    };
};

struct TelemetryAccountType
{
    std::string prefix = "";
    float hashrate = 0.0f;
    bool paused = false;
    HwSensorsType sensors;
    SolutionAccountType solutions;
};

struct DeviceDescriptor
{
    DeviceTypeEnum type = DeviceTypeEnum::Unknown;
    DeviceSubscriptionTypeEnum subscriptionType = DeviceSubscriptionTypeEnum::None;

    std::string uniqueId;  // For GPUs this is the PCI ID
    size_t totalMemory;    // Total memory available on device
    size_t freeMemory;     // Free memory available on device
    std::string name;      // Device Name

    bool clDetected;  // For OpenCL detected devices
    std::string clName;
    unsigned int clPlatformId;
    std::string clPlatformName;
    ClPlatformTypeEnum clPlatformType = ClPlatformTypeEnum::Unknown;
    std::string clPlatformVersion;
    unsigned int clPlatformVersionMajor;
    unsigned int clPlatformVersionMinor;
    unsigned int clDeviceOrdinal;
    unsigned int clDeviceIndex;
    std::string clDeviceVersion;
    unsigned int clDeviceVersionMajor;
    unsigned int clDeviceVersionMinor;
    std::string clBoardName;
    size_t clMaxMemAlloc;
    size_t clMaxWorkGroup;
    unsigned int clMaxComputeUnits;
    std::string clNvCompute;
    unsigned int clNvComputeMajor;
    unsigned int clNvComputeMinor;

    bool cuDetected;  // For CUDA detected devices
    std::string cuName;
    unsigned int cuDeviceOrdinal;
    unsigned int cuDeviceIndex;
    std::string cuCompute;
    unsigned int cuComputeMajor;
    unsigned int cuComputeMinor;

    int cpCpuNumer;  // For CPU
};

struct HwMonitorInfo
{
    HwMonitorInfoType deviceType = HwMonitorInfoType::UNKNOWN;
    std::string devicePciId;
    int deviceIndex = -1;
};

/// Pause mining
enum MinerPauseEnum
{
    PauseDueToOverHeating,
    PauseDueToAPIRequest,
    PauseDueToFarmPaused,
    PauseDueToInsufficientMemory,
    PauseDueToInitEpochError,
    Pause_MAX  // Must always be last as a placeholder of max count
};

/// Keeps track of progress for farm and miners
struct TelemetryType
{
    bool hwmon = false;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    TelemetryAccountType farm;
    std::vector<TelemetryAccountType> miners;
    std::string str()
    {
        std::stringstream _ret;

        /*

        Output is formatted as

        Run <h:mm> <Solutions> <Speed> [<miner> ...]
        where
        - Run h:mm    Duration of the batch
        - Solutions   Detailed solutions (A+R+F) per farm
        - Speed       Actual hashing rate

        each <miner> reports
        - speed       Actual speed at the same level of
                      magnitude for farm speed
        - sensors     Values of sensors (temp, fan, power)
        - solutions   Optional (LOG_PER_GPU) Solutions detail per GPU
        */

        /*
        Calculate duration
        */
        auto duration = std::chrono::steady_clock::now() - start;
        auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
        int hoursSize = (hours.count() > 9 ? (hours.count() > 99 ? 3 : 2) : 1);
        duration -= hours;
        auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);
        _ret << EthGreen << std::setw(hoursSize) << hours.count() << ":" << std::setfill('0') << std::setw(2)
             << minutes.count() << EthReset << EthWhiteBold << " " << farm.solutions.str() << EthReset << " ";

        /*
        Github : @AndreaLanfranchi
        I whish I could simply make use of getFormattedHashes but in this case
        this would be misleading as total hashrate could be of a different order
        of magnitude than the hashrate expressed by single devices.
        Thus I need to set the vary same scaling index on the farm and on devices
        */
        static std::string suffixes[] = {"h", "Kh", "Mh", "Gh"};
        float hr = farm.hashrate;
        int magnitude = 0;
        while (hr > 1000.0f && magnitude <= 3)
        {
            hr /= 1000.0f;
            magnitude++;
        }

        _ret << EthTealBold << std::fixed << std::setprecision(2) << hr << " " << suffixes[magnitude] << EthReset
             << " - ";

        int i = -1;                 // Current miner index
        int m = miners.size() - 1;  // Max miner index
        for (TelemetryAccountType miner : miners)
        {
            i++;
            hr = miner.hashrate;
            if (hr > 0.0f)
                hr /= pow(1000.0f, magnitude);

            _ret << (miner.paused ? EthRed : "") << miner.prefix << i << " " << EthTeal << std::fixed
                 << std::setprecision(2) << hr << EthReset;

            if (hwmon)
                _ret << " " << EthTeal << miner.sensors.str() << EthReset;

            // Eventually push also solutions per single GPU
            if (g_logOptions & LOG_PER_GPU)
                _ret << " " << EthTeal << miner.solutions.str() << EthReset;

            // Separator if not the last miner index
            if (i < m)
                _ret << ", ";
        }

        return _ret.str();
    };
};

struct WorkPackage
{
    WorkPackage() = default;

    explicit operator bool() const { return header != h256(); }

    std::string job;  // Job identifier can be anything. Not necessarily a hash
    h256 boundary;
    h256 header;  // < When h256() means "pause until notified a new work package is available".
    h256 seed;
    h256 block_boundary;

    h256 get_boundary() const
    {
        if (block_boundary == h256{})
            return boundary;
        else if (boundary < block_boundary)
            return block_boundary;
        else
            return boundary;
    }

    std::optional<uint32_t> epoch;
    std::optional<uint32_t> block;

    uint64_t startNonce = 0;
    uint16_t exSizeBytes = 0;

    std::string algo = "meowpow";
};


struct Solution
{
    uint64_t nonce;                                // Solution found nonce
    h256 mixHash;                                  // Mix hash
    WorkPackage work;                              // WorkPackage this solution refers to
    std::chrono::steady_clock::time_point tstamp;  // Timestamp of found solution
    unsigned midx;                                 // Originating miner Id
};

/**
 * @brief Class for hosting one or more Miners.
 * @warning Must be implemented in a threadsafe manner since it will be called from multiple
 * miner threads.
 */
class FarmFace
{
public:
    FarmFace() { m_this = this; }
    static FarmFace& f() { return *m_this; };

    virtual ~FarmFace() = default;
    virtual unsigned get_tstart() = 0;
    virtual unsigned get_tstop() = 0;
    virtual unsigned get_ergodicity() = 0;

    /**
     * @brief Called from a Miner to note a WorkPackage has a solution.
     * @param _p The solution.
     * @return true iff the solution was good (implying that mining should be .
     */
    virtual void submitProof(Solution const& _p) = 0;
    virtual void accountSolution(unsigned _minerIdx, SolutionAccountingEnum _accounting) = 0;
    virtual uint64_t get_nonce_scrambler() = 0;
    virtual unsigned get_segment_width() = 0;

private:
    static FarmFace* m_this;
};

/**
 * @brief A miner - a member and adoptee of the Farm.
 * @warning Not threadsafe. It is assumed Farm will synchronise calls to/from this class.
 */

class Miner : public Worker
{
public:
    Miner(std::string const& _name, unsigned _index) : Worker(_name + std::to_string(_index)), m_index(_index) {}

    ~Miner() override = default;

    // Sets basic info for eventual serialization of DAG load
    static void setDagLoadInfo(unsigned _mode, unsigned _devicecount)
    {
        s_dagLoadMode = _mode;
        s_dagLoadIndex = 0;
        s_minersCount = _devicecount;
    };

    /**
     * @brief Gets the device descriptor assigned to this instance
     */
    DeviceDescriptor getDescriptor();

    /**
     * @brief Assigns hashing work to this instance
     */
    void setWork(WorkPackage const& _work);

    /**
     * @brief Assigns Epoch context to this instance
     */
    void setEpoch(std::shared_ptr<ethash::epoch_context> const& _ec) { m_epochContext = _ec; }

    unsigned Index() { return m_index; };

    HwMonitorInfo hwmonInfo() { return m_hwmoninfo; }

    void setHwmonDeviceIndex(int i) { m_hwmoninfo.deviceIndex = i; }

    /**
     * @brief Kick an asleep miner.
     */
    virtual void kick_miner() = 0;

    /**
     * @brief Pauses mining setting a reason flag
     */
    void pause(MinerPauseEnum what);

    /**
     * @brief Whether or not this miner is paused for any reason
     */
    bool paused();

    /**
     * @brief Checks if the given reason for pausing is currently active
     */
    bool pauseTest(MinerPauseEnum what);

    /**
     * @brief Returns the human readable reason for this miner being paused
     */
    std::string pausedString();

    /**
     * @brief Cancels a pause flag.
     * @note Miner can be paused for multiple reasons at a time.
     */
    void resume(MinerPauseEnum fromwhat);

    /**
     * @brief Retrieves currrently collected hashrate
     */
    float RetrieveHashRate() noexcept;

    void TriggerHashRateUpdate() noexcept;

protected:
    /**
     * @brief Initializes miner's device.
     */
    virtual bool initDevice() = 0;

    /**
     * @brief Initializes miner to current (or changed) epoch.
     */
    bool initEpoch();

    /**
     * @brief Miner's specific initialization to current (or changed) epoch.
     */
    virtual bool initEpoch_internal() = 0;

    /**
     * @brief Returns current workpackage this miner is working on
     */
    WorkPackage work() const;

    void updateHashRate(uint32_t _groupSize, uint32_t _increment) noexcept;

    bool dropThreadPriority();

    static unsigned s_minersCount;   // Total Number of Miners
    static unsigned s_dagLoadMode;   // Way dag should be loaded
    static unsigned s_dagLoadIndex;  // In case of serialized load of dag this is the index of miner
                                     // which should load next

    const unsigned m_index = 0;           // Ordinal index of the Instance (not the device)
    DeviceDescriptor m_deviceDescriptor;  // Info about the device

    std::shared_ptr<ethash::epoch_context> m_epochContext;

#ifdef DEV_BUILD
    std::chrono::steady_clock::time_point m_workSwitchStart;
#endif

    HwMonitorInfo m_hwmoninfo;
    mutable std::mutex x_work;
    mutable std::mutex x_pause;
    std::condition_variable m_new_work_signal;
    std::condition_variable m_dag_loaded_signal;
    uint64_t m_nextProgpowPeriod = 0;
    std::unique_ptr<std::thread> m_compileThread = nullptr;

private:
    std::bitset<MinerPauseEnum::Pause_MAX> m_pauseFlags;

    WorkPackage m_work;

    std::chrono::steady_clock::time_point m_hashTime = std::chrono::steady_clock::now();
    std::atomic<float> m_hashRate = {0.0};
    uint64_t m_groupCount = 0;
    std::atomic<bool> m_hashRateUpdate = {false};
};

}  // namespace dev::eth

#endif  // !LIBCRYPTO_MINER_H_