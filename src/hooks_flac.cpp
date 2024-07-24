#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <mmiscapi.h>
#include <mmreg.h>
#include <fstream>
#include <FLAC/stream_decoder.h>

class CWaveFile
{
public:
    virtual ~CWaveFile() = 0;
    virtual HRESULT Open(LPSTR strFileName, WAVEFORMATEX* a3, DWORD dwFlags) = 0;
    virtual HRESULT OpenFromMemory(BYTE* pbData, ULONG ulDataSize, WAVEFORMATEX* pwfx, DWORD dwFlags) = 0;
    virtual HRESULT Close() = 0;
    virtual HRESULT Read(BYTE* pBuffer, DWORD dwSizeToRead, DWORD* pdwSizeRead) = 0;
    virtual HRESULT Write(UINT nSizeToWrite, BYTE* pbSrcData, UINT* pnSizeWrote) = 0;
    virtual int GetSize() = 0;
    virtual HRESULT ResetFile() = 0;

    WAVEFORMATEX* m_pwfx_4;
    DWORD m_dwSize_8;
    HMMIO m_hmmio_C;
    MMCKINFO m_ck_10;
    MMCKINFO m_ckRiff_24;
    MMIOINFO m_mmioinfoOut_38;
    DWORD m_dwFlags_80;
    BOOL m_bIsReadingFromMemory_84;
    BYTE* m_pbData_88;
    BYTE* m_pbDataCur_8C;
    ULONG m_ulDataSize_90;
    CHAR* m_pResourceBuffer_94;
};

CWaveFile::~CWaveFile() {}

class CFLACFile : public CWaveFile
{
public:
    CFLACFile();
    ~CFLACFile();
    HRESULT Open(LPSTR strFileName, WAVEFORMATEX* a3, DWORD dwFlags);
    HRESULT OpenFromMemory(BYTE* pbData, ULONG ulDataSize, WAVEFORMATEX* pwfx, DWORD dwFlags);
    HRESULT Close();
    HRESULT Read(BYTE* pBuffer, DWORD dwSizeToRead, DWORD* pdwSizeRead);
    HRESULT Write(UINT nSizeToWrite, BYTE* pbSrcData, UINT* pnSizeWrote);
    int GetSize();
    HRESULT ResetFile();

private:
    FLAC__StreamDecoder* m_pDecoder;
    std::vector<BYTE> m_pDecodedData;
    DWORD m_dwDecodedDataSize;
    DWORD m_dwCurrentPosition;

    FLAC__uint64 m_currentSample;

    bool m_loopEnabled;
    FLAC__uint64 m_loopStart;
    FLAC__uint64 m_loopEnd;

    // FLAC callbacks
    static FLAC__StreamDecoderWriteStatus WriteCallback(const FLAC__StreamDecoder* decoder, const FLAC__Frame* frame, const FLAC__int32* const buffer[], void* client_data);
    static void MetadataCallback(const FLAC__StreamDecoder* decoder, const FLAC__StreamMetadata* metadata, void* client_data);
    static void ErrorCallback(const FLAC__StreamDecoder* decoder, FLAC__StreamDecoderErrorStatus status, void* client_data);

    // Helper functions
    HRESULT DecodeFile();
    void EnsureBufferSize(size_t requiredSize);
    bool SeekToSample(FLAC__uint64 sample);
};

CFLACFile::CFLACFile() : m_pDecoder(NULL), m_dwDecodedDataSize(0), m_dwCurrentPosition(0), m_loopEnabled(false), m_currentSample(0)
{
}

CFLACFile::~CFLACFile()
{
    Close();
}

