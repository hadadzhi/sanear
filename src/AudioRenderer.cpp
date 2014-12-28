#include "pch.h"
#include "AudioRenderer.h"

namespace SaneAudioRenderer
{
    AudioRenderer::AudioRenderer(ISettings* pSettings, IMyClock* pClock, CAMEvent& bufferFilled, HRESULT& result)
        : m_deviceManager(result)
        , m_myClock(pClock)
        , m_flush(TRUE/*manual reset*/)
        , m_dspVolume(*this)
        , m_dspBalance(*this)
        , m_bufferFilled(bufferFilled)
        , m_settings(pSettings)
    {
        if (FAILED(result))
            return;

        try
        {
            if (!m_settings || !m_myClock)
                throw E_UNEXPECTED;

            ThrowIfFailed(m_myClock->QueryInterface(IID_PPV_ARGS(&m_graphClock)));

            if (static_cast<HANDLE>(m_flush) == NULL ||
                static_cast<HANDLE>(m_bufferFilled) == NULL)
            {
                throw E_OUTOFMEMORY;
            }
        }
        catch (HRESULT ex)
        {
            result = ex;
        }
    }

    AudioRenderer::~AudioRenderer()
    {
        // Just in case.
        if (m_state != State_Stopped)
            Stop();
    }

    void AudioRenderer::SetClock(IReferenceClock* pClock)
    {
        CAutoLock objectLock(this);

        ThrowIfFailed(m_myClock->QueryInterface(IID_PPV_ARGS(&m_graphClock)));
        m_externalClock = false;

        if (pClock && m_graphClock != pClock)
        {
            m_graphClock = pClock;

            if (!m_externalClock)
            {
                m_externalClock = true;
                ClearDevice();
            }
        }
    }

    bool AudioRenderer::OnExternalClock()
    {
        CAutoLock objectLock(this);

        return m_externalClock;
    }

    bool AudioRenderer::Enqueue(IMediaSample* pSample, AM_SAMPLE2_PROPERTIES& sampleProps)
    {
        DspChunk chunk;

        {
            CAutoLock objectLock(this);
            assert(m_inputFormat);
            assert(m_state != State_Stopped);

            try
            {
                CheckDeviceSettings();

                if (!m_device)
                    CreateDevice();

                chunk = m_timingsCorrection.ProcessSample(pSample, sampleProps);

                if (m_device && m_state == State_Running)
                {
                    {
                        REFERENCE_TIME offset = m_timingsCorrection.GetTimingsError() - m_myClock->GetSlavedClockOffset();
                        if (std::abs(offset) > 1000)
                        {
                            m_myClock->OffsetSlavedClock(offset);
                            //DbgOutString((std::to_wstring(offset) + L" " + std::to_wstring(sampleProps.tStop - sampleProps.tStart) + L"\n").c_str());
                        }
                    }

                    if (m_externalClock && !m_device->bitstream)
                    {
                        assert(m_dspRate.Active());
                        REFERENCE_TIME graphTime, myTime, myStartTime;
                        if (SUCCEEDED(m_myClock->GetAudioClockStartTime(&myStartTime)) &&
                            SUCCEEDED(m_myClock->GetAudioClockTime(&myTime, nullptr)) &&
                            SUCCEEDED(m_graphClock->GetTime(&graphTime)) &&
                            myTime > myStartTime)
                        {
                            REFERENCE_TIME offset = graphTime - myTime - m_correctedWithRateDsp;
                            if (std::abs(offset) > MILLISECONDS_TO_100NS_UNITS(2))
                            {
                                //DbgOutString((std::to_wstring(offset) + L" " + std::to_wstring(m_corrected) + L"\n").c_str());
                                m_dspRate.Adjust(offset);
                                m_correctedWithRateDsp += offset;
                            }
                        }
                    }
                }

                if (m_device && !m_device->bitstream)
                {
                    auto f = [&](DspBase* pDsp)
                    {
                        pDsp->Process(chunk);
                    };

                    EnumerateProcessors(f);

                    DspChunk::ToFormat(m_device->dspFormat, chunk);
                }
            }
            catch (std::bad_alloc&)
            {
                ClearDevice();
                chunk = DspChunk();
            }
        }

        return Push(chunk);
    }

