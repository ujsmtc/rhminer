/**
 * RandomHash source code implementation
 *
 * Copyright 2018 Polyminer1 <https://github.com/polyminer1>
 *
 * To the extent possible under law, the author(s) have dedicated all copyright
 * and related and neighboring rights to this software to the public domain
 * worldwide. This software is distributed without any warranty.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along with
 * this software. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
 */
///
/// @file
/// @copyright Polyminer1

#include "precomp.h"
#include "Farm.h"
#include "corelib/PascalWork.h"
#include "MinersLib/Global.h"
#include "MinersLib/GpuManager.h"

void Farm::SetWork(PascalWorkSptr _wp)
{
	Guard l(m_farmData.m_workMutex);
    if (m_farmData.m_work.get() && m_farmData.m_work->IsSame(_wp.get()))
    {
        //resume miners if their where paused
	    for (auto const& m: m_miners)
        {
		    m->SetWork(m_farmData.m_work);
        }
    }
    else
    {
	    m_farmData.m_work = _wp;
	    for (auto const& m: m_miners)
        {
		    m->SetWork(m_farmData.m_work);
        }
	    resetTimer();
    }
}

void Farm::SetWorkpackageDirty()
{
	Guard l(m_farmData.m_workMutex);
	for (auto const& m: m_miners)
    {
        m->SetWorkpackageDirty();
    }
}

bool Farm::start()
{
    //Pre init
    {
        Guard l(m_minerWorkMutex);
        if (m_isMining)
            return true;

        m_farmData.m_solutionStats.Begin();

	    if (!m_miners.empty())
        {
            PrintOut("Error. Atempting to start miners while some are still running.");
            return true;
        }
    }

    U32 globalIndex = 0;

    /////////////////////////////////////////////////////
    //Allocate ALL miners
    vector<std::shared_ptr<Miner>> newMiners;
    for (auto& gpu : GpuManager::Gpus)
    {
        RHMINER_RETURN_ON_EXIT_FLAG_EX(false);
        if (gpu.enabled)
        {
            RHMINER_ASSERT(gpu.initialized);

            GlobalMiningPreset::CreatorClasType createType = GlobalMiningPreset::ClassOpenCL;
            gpu.gpuName = FormatString("GPU%d", globalIndex);
            if (RHMINER_TEST_BIT(gpu.gpuType, GpuType_NVIDIA))
                createType = GlobalMiningPreset::ClassNvidia;
            else if (RHMINER_TEST_BIT(gpu.gpuType, GpuType_AMD))
                createType = GlobalMiningPreset::ClassOpenCL;
            else if (RHMINER_TEST_BIT(gpu.gpuType, GpuType_CPU))
            {
                createType = GlobalMiningPreset::ClassCPU;
                gpu.gpuName = "CPU";
            }

            if (RHMINER_TEST_BIT(gpu.gpuType, GpuType_AMD))
            {
                PrintOutCritical("Error. No kernel for device %d '%s'\n", gpu.deviceID, gpu.deviceName.c_str());
            }
            else
            {
                std::shared_ptr<Miner> newMiner = std::shared_ptr<Miner>(GlobalMiningPreset::I().CreateMiner(createType, *this, globalIndex));
                newMiner->UpdateWorkSize(0);
                newMiners.push_back(newMiner);
            }
        }
        globalIndex++;
    }

    //Miners initialization creation
    {
        Guard l(m_minerWorkMutex);
        m_miners.reserve(newMiners.size()+1);

        for (auto& miner : newMiners)
        {
            miner->InitFromFarm((U32)m_miners.size());
		    m_miners.push_back(miner);

		    // Start miners' threads. They should pause waiting for new work
		    // package.
		    m_miners.back()->StartWorking();
        }

        m_minersCount = (unsigned)m_miners.size();
        if (!m_minersCount)
        {
            PrintOutCritical("No cpu/gpu selected\n");
            exit(0);
        }

	    m_isMining = true;
	    resetTimer();
    }

	return true;
}

bool Farm::IsOneMinerInitializing()
{
    Guard l(m_minerWorkMutex);
    for (auto i : m_miners)
        if (i->isInitializing())
            return true; 
    return false;
}

void Farm::Pause()
{
    Guard l(m_minerWorkMutex);
    for (auto&m : m_miners)
        m->Pause();
}

void Farm::PauseCpuMiners()
{
    Guard l(m_minerWorkMutex);
    for (auto&m : m_miners)
        if (m->GetPlatformType() == PlatformType_CPU)
            m->Pause();
}
	
void Farm::stop()
{
    {
        Guard l2(m_sumbitMutex);
        PurgeThreadInternal();
        CpuSleep(1000);
    }
    
    Guard l(m_minerWorkMutex);
    internalStop();
}

void Farm::internalStop()
{
    if (!m_isMining)
        return;

    for (auto&m : m_miners)
        m->Kill();

    m_miners.clear();
    m_isMining = false;
}


bool Farm::HasOneCPUMiner()
{
    Guard l2(m_minerWorkMutex);
	for (auto const& i: m_miners)
	{
        if (i->GetPlatformType() == PlatformType_CPU)
        {
            return true;
        }
    }

    return false;
}


bool Farm::DetectDeadMiners()
{
    Guard l2(m_minerWorkMutex);
    unsigned deadCount = 0;
	for (auto const& i: m_miners)
	{
        if (i->isStopped())
            deadCount++;
    }
    if (m_miners.size() == deadCount && deadCount)
    {
        internalStop();
        return true;
    }
    return false;
}