HRESULT CFLACFile::Open(LPSTR strFileName, WAVEFORMATEX* pwfx, DWORD dwFlags)
{
    // Initialize FLAC decoder
    m_pDecoder = FLAC__stream_decoder_new();
    if (m_pDecoder == NULL)
        return E_FAIL;

    // Set up callbacks
    FLAC__stream_decoder_set_metadata_ignore_all(m_pDecoder);
    FLAC__stream_decoder_set_metadata_respond(m_pDecoder, FLAC__METADATA_TYPE_STREAMINFO);
    FLAC__stream_decoder_set_metadata_respond(m_pDecoder, FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__stream_decoder_init_file(m_pDecoder, strFileName, WriteCallback, MetadataCallback, ErrorCallback, this);

    // Decode the entire file
    return DecodeFile();
}

HRESULT CFLACFile::OpenFromMemory(BYTE* pbData, ULONG ulDataSize, WAVEFORMATEX* pwfx, DWORD dwFlags)
{
    OutputDebugString("CFLACFile::OpenFromMemory not implementamundo");
    return 0;
}

HRESULT CFLACFile::Read(BYTE* pBuffer, DWORD dwSizeToRead, DWORD* pdwSizeRead)
{
    if (!m_pDecodedData.size())
        return E_FAIL;

    DWORD dwRead = 0;
    DWORD dwRemaining = dwSizeToRead;

    while (dwRemaining > 0)
    {
        // Check if we've reached the loop end
        if (m_loopEnabled && m_currentSample >= m_loopEnd)
        {
            // Seek back to loop start
            if (!SeekToSample(m_loopStart))
                break;
        }

        DWORD numRead = dwRemaining;
        if (m_loopEnabled)
        {
            FLAC__uint64 samplesUntilLoopEnd = m_loopEnd - m_currentSample;
            DWORD bytesUntilLoopEnd = static_cast<DWORD>(samplesUntilLoopEnd * m_pwfx_4->nBlockAlign);
            numRead = min(bytesUntilLoopEnd, numRead);
        }

        while (m_dwCurrentPosition + numRead > m_dwDecodedDataSize)
        {
            if (FLAC__stream_decoder_get_state(m_pDecoder) == FLAC__STREAM_DECODER_END_OF_STREAM)
                break;

            // Try to read two frames at a time, hopefully reduce any skipping...
            if (!FLAC__stream_decoder_process_single(m_pDecoder))
                break;

            if (!FLAC__stream_decoder_process_single(m_pDecoder))
                break;
        }

        DWORD dwAvailable = m_dwDecodedDataSize - m_dwCurrentPosition;
        DWORD dwToRead = min(dwAvailable, numRead);
        if (dwToRead == 0)
            break;

        memcpy(pBuffer + dwRead, m_pDecodedData.data() + m_dwCurrentPosition, dwToRead);
        m_dwCurrentPosition += dwToRead;
        m_currentSample += dwToRead / m_pwfx_4->nBlockAlign;
        dwRead += dwToRead;
        dwRemaining -= dwToRead;
    }

    if (pdwSizeRead)
        *pdwSizeRead = dwRead;

    return S_OK;
}

bool CFLACFile::SeekToSample(FLAC__uint64 sample)
{
    m_currentSample = sample;
    m_dwCurrentPosition = static_cast<DWORD>(sample * m_pwfx_4->nBlockAlign);
    return true;
}

HRESULT CFLACFile::Write(UINT nSizeToWrite, BYTE* pbSrcData, UINT* pnSizeWrote)
{
    OutputDebugString("Negatory on the CFLACFile::Write");
    return 0;
}

int CFLACFile::GetSize()
{
    return this->m_dwSize_8;
}

HRESULT CFLACFile::Close()
{
    if (m_pDecoder)
    {
        FLAC__stream_decoder_finish(m_pDecoder);
        FLAC__stream_decoder_delete(m_pDecoder);
        m_pDecoder = NULL;
    }

    m_pDecodedData.clear();
    m_dwDecodedDataSize = 0;
    m_dwCurrentPosition = 0;

    return S_OK;
}

HRESULT CFLACFile::ResetFile()
{
    m_dwCurrentPosition = 0;
    return S_OK;
}

HRESULT CFLACFile::DecodeFile()
{
    if (!FLAC__stream_decoder_process_until_end_of_metadata(m_pDecoder))
        return E_FAIL;

    if (!FLAC__stream_decoder_process_single(m_pDecoder))
        return E_FAIL;

    return S_OK;
}

void CFLACFile::EnsureBufferSize(size_t requiredSize)
{
    if (m_pDecodedData.size() < requiredSize)
    {
        size_t newCapacity = m_pDecodedData.size();
        if (!newCapacity)
            newCapacity = requiredSize;
        while (newCapacity < requiredSize)
            newCapacity *= 2;  // Double the capacity
        m_pDecodedData.resize(newCapacity);
    }
}

FLAC__StreamDecoderWriteStatus CFLACFile::WriteCallback(const FLAC__StreamDecoder* decoder, const FLAC__Frame* frame, const FLAC__int32* const buffer[], void* client_data)
{
    CFLACFile* pThis = static_cast<CFLACFile*>(client_data);

    const unsigned int bps = frame->header.bits_per_sample;
    const DWORD totalSamples = frame->header.blocksize * frame->header.channels;
    const DWORD bytesPerSample = bps / 8;
    const DWORD totalBytes = totalSamples * bytesPerSample;

    // Ensure buffer is large enough
    size_t requiredSize = pThis->m_dwDecodedDataSize + totalBytes;
    pThis->EnsureBufferSize(requiredSize);

    // Append decoded FLAC samples to m_pDecodedData
    unsigned char* outBuffer = pThis->m_pDecodedData.data() + pThis->m_dwDecodedDataSize;

    for (unsigned int i = 0; i < frame->header.blocksize; i++)
    {
        for (unsigned int channel = 0; channel < frame->header.channels; channel++)
        {
            FLAC__int32 sample = buffer[channel][i];

            // Handle different bit depths
            switch (bps) {
            case 16:
            {
                INT16 pcm = static_cast<INT16>(max(-32768, min(32767, sample)));
                *(INT16*)outBuffer = pcm;
                outBuffer += sizeof(INT16);
            }
            break;
            case 24:
            {
                outBuffer[0] = (sample & 0xFF);
                outBuffer[1] = ((sample >> 8) & 0xFF);
                outBuffer[2] = ((sample >> 16) & 0xFF);
                outBuffer += 3;
            }
            break;
            case 32:
                *(FLAC__int32*)outBuffer = sample;
                outBuffer += sizeof(FLAC__int32);
                break;
            default:
                // Unsupported bit depth
                return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            }
        }
    }

    pThis->m_dwDecodedDataSize += totalBytes;

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void CFLACFile::MetadataCallback(const FLAC__StreamDecoder* decoder, const FLAC__StreamMetadata* metadata, void* client_data)
{
    CFLACFile* pThis = static_cast<CFLACFile*>(client_data);

    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO)
    {
        // Fill in WAVEFORMATEX structure
        pThis->m_pwfx_4 = new WAVEFORMATEX;
        pThis->m_pwfx_4->wFormatTag = WAVE_FORMAT_PCM;
        pThis->m_pwfx_4->nChannels = metadata->data.stream_info.channels;
        pThis->m_pwfx_4->nSamplesPerSec = metadata->data.stream_info.sample_rate;
        pThis->m_pwfx_4->wBitsPerSample = metadata->data.stream_info.bits_per_sample;
        pThis->m_pwfx_4->nBlockAlign = (pThis->m_pwfx_4->nChannels * pThis->m_pwfx_4->wBitsPerSample) / 8;
        pThis->m_pwfx_4->nAvgBytesPerSec = pThis->m_pwfx_4->nSamplesPerSec * pThis->m_pwfx_4->nBlockAlign;
        pThis->m_pwfx_4->cbSize = 0;

        // Pre-allocate buffer for decoded data
        FLAC__uint64 totalSamples = metadata->data.stream_info.total_samples;
        unsigned channels = metadata->data.stream_info.channels;
        unsigned bitsPerSample = metadata->data.stream_info.bits_per_sample;

        // Calculate total buffer size in bytes
        size_t totalBufferSize = totalSamples * channels * (bitsPerSample / 8);

        // Pre-allocate the buffer
        pThis->m_pDecodedData.reserve(totalBufferSize);
    }
    else if (metadata->type == FLAC__METADATA_TYPE_VORBIS_COMMENT)
    {
        std::optional<FLAC__uint64> loop_start;
        std::optional<FLAC__uint64> loop_length;
        std::optional<FLAC__uint64> loop_end;
        for (unsigned int i = 0; i < metadata->data.vorbis_comment.num_comments; i++)
        {
            const char* tag = (const char*)metadata->data.vorbis_comment.comments[i].entry;
            unsigned int length = metadata->data.vorbis_comment.comments[i].length;
            spdlog::debug("FLAC comment: size {}, {}", length, tag);

            if (length > 10 && !_strnicmp(tag, "LOOPSTART=", 10))
                loop_start = std::stoull(tag + 10, NULL, 10);
            else if (length > 11 && !_strnicmp(tag, "LOOPLENGTH=", 11))
                loop_length = std::stoull(tag + 11, NULL, 10);
            else if (length > 8 && !_strnicmp(tag, "LOOPEND=", 8))
                loop_end = std::stoull(tag + 8, NULL, 10);
        }

        if (loop_start.has_value() && loop_length.has_value())
            loop_end = loop_start.value() + loop_length.value() - 1;

        if (loop_start.has_value() && loop_end.has_value())
        {
            spdlog::info("FLAC loop: {} - {}", loop_start.value(), loop_end.value());
            pThis->m_loopStart = loop_start.value();
            pThis->m_loopEnd = loop_end.value();
            pThis->m_loopEnabled = true;
        }
    }
}

void CFLACFile::ErrorCallback(const FLAC__StreamDecoder* decoder, FLAC__StreamDecoderErrorStatus status, void* client_data)
{
    // Log the error
    OutputDebugString("FLAC decoding error: ");
    OutputDebugString(FLAC__StreamDecoderErrorStatusString[status]);
    OutputDebugString("\n");
}

class AllowFLAC : public Hook
{
    inline static SafetyHookMid hook = {};
    static void destination(safetyhook::Context& ctx)
    {
        if (ctx.eax == 2)
        {
            CFLACFile* file = new CFLACFile();
            ctx.eax = (uintptr_t)file;

            ctx.eip = 0x412008; // the code we hook heads toward end of function, move it back to the file loading part
        }
    }

public:
    std::string_view description() override
    {
        return "AllowFLAC";
    }

    bool validate() override
    {
        return Settings::AllowFLAC;
    }

    bool apply() override
    {
        hook = safetyhook::create_mid(Module::exe_ptr(0x120F4), destination);
        return !!hook;
    }

    static AllowFLAC instance;
};
AllowFLAC AllowFLAC::instance;