    bool AudioRenderer::Finish(bool blockUntilEnd)
    {
        DspChunk chunk;

        {
            CAutoLock objectLock(this);
            assert(m_state != State_Stopped);

            if (!m_device)
                blockUntilEnd = false;

            try
            {
                if (m_device && !m_device->bitstream)
                {
                    auto f = [&](DspBase* pDsp)
                    {
                        pDsp->Finish(chunk);
                    };

                    EnumerateProcessors(f);

                    DspChunk::ToFormat(m_device->dspFormat, chunk);
                }
            }
            catch (std::bad_alloc&)
            {
                chunk = DspChunk();
                assert(chunk.IsEmpty());
            }
        }

        auto doBlock = [this]
        {
            TimePeriodHelper timePeriodHelper(1);

            m_myClock->UnslaveClockFromAudio();

            for (;;)
            {
                int64_t actual = INT64_MAX;
                int64_t target;

                {
                    CAutoLock objectLock(this);

                    if (!m_device)
                        return true;

                    UINT64 deviceClockFrequency, deviceClockPosition;

                    try
                    {
                        ThrowIfFailed(m_device->audioClock->GetFrequency(&deviceClockFrequency));
                        ThrowIfFailed(m_device->audioClock->GetPosition(&deviceClockPosition, nullptr));
                    }
                    catch (HRESULT)
                    {
                        ClearDevice();
                        return true;
                    }

                    const auto previous = actual;
                    actual = llMulDiv(deviceClockPosition, OneSecond, deviceClockFrequency, 0);
                    target = llMulDiv(m_pushedFrames, OneSecond, m_device->waveFormat->nSamplesPerSec, 0);

                    if (actual == target)
                        return true;

                    if (actual == previous && m_state == State_Running)
                        return true;
                }

                if (m_flush.Wait(std::max(1, (int32_t)((target - actual) * 1000 / OneSecond))))
                    return false;
            }
        };

        return Push(chunk) && (!blockUntilEnd || doBlock());
    }

    void AudioRenderer::BeginFlush()
    {
        m_flush.Set();
    }

    void AudioRenderer::EndFlush()
    {
        CAutoLock objectLock(this);
        assert(m_state != State_Running);

        if (m_device)
        {
            m_device->audioClient->Reset();
            m_bufferFilled.Reset();
        }

        m_flush.Reset();

        m_pushedFrames = 0;
    }

    bool AudioRenderer::CheckFormat(SharedWaveFormat inputFormat)
    {
        assert(inputFormat);

        if (DspFormatFromWaveFormat(*inputFormat) != DspFormat::Unknown)
            return true;

        BOOL exclusive;
        m_settings->GetOuputDevice(nullptr, &exclusive);
        BOOL bitstreamingAllowed;
        m_settings->GetAllowBitstreaming(&bitstreamingAllowed);

        if (!exclusive || !bitstreamingAllowed)
            return false;

        CAutoLock objectLock(this);

        return m_deviceManager.BitstreamFormatSupported(inputFormat, m_settings);
    }

    void AudioRenderer::SetFormat(SharedWaveFormat inputFormat)
    {
        CAutoLock objectLock(this);

        m_inputFormat = inputFormat;

        m_timingsCorrection.SetFormat(inputFormat);

        ClearDevice();
    }

    void AudioRenderer::NewSegment(double rate)
    {
        CAutoLock objectLock(this);

        // It makes things a lot easier when rate is within float precision,
        // please add a cast to your player's code.
        assert((double)(float)rate == rate);

        m_startClockOffset = 0;
        m_rate = rate;

        m_timingsCorrection.NewSegment(m_rate);

        assert(m_inputFormat);
        if (m_device)
            InitializeProcessors();
    }

    void AudioRenderer::Play(REFERENCE_TIME startTime)
    {
        CAutoLock objectLock(this);
        assert(m_state != State_Running);
        m_state = State_Running;

        m_startTime = startTime;
        StartDevice();
    }

    void AudioRenderer::Pause()
    {
        CAutoLock objectLock(this);
        m_state = State_Paused;

        if (m_device)
        {
            m_myClock->UnslaveClockFromAudio();
            m_device->audioClient->Stop();
        }
    }

    void AudioRenderer::Stop()
    {
        CAutoLock objectLock(this);
        m_state = State_Stopped;

        ClearDevice();
    }

    SharedWaveFormat AudioRenderer::GetInputFormat()
    {
        CAutoLock objectLock(this);

        return m_inputFormat;
    }

    SharedAudioDevice AudioRenderer::GetAudioDevice()
    {
        CAutoLock objectLock(this);

        return m_device;
    }

    std::vector<std::wstring> AudioRenderer::GetActiveProcessors()
    {
        CAutoLock objectLock(this);

        std::vector<std::wstring> ret;

        if (m_inputFormat && m_device && !m_device->bitstream)
        {
            auto f = [&](DspBase* pDsp)
            {
                if (pDsp->Active())
                    ret.emplace_back(pDsp->Name());
            };

            EnumerateProcessors(f);
        }

        return ret;
    }

    void AudioRenderer::CheckDeviceSettings()
    {
        CAutoLock objectLock(this);

        UINT32 serial = m_settings->GetSerial();

        if (m_device && m_device->settingsSerial != serial)
        {
            LPWSTR pDeviceName = nullptr;
            BOOL exclusive;
            if (SUCCEEDED(m_settings->GetOuputDevice(&pDeviceName, &exclusive)))
            {
                if (m_device->exclusive != !!exclusive ||
                    (pDeviceName && *pDeviceName && wcscmp(pDeviceName, m_device->friendlyName->c_str())) ||
                    ((!pDeviceName || !*pDeviceName) && !m_device->default))
                {
                    ClearDevice();
                    assert(!m_device);
                }
                else
                {
                    m_device->settingsSerial = serial;
                }
                CoTaskMemFree(pDeviceName);
            }
        }
    }