WorkingProgress const& Farm::miningProgress(bool reset)
{
	WorkingProgress p;
	{
        Guard l2(m_minerWorkMutex);
        if (!m_lastProgressTime)
            m_lastProgressTime = TimeGetMilliSec();
        U32 dt = TimeGetMilliSec() - m_lastProgressTime;
        if (dt < 100)
            dt = 100;
        m_lastProgressTime = TimeGetMilliSec();

        unsigned deadCount = 0;
		for (int cnt = 0; cnt < m_miners.size(); cnt++)
		{
            if (m_miners[cnt]->isStopped())
                deadCount++;
            
            U64 hcount = m_miners[cnt]->GetHashRatePerSec();
            U32 minerHashRate = (U64)round(hcount /(float)(dt/1000.0f));

            p.totalHashRate += minerHashRate;
            p.minersHasheRate.push_back(minerHashRate);
            p.gpuGlobalIndex.push_back(m_miners[cnt]->getAbsoluteIndex());

            p.acceptedShares.push_back(m_farmData.m_solutionStats.getAccepted(m_miners[cnt]->getAbsoluteIndex()));
            p.rejectedShares.push_back(m_farmData.m_solutionStats.getRejected(m_miners[cnt]->getAbsoluteIndex()));

            U32 t, f;
            m_miners[cnt]->GetTemp(t, f);
            p.temperature.push_back(t);
            p.fan.push_back(f);
		}

        if (m_miners.size() == deadCount && deadCount)
            internalStop();
    }

	Guard l(m_farmData.m_progressMutex);
    if (m_farmData.m_minersHasheRatePeak.size() != p.minersHasheRate.size())
    {
        m_farmData.m_minersHasheRatePeak.clear();
        for (auto a : p.minersHasheRate)
            m_farmData.m_minersHasheRatePeak.push_back(0);
    }

    for (unsigned i = 0; i < p.minersHasheRate.size(); i++)
    {
        auto rate = p.minersHasheRate[i];
        if (rate > m_farmData.m_minersHasheRatePeak[i])
            m_farmData.m_minersHasheRatePeak[i] = rate;
    }
	m_farmData.m_progress = p;
    m_farmData.m_progress.minersHasheRatePeak = m_farmData.m_minersHasheRatePeak;
	return m_farmData.m_progress;
}

void Farm::ReconnectToServer(uint32_t gpuAbsIndex)
{
    RHMINER_ASSERT(m_reconnect);
    m_reconnect(gpuAbsIndex);
}


void Farm::RequestNewWork(PascalWorkSptr wp, GenericCLMiner* miner)
{
    RHMINER_ASSERT(m_requestNeWork);
    m_requestNeWork(wp, miner);
}

MinerSptr Farm::GetCPUMiner()
{
    //find the cpu miner
    Guard l(m_minerWorkMutex);
    for (auto i : m_miners)
    {
        if (i->GetPlatformType() == PlatformType_CPU)
            return i;
    }

    return MinerSptr(0);
}

void Farm::submitProof(SolutionSptr sol)
{
   RHMINER_ASSERT(m_onSolutionFound);

    m_submitID++;
    uint32_t id = m_submitID;    
    std::atomic<bool> started;
    started = false;
    U64 lt = TimeGetMilliSec();
        
    //launch m_onSolutionFound in an autothread
    auto t = new std::thread(([&]
    {
        RH_SetThreadPriority(RH_ThreadPrio_High);

        //Because we can find a none while other GPU are initializing. without this init crashes
        extern std::mutex*  gs_sequentialBuildMutex;
        std::lock_guard<std::mutex> g(*gs_sequentialBuildMutex);

        setThreadName("Send");
        SolutionSptr solLocal = sol;
        uint32_t idlocal = id;
        
        //wait until the sptr is increfed
        started = true;
        try
        {
            m_onSolutionFound(solLocal);
        }
        catch (...)
        {
            PrintOut("Exception caught in Farm::submitProof. Submit aborted...\n");
        }

        //prevent this thread from ending before being put in m_submitters to late
        CpuSleep(100);

        Guard l(m_sumbitMutex);
        auto fnd = m_submiters.find(idlocal);
        if (fnd != m_submiters.end())
        {
            fnd->second.second = true;
        }
        solLocal.reset();
    }));

    while(!started)
        CpuSleep(20);
        
    {
        Guard l(m_sumbitMutex);
        m_submiters[id] = { t, false };
        t->detach();

        //purge old threads
        PurgeThreadInternal();
    }
}

void Farm::PurgeThreadInternal()
{
    if (m_submiters.size())
    {
        std::map<unsigned, std::pair<std::thread*, bool>> newsub;
        for (auto it : m_submiters)
        {
            if (it.second.second)
            {
                try
                {
                    delete it.second.first;
                    it.second.first = 0;
                }
                catch (...)
                {
                }
            }
            else
                newsub[it.first] = it.second;
        }
        m_submiters = newsub;
    }
}

extern int g_maxConsecutiveSubmitErrors;
void Farm::AddAcceptedSolution(int gpuAbsIndex) 
{ 
    m_farmData.m_solutionStats.accepted(gpuAbsIndex); 
    m_farmData.m_consecutiveRejectedCount = 0;
}
    
void Farm::AddRejectedSolution(int gpuAbsIndex) 
{ 
    m_farmData.m_solutionStats.rejected(gpuAbsIndex); 

    if (m_farmData.m_lastRejectedTimeMS &&
        (TimeGetMilliSec() - m_farmData.m_lastRejectedTimeMS) > 5 * 60000)
    {
        m_farmData.m_consecutiveRejectedCount = 0;
    }
    else
    {
        m_farmData.m_consecutiveRejectedCount++;
        m_farmData.m_lastRejectedTimeMS = TimeGetMilliSec();
        if (m_farmData.m_consecutiveRejectedCount >= (U32)g_maxConsecutiveSubmitErrors)
        {
            RHMINER_EXIT_APP("To many consecutive submit errors.");
        }
    }
}