    void AudioRenderer::StartDevice()
    {
        CAutoLock objectLock(this);
        assert(m_state == State_Running);

        if (m_device)
        {
            m_myClock->SlaveClockToAudio(m_device->audioClock, m_startTime + m_startClockOffset);
            m_startClockOffset = 0;
            m_device->audioClient->Start();
            //assert(m_bufferFilled.Check());
        }
    }

    void AudioRenderer::CreateDevice()
    {
        CAutoLock objectLock(this);

        assert(!m_device);
        assert(m_inputFormat);

        m_device = m_deviceManager.CreateDevice(m_inputFormat, m_settings);

        if (m_device)
        {
            InitializeProcessors();

            m_startClockOffset = m_timingsCorrection.GetLastSampleEnd();

            if (m_state == State_Running)
                StartDevice();
        }
    }

    void AudioRenderer::ClearDevice()
    {
        CAutoLock objectLock(this);

        if (m_device)
        {
            m_myClock->UnslaveClockFromAudio();
            m_device->audioClient->Stop();
            m_bufferFilled.Reset();
        }

        m_device = nullptr;
        m_deviceManager.ReleaseDevice();

        m_pushedFrames = 0;
    }

    void AudioRenderer::InitializeProcessors()
    {
        CAutoLock objectLock(this);
        assert(m_inputFormat);
        assert(m_device);

        m_correctedWithRateDsp = 0;

        if (m_device->bitstream)
            return;

        const auto inRate = m_inputFormat->nSamplesPerSec;
        const auto inChannels = m_inputFormat->nChannels;
        const auto inMask = DspMatrix::GetChannelMask(*m_inputFormat);
        const auto outRate = m_device->waveFormat->nSamplesPerSec;
        const auto outChannels = m_device->waveFormat->nChannels;
        const auto outMask = DspMatrix::GetChannelMask(*m_device->waveFormat);

        m_dspMatrix.Initialize(inChannels, inMask, outChannels, outMask);
        m_dspRate.Initialize(m_externalClock, inRate, outRate, outChannels);
        m_dspTempo.Initialize((float)m_rate, outRate, outChannels);
        m_dspCrossfeed.Initialize(m_settings, outRate, outChannels, outMask);
        m_dspVolume.Initialize(m_device->exclusive);
        m_dspLimiter.Initialize(m_settings, outRate, m_device->exclusive);
        m_dspDither.Initialize(m_device->dspFormat);
    }

    bool AudioRenderer::Push(DspChunk& chunk)
    {
        if (chunk.IsEmpty())
            return true;

        const uint32_t frameSize = chunk.GetFrameSize();
        const size_t chunkFrames = chunk.GetFrameCount();

        bool firstIteration = true;
        for (size_t doneFrames = 0; doneFrames < chunkFrames;)
        {
            // The device buffer is full or almost full at the beginning of the second and subsequent iterations.
            // Sleep until the buffer may have significant amount of free space. Unless interrupted.
            if (!firstIteration && m_flush.Wait(50))
                return false;

            firstIteration = false;

            CAutoLock objectLock(this);

            assert(m_state != State_Stopped);

            if (m_device)
            {
                try
                {
                    // Get up-to-date information on the device buffer.
                    UINT32 bufferFrames, bufferPadding;
                    ThrowIfFailed(m_device->audioClient->GetBufferSize(&bufferFrames));
                    ThrowIfFailed(m_device->audioClient->GetCurrentPadding(&bufferPadding));

                    // Find out how many frames we can write this time.
                    const UINT32 doFrames = std::min(bufferFrames - bufferPadding, (UINT32)(chunkFrames - doneFrames));

                    if (doFrames == 0)
                        continue;

                    // Write frames to the device buffer.
                    BYTE* deviceBuffer;
                    ThrowIfFailed(m_device->audioRenderClient->GetBuffer(doFrames, &deviceBuffer));
                    assert(frameSize == (m_device->waveFormat->wBitsPerSample / 8 * m_device->waveFormat->nChannels));
                    memcpy(deviceBuffer, chunk.GetConstData() + doneFrames * frameSize, doFrames * frameSize);
                    ThrowIfFailed(m_device->audioRenderClient->ReleaseBuffer(doFrames, 0));

                    // If the buffer is fully filled, set the corresponding event.
                    (bufferPadding + doFrames == bufferFrames) ? m_bufferFilled.Set() : m_bufferFilled.Reset();

                    doneFrames += doFrames;
                    m_pushedFrames += doFrames;

                    continue;
                }
                catch (HRESULT)
                {
                    ClearDevice();
                }
            }

            assert(!m_device);
            m_bufferFilled.Set();
            REFERENCE_TIME graphTime;
            if (m_state == State_Running &&
                SUCCEEDED(m_graphClock->GetTime(&graphTime)) &&
                graphTime + MILLISECONDS_TO_100NS_UNITS(20) > m_startTime + m_timingsCorrection.GetLastSampleEnd())
            {
                break;
            }
        }

        return true;
    }
}
