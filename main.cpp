// ============================================================================
// AHX TRACKER v2.7 - ABYSS / DEXTER & PINK / QST NATIVE
// ============================================================================

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

constexpr int NATIVE_W           = 320;
constexpr int NATIVE_H           = 256;

constexpr int MAX_CHANNELS       = 4;
constexpr int VISIBLE_CHANNELS   = 4;
constexpr int MAX_SEQUENCES      = 100;  // 100 POS (000..099)
constexpr int MAX_PATTERNS       = 256;  // Independent pattern pool (up to 256 unique channel tracks)
constexpr int MAX_TRACK_LEN      = 64;   // TRL rows per pattern (up to 64)
constexpr int MAX_INSTRUMENTS    = 64;
constexpr int MAX_PERF_STEPS     = 64;
constexpr int MAX_SUBSONGS       = 16;

enum ScreenMode {
    SCREEN_MAIN = 0,
    SCREEN_INSTRUMENT,
    SCREEN_PREFS,
    SCREEN_THEME_FILTER,
    SCREEN_AUDIO_HELP,
    SCREEN_SAMPLE_EDITOR,
    SCREEN_FLOPPY_BROWSER
};

enum FloppyBrowserMode {
    FLOPPY_SONGS = 0,
    FLOPPY_INSTRUMENTS = 1
};

enum AmigaFilterModel {
    FILTER_OFF = 0,
    FILTER_A500,
    FILTER_A1200,
    FILTER_LED_LPF
};

enum AmigaPointerStyle {
    PTR_WB1X = 0,
    PTR_WB2X,
    PTR_MAGIC_MOUSE
};

static const int kBufferSizes[6] = { 64, 128, 256, 512, 1024, 2048 };
static const int kSampleRates[4] = { 44100, 48000, 88200, 96000 };

// ============================================================================
// CONFIGURABLE THEME COLORS
// ============================================================================
struct ThemeColors {
    COLORREF colBlack      = RGB(0, 0, 0);
    COLORREF colWhite      = RGB(255, 255, 255);
    COLORREF colGrayMain   = RGB(136, 136, 136);
    COLORREF colGrayLight  = RGB(170, 170, 170);
    COLORREF colGrayDark   = RGB(85, 85, 85);
    COLORREF colBlueMeter  = RGB(0, 119, 187);
    COLORREF colTealPerf   = RGB(0, 153, 136);
};

static ThemeColors g_theme;

void SaveThemeConfig() {
    std::ofstream f("ahx_theme.cfg", std::ios::binary);
    if (f) f.write(reinterpret_cast<const char*>(&g_theme), sizeof(g_theme));
}

void LoadThemeConfig() {
    std::ifstream f("ahx_theme.cfg", std::ios::binary);
    if (f) f.read(reinterpret_cast<char*>(&g_theme), sizeof(g_theme));
}

// ============================================================================
// MUSICAL STRUCTURES, SAMPLES & PREFS
// ============================================================================
struct PatternCell {
    uint8_t note = 0;       // 0 = ---, 1..97 = C-0 .. C-8
    uint8_t instrument = 0; // 0..63
    uint8_t fxCmd = 0;      // 0..F
    uint8_t fxParam = 0;    // 00..64 (00 = OFF, 64 = 100% ON)
};

struct TrackPattern {
    PatternCell rows[MAX_TRACK_LEN]{};
};

struct TrackEntry {
    uint8_t patternIndex = 0; // 0..255 (strictly unique per sequence & channel)
    int8_t  transpose = 0;
};

struct SubSong {
    uint16_t startPos = 0;
};

struct PerfStep {
    uint8_t  note = 0;
    bool     fixedNote = false;
    uint8_t  waveform = 0; // 0=Hold, 1=Tri, 2=Saw, 3=Square, 4=WhiteNoise
    uint16_t fx1 = 0;
    uint16_t fx2 = 0;
};

struct SampleData {
    int bitDepth = 16;          // Exact native bit depth: 4..32 bits per sample
    uint32_t sampleRate = 44100;
    std::vector<float> floatData;
    bool loopEnabled = false;
    size_t loopStart = 0;
    size_t loopEnd = 0;
    float ampPercent = 100.0f;
    float pan = 0.0f;
    int tuneSemitones = 0;
    uint8_t relNote = 48;
};

struct Instrument {
    std::string name = "";
    uint8_t volume = 64;
    uint8_t waveLength = 3;

    uint8_t attackFrames = 1;
    uint8_t attackVol = 64;
    uint8_t decayFrames = 1;
    uint8_t decayVol = 64;
    uint8_t sustainFrames = 1;
    uint8_t releaseFrames = 1;
    uint8_t releaseVol = 64;

    uint8_t vibratoDelay = 0;
    uint8_t vibratoDepth = 0;
    uint8_t vibratoSpeed = 0;

    uint8_t squareLower = 1;
    uint8_t squareUpper = 63;
    uint8_t squareSpeed = 1;

    uint8_t filterLower = 0;
    uint8_t filterUpper = 0;
    uint8_t filterSpeed = 0;

    uint8_t perfSpeed = 1;
    uint8_t perfLen = 3;
    uint8_t hardcut = 0;
    bool    releaseCut = false;

    PerfStep perfList[MAX_PERF_STEPS]{};

    bool hasSample = false;
    SampleData sample{};
};

struct SongData {
    std::string title = "";
    uint16_t length = 1;          // LEN: number of active sequences (1..64)
    uint16_t restart = 0;         // RES: restart sequence index (0..63)
    uint8_t  trackLength = 64;    // TRL: rows per pattern (1..64)
    uint8_t  activeSubSong = 0;
    SubSong  subSongs[MAX_SUBSONGS]{};

    AmigaFilterModel filterModel = FILTER_OFF;
    uint8_t speedMultiplier = 1;  // SS
    uint16_t bpm = 125;           // SSN
    bool ciaTiming = true;

    TrackEntry sequences[MAX_SEQUENCES][MAX_CHANNELS]{};
    TrackPattern patterns[MAX_PATTERNS]{};
    Instrument instruments[MAX_INSTRUMENTS]{};
};

struct PrefsConfig {
    std::wstring songsDir = L"&:";
    std::wstring instrDir = L"/    Instruments";
    bool explicitVblStartup = true;
    bool allocateAudioChannels = true;
    bool ptFontInRequesters = true;
    bool transparentVolumeMeter = false;
    int volumeMeterMode = 0;
    int cursorMode = 0;
    int rastertimeMode = 0;
};

// ============================================================================
// AUDIO & SEQUENCER ENGINE
// ============================================================================
struct VoiceState {
    bool active = false;
    bool muted = false;
    bool keyReleased = false;
    uint8_t instIndex = 0;
    uint8_t baseNote = 0;
    int8_t  transpose = 0;

    double phase = 0.0;
    double freqHz = 0.0;
    double samplePos = 0.0;

    float envVol = 0.0f;
    uint32_t envFrame = 0;
    uint32_t envPhase = 0;

    uint8_t perfStep = 0;
    uint8_t perfTick = 0;
    uint8_t perfSpeedOverride = 0;
    uint8_t waveform = 1;
    float squarePos = 0.5f;
    bool modActive = true;
    uint32_t vibTick = 0;

    float channelVol = 1.0f;
    float peakMeter = 0.0f;
    float lpState = 0.0f;
    uint32_t rngState = 0x12345678u;
};

class AHXEngine {
public:
    SongData song;
    VoiceState voices[MAX_CHANNELS]{};
    std::mutex engineMutex;

    std::atomic<bool> isPlaying{false};
    std::atomic<uint16_t> playSeq{0}; // Current sequence index (0..LEN-1)
    std::atomic<uint8_t> playRow{0};  // Current row index (0..TRL-1)
    std::atomic<uint8_t> currentSpeed{6};
    std::atomic<uint32_t> elapsedSeconds{0};
    std::atomic<float> cpuUsage{5.0f};

    uint32_t tickCounter = 0;
    double samplesPerTickAccum = 0.0;
    uint32_t sampleFrameCounter = 0;
    float masterVolume = 1.0f;
    float globalLpL1 = 0.0f, globalLpR1 = 0.0f;
    float globalLpL2 = 0.0f, globalLpR2 = 0.0f;

    AHXEngine() {
        resetEmptySong();
    }

    // Guarantees 100% isolated pattern indices per sequence and channel (0..255)
    // so writing a note on one channel NEVER duplicates or clones into sibling channels.
    void assignIndependentSequencePatterns(uint16_t seqIdx) {
        if (seqIdx >= MAX_SEQUENCES) return;
        for (int c = 0; c < MAX_CHANNELS; ++c) {
            int seqPat = int(seqIdx) % MAX_PATTERNS;
            song.sequences[seqIdx][c].patternIndex = static_cast<uint8_t>(seqPat);
            song.sequences[seqIdx][c].transpose = 0;
        }
    }

    // Keeps each channel isolated in its own track buffer while preserving sequential POS/LEN index (000, 001, 002...)
    void ensureUniqueChannelIsolation(uint16_t seqIdx) {
        if (seqIdx >= MAX_SEQUENCES) return;
        for (int c = 0; c < MAX_CHANNELS; ++c) {
            if (song.sequences[seqIdx][c].patternIndex >= MAX_PATTERNS) {
                song.sequences[seqIdx][c].patternIndex = uint8_t(seqIdx % MAX_PATTERNS);
            }
        }
    }

    void resetEmptySong() {
        std::lock_guard<std::mutex> lock(engineMutex);
        song = SongData{};
        song.title = "";
        song.length = 1;
        song.restart = 0;
        song.trackLength = 64;
        song.bpm = 125;
        song.speedMultiplier = 1;

        for (uint16_t s = 0; s < MAX_SEQUENCES; ++s) {
            assignIndependentSequencePatterns(s);
        }
        for (int i = 0; i < MAX_INSTRUMENTS; ++i) {
            song.instruments[i] = Instrument{};
        }
        for (int c = 0; c < MAX_CHANNELS; ++c) {
            voices[c] = VoiceState{};
            voices[c].rngState = 0x9E3779B9u + uint32_t(c * 1013);
        }
        playSeq = 0;
        playRow = 0;
        currentSpeed = 6;
        elapsedSeconds = 0;
    }

    void setSequenceLength(uint16_t newLen) {
        newLen = std::clamp<uint16_t>(newLen, 1, 64);
        if (newLen > song.length) {
            for (uint16_t s = song.length; s < newLen; ++s) {
                assignIndependentSequencePatterns(s);
            }
        }
        song.length = newLen;
        if (playSeq.load() >= song.length) {
            playSeq = song.length - 1;
        }
        if (song.restart >= song.length) {
            song.restart = 0;
        }
    }

    void stopAndResetToRowZero() {
        isPlaying = false;
        playRow = 0;
        tickCounter = 0;
        samplesPerTickAccum = 0.0;
        for (int c = 0; c < MAX_CHANNELS; ++c) {
            voices[c].active = false;
            voices[c].envVol = 0.0f;
            voices[c].peakMeter = 0.0f;
        }
        globalLpL1 = globalLpR1 = globalLpL2 = globalLpR2 = 0.0f;
    }

    // Decode .MP3 at full native quality via Windows Media Foundation (32-bit float precision)
    bool decodeMp3FileMF(const std::wstring& path, std::vector<float>& outMono, uint32_t& outRate, int& outDetectedBits) {
        IMFSourceReader* pReader = nullptr;
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &pReader)) || !pReader) return false;

        IMFMediaType* pPartialType = nullptr;
        if (FAILED(MFCreateMediaType(&pPartialType))) {
            pReader->Release();
            return false;
        }
        pPartialType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pPartialType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (FAILED(pReader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pPartialType))) {
            pPartialType->Release();
            pReader->Release();
            return false;
        }
        pPartialType->Release();

        IMFMediaType* pActualType = nullptr;
        uint32_t sampleRate = 44100;
        uint32_t numChannels = 1;
        if (SUCCEEDED(pReader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &pActualType)) && pActualType) {
            sampleRate = MFGetAttributeUINT32(pActualType, MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
            numChannels = std::max<uint32_t>(1, MFGetAttributeUINT32(pActualType, MF_MT_AUDIO_NUM_CHANNELS, 1));
            pActualType->Release();
        }

        outMono.clear();
        while (true) {
            DWORD flags = 0;
            IMFSample* pSample = nullptr;
            HRESULT hr = pReader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &pSample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                if (pSample) pSample->Release();
                break;
            }
            if (pSample) {
                IMFMediaBuffer* pBuffer = nullptr;
                if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuffer)) && pBuffer) {
                    BYTE* pAudioData = nullptr;
                    DWORD cbCurrentLength = 0;
                    if (SUCCEEDED(pBuffer->Lock(&pAudioData, nullptr, &cbCurrentLength)) && pAudioData) {
                        size_t floatCount = cbCurrentLength / sizeof(float);
                        size_t frameCount = floatCount / numChannels;
                        const float* fPtr = reinterpret_cast<const float*>(pAudioData);
                        for (size_t i = 0; i < frameCount; ++i) {
                            float sum = 0.0f;
                            for (uint32_t c = 0; c < numChannels; ++c) {
                                sum += fPtr[i * numChannels + c];
                            }
                            outMono.push_back(std::clamp(sum / float(numChannels), -1.0f, 1.0f));
                        }
                        pBuffer->Unlock();
                    }
                    pBuffer->Release();
                }
                pSample->Release();
            }
        }
        pReader->Release();

        if (outMono.empty()) return false;
        outRate = std::max<uint32_t>(8000, sampleRate);
        outDetectedBits = 32;
        return true;
    }

    // Exclusively accepts .WAV, .AIFF, and .MP3 at full maximum quality with exact bit-depth detection (4..32 bits)
    bool decodeAudioFile(const std::wstring& path, std::vector<float>& outMono, uint32_t& outRate, int& outDetectedBits) {
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".mp3") {
            return decodeMp3FileMF(path, outMono, outRate, outDetectedBits);
        }
        if (ext != ".wav" && ext != ".aiff") {
            return false;
        }

        std::ifstream f(fs::path(path), std::ios::binary);
        if (!f) return false;

        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (raw.size() < 36) return false;

        uint32_t srcRate = 44100;
        uint16_t numCh = 1;
        uint16_t srcBits = 16;
        uint16_t audioFmt = 1; // 1 = PCM, 3 = IEEE Float
        size_t dataOffset = 0;
        size_t dataBytes = 0;

        // 1. RIFF / WAVE (.wav) - Supports 4-bit, 8-bit, 16-bit, 24-bit, 32-bit PCM & 32-bit IEEE Float
        if (ext == ".wav" && std::memcmp(raw.data(), "RIFF", 4) == 0 && std::memcmp(raw.data() + 8, "WAVE", 4) == 0) {
            size_t pos = 12;
            while (pos + 8 <= raw.size()) {
                uint32_t chunkId = 0, chunkSize = 0;
                std::memcpy(&chunkId, raw.data() + pos, 4);
                std::memcpy(&chunkSize, raw.data() + pos + 4, 4);
                if (std::memcmp(&chunkId, "fmt ", 4) == 0 && pos + 24 <= raw.size()) {
                    std::memcpy(&audioFmt, raw.data() + pos + 8, 2);
                    std::memcpy(&numCh,    raw.data() + pos + 10, 2);
                    std::memcpy(&srcRate,  raw.data() + pos + 12, 4);
                    std::memcpy(&srcBits,  raw.data() + pos + 22, 2);
                    if (audioFmt == 0xFFFE && pos + 48 <= raw.size()) {
                        uint16_t subFmt = 1;
                        std::memcpy(&subFmt, raw.data() + pos + 32, 2);
                        audioFmt = subFmt;
                    }
                    numCh   = std::max<uint16_t>(1, numCh);
                    srcRate = std::max<uint32_t>(4000, srcRate);
                } else if (std::memcmp(&chunkId, "data", 4) == 0) {
                    dataOffset = pos + 8;
                    dataBytes = std::min<size_t>(chunkSize, raw.size() - dataOffset);
                    break;
                }
                pos += 8 + size_t(chunkSize) + (chunkSize & 1);
            }

            if (dataOffset == 0 || dataBytes == 0) return false;
            outMono.clear();
            const uint8_t* ptr = raw.data() + dataOffset;

            if (srcBits == 4) {
                outMono.reserve(dataBytes * 2);
                for (size_t i = 0; i < dataBytes; ++i) {
                    int8_t hi = int8_t((ptr[i] >> 4) & 0x0F) - 8;
                    int8_t lo = int8_t(ptr[i] & 0x0F) - 8;
                    outMono.push_back(float(hi) / 8.0f);
                    outMono.push_back(float(lo) / 8.0f);
                }
            } else if (audioFmt == 3 && srcBits == 32) {
                size_t frameSize = 4 * numCh, frames = dataBytes / frameSize;
                outMono.reserve(frames);
                for (size_t i = 0; i < frames; ++i) {
                    float acc = 0.0f;
                    for (uint16_t c = 0; c < numCh; ++c) {
                        float v = 0.0f;
                        std::memcpy(&v, ptr + i * frameSize + c * 4, 4);
                        acc += std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
                    }
                    outMono.push_back(acc / float(numCh));
                }
            } else if (srcBits == 32) {
                size_t frameSize = 4 * numCh, frames = dataBytes / frameSize;
                outMono.reserve(frames);
                for (size_t i = 0; i < frames; ++i) {
                    double acc = 0.0;
                    for (uint16_t c = 0; c < numCh; ++c) {
                        int32_t v = 0;
                        std::memcpy(&v, ptr + i * frameSize + c * 4, 4);
                        acc += double(v) / 2147483648.0;
                    }
                    outMono.push_back(float(acc / double(numCh)));
                }
            } else if (srcBits == 24) {
                size_t frameSize = 3 * numCh, frames = dataBytes / frameSize;
                outMono.reserve(frames);
                for (size_t i = 0; i < frames; ++i) {
                    double acc = 0.0;
                    for (uint16_t c = 0; c < numCh; ++c) {
                        const uint8_t* sp = ptr + i * frameSize + c * 3;
                        int32_t v = (int32_t(int8_t(sp[2])) << 16) | (int32_t(sp[1]) << 8) | int32_t(sp[0]);
                        acc += double(v) / 8388608.0;
                    }
                    outMono.push_back(float(acc / double(numCh)));
                }
            } else if (srcBits == 16) {
                size_t frameSize = 2 * numCh, frames = dataBytes / frameSize;
                outMono.reserve(frames);
                for (size_t i = 0; i < frames; ++i) {
                    double acc = 0.0;
                    for (uint16_t c = 0; c < numCh; ++c) {
                        int16_t v = 0;
                        std::memcpy(&v, ptr + i * frameSize + c * 2, 2);
                        acc += double(v) / 32768.0;
                    }
                    outMono.push_back(float(acc / double(numCh)));
                }
            } else {
                size_t frames = dataBytes / numCh;
                outMono.reserve(frames);
                for (size_t i = 0; i < frames; ++i) {
                    float acc = 0.0f;
                    for (uint16_t c = 0; c < numCh; ++c) {
                        uint8_t u = ptr[i * numCh + c];
                        acc += (int(u) - 128) / 128.0f;
                    }
                    outMono.push_back(acc / float(numCh));
                }
            }

            if (outMono.empty()) return false;
            for (float& s : outMono) s = std::clamp(s, -1.0f, 1.0f);
            outRate = srcRate;
            outDetectedBits = std::clamp<int>(srcBits, 4, 32);
            return true;
        }

        // 2. FORM / AIFF (.aiff) - Big-Endian 4-bit to 32-bit PCM at maximum quality
        if (ext == ".aiff" && std::memcmp(raw.data(), "FORM", 4) == 0 && std::memcmp(raw.data() + 8, "AIFF", 4) == 0) {
            size_t pos = 12;
            while (pos + 8 <= raw.size()) {
                uint32_t chunkSize = (uint32_t(raw[pos + 4]) << 24) | (uint32_t(raw[pos + 5]) << 16) |
                                     (uint32_t(raw[pos + 6]) << 8)  | uint32_t(raw[pos + 7]);
                if (std::memcmp(raw.data() + pos, "COMM", 4) == 0 && pos + 26 <= raw.size()) {
                    numCh = std::max<uint16_t>(1, (uint16_t(raw[pos + 8]) << 8) | uint16_t(raw[pos + 9]));
                    srcBits = (uint16_t(raw[pos + 14]) << 8) | uint16_t(raw[pos + 15]);
                    uint16_t expon = ((uint16_t(raw[pos + 16]) & 0x7F) << 8) | uint16_t(raw[pos + 17]);
                    uint64_t mant = 0;
                    for (int b = 0; b < 8; ++b) mant = (mant << 8) | raw[pos + 18 + b];
                    if (expon != 0 && mant != 0) {
                        double sr = std::ldexp(double(mant), int(expon) - 16383 - 63);
                        if (sr >= 4000.0 && sr <= 192000.0) srcRate = uint32_t(std::round(sr));
                    }
                } else if (std::memcmp(raw.data() + pos, "SSND", 4) == 0 && pos + 16 <= raw.size()) {
                    dataOffset = pos + 16;
                    dataBytes = std::min<size_t>(chunkSize > 8 ? chunkSize - 8 : 0, raw.size() - dataOffset);
                    break;
                }
                pos += 8 + size_t(chunkSize) + (chunkSize & 1);
            }

            if (dataOffset == 0 || dataBytes == 0) return false;
            outMono.clear();
            const uint8_t* ptr = raw.data() + dataOffset;
            int bytesPerSample = std::max(1, (srcBits + 7) / 8);
            size_t frameSize = size_t(bytesPerSample) * numCh;
            size_t frames = dataBytes / frameSize;
            outMono.reserve(frames);

            for (size_t i = 0; i < frames; ++i) {
                double acc = 0.0;
                for (uint16_t c = 0; c < numCh; ++c) {
                    const uint8_t* sp = ptr + i * frameSize + c * bytesPerSample;
                    if (srcBits <= 4) {
                        acc += double(int8_t(sp[0])) / 16.0;
                    } else if (srcBits <= 8) {
                        acc += double(int8_t(sp[0])) / 128.0;
                    } else if (srcBits == 16) {
                        int16_t v = static_cast<int16_t>((uint16_t(sp[0]) << 8) | uint16_t(sp[1]));
                        acc += double(v) / 32768.0;
                    } else if (srcBits == 24) {
                        int32_t v = (int32_t(int8_t(sp[0])) << 16) | (int32_t(sp[1]) << 8) | int32_t(sp[2]);
                        acc += double(v) / 8388608.0;
                    } else {
                        int32_t v = (int32_t(sp[0]) << 24) | (int32_t(sp[1]) << 16) | (int32_t(sp[2]) << 8) | int32_t(sp[3]);
                        acc += double(v) / 2147483648.0;
                    }
                }
                outMono.push_back(std::clamp(float(acc / double(numCh)), -1.0f, 1.0f));
            }
            if (outMono.empty()) return false;
            outRate = srcRate;
            outDetectedBits = std::clamp<int>(srcBits, 4, 32);
            return true;
        }
        return false;
    }

    // Loads .WAV, .AIFF, or .MP3 into the selected instrument slot while preserving its exact bit depth (4..32 bits)
    bool loadSampleFile(int instIdx, const std::wstring& path) {
        if (instIdx <= 0 || instIdx >= MAX_INSTRUMENTS) return false;
        std::vector<float> monoFloat;
        uint32_t srcRate = 44100;
        int detectedBits = 16;
        if (!decodeAudioFile(path, monoFloat, srcRate, detectedBits)) return false;

        std::lock_guard<std::mutex> lock(engineMutex);
        Instrument& inst = song.instruments[instIdx];
        inst.hasSample = true;
        inst.sample.bitDepth = std::clamp(detectedBits, 4, 32);
        inst.sample.sampleRate = srcRate;
        inst.sample.loopEnabled = false;
        inst.sample.loopStart = 0;
        inst.sample.loopEnd = monoFloat.size();
        inst.sample.ampPercent = 100.0f;
        inst.sample.pan = 0.0f;
        inst.sample.tuneSemitones = 0;
        inst.sample.relNote = 48;
        inst.sample.floatData = std::move(monoFloat);

        std::string fname = fs::path(path).filename().string();
        if (fname.size() > 18) fname.resize(18);
        inst.name = fname;
        return true;
    }

    // Complete FX1, FX2, and Pattern Command processor (0..F)
    // Every command operates cleanly across the [00 .. 64] parameter scale (00 = 0% OFF, 64 = 100% ON)
    void executeEffectCommand(VoiceState& v, Instrument& inst, uint8_t cmd, uint8_t param, bool isPerfList) {
        if (cmd == 0 && param == 0 && !isPerfList) return;

        // Decode parameter in range 00..64 (00 = 0.0 OFF, 64 = 1.0 100% ON)
        int val64 = std::clamp<int>(param, 0, 64);
        float norm64 = float(val64) / 64.0f;

        switch (cmd) {
            case 0x0: // 0: Filter Cutoff / Arpeggio Reset (00 = off, 01..64 = 100% on)
                v.lpState = norm64 * 0.5f;
                break;
            case 0x1: // 1: Pitch Slide Up (00 = off, 01..64 = upward frequency glide rate)
                if (val64 > 0) {
                    v.freqHz = std::min(16000.0, v.freqHz * std::pow(2.0, double(val64) / 384.0));
                }
                break;
            case 0x2: // 2: Pitch Slide Down (00 = off, 01..64 = downward frequency glide rate)
                if (val64 > 0) {
                    v.freqHz = std::max(20.0, v.freqHz / std::pow(2.0, double(val64) / 384.0));
                }
                break;
            case 0x3: // 3: Square / Pulse Width Duty Cycle (00..64 -> 0% to 100%)
                v.squarePos = std::clamp(norm64, 0.02f, 0.98f);
                break;
            case 0x4: // 4: Modulation Depth & Toggle (00 = OFF, 01..64 = ON at 0..100%)
                if (val64 == 0) {
                    v.modActive = false;
                } else {
                    v.modActive = true;
                    inst.vibratoDepth = uint8_t(val64);
                }
                break;
            case 0x5: // 5: Jump to Performance Step (00..63)
                if (inst.perfLen > 0) {
                    v.perfStep = uint8_t(std::min<int>(val64, inst.perfLen - 1));
                }
                break;
            case 0x6: // 6: Vibrato + Volume Attenuation (00 = 0% mute, 64 = 100% full)
                v.channelVol = norm64;
                break;
            case 0x7: // 7: Tremolo / Gain Scale (00 = off/unity, 01..64 = 100% depth)
                v.channelVol = (val64 == 0) ? 1.0f : std::clamp(1.0f - (norm64 * 0.5f), 0.0f, 1.0f);
                break;
            case 0x8: // 8: Stereo Panning (00 = Hard Left, 32 = Center, 64 = Hard Right)
                if (inst.hasSample) {
                    inst.sample.pan = std::clamp((norm64 * 2.0f) - 1.0f, -1.0f, 1.0f);
                }
                break;
            case 0x9: // 9: Sample Start Offset (00 = 0% beginning, 64 = 100% end)
                if (inst.hasSample && !inst.sample.floatData.empty()) {
                    v.samplePos = double(inst.sample.floatData.size() - 1) * double(norm64);
                }
                break;
            case 0xA: // A: Volume Level / Slide (00 = 0% OFF, 64 = 100% ON)
                v.envVol = norm64;
                v.channelVol = norm64;
                break;
            case 0xB: // B: Position / Sequence Jump (00..63)
                if (!isPerfList) {
                    playSeq = std::min<uint16_t>(val64, song.length > 0 ? song.length - 1 : 0);
                    playRow = 0;
                }
                break;
            case 0xC: // C: VOL (Volume Command: 00 = 0% OFF, 64 = 100% ON)
                v.envVol = norm64;
                v.channelVol = norm64;
                break;
            case 0xD: // D: Pattern Break to Row (00..63)
                if (!isPerfList) {
                    playSeq = (playSeq + 1) % std::max<uint16_t>(1, song.length);
                    playRow = std::min<uint8_t>(val64, song.trackLength - 1);
                }
                break;
            case 0xE: // E: Note Cut / Retrigger (00 = Note Cut OFF, 01..64 = Retrigger at volume 0..100%)
                if (val64 == 0) {
                    v.active = false;
                    v.envVol = 0.0f;
                    v.channelVol = 0.0f;
                } else {
                    v.samplePos = 0.0;
                    v.phase = 0.0;
                    v.envVol = norm64;
                    v.channelVol = norm64;
                }
                break;
            case 0xF: // F: Speed / Tempo (00 = stop tick, 01..31 = Speed ticks, 32..64 = Tempo scale)
                if (val64 > 0) {
                    if (isPerfList) v.perfSpeedOverride = uint8_t(val64);
                    else if (val64 < 32) currentSpeed = uint8_t(val64);
                    else song.bpm = uint16_t(std::clamp<int>(param, 35, 255));
                }
                break;
            default:
                break;
        }
    }

    void triggerNote(int ch, uint8_t note, uint8_t instIdx, int8_t transpose, uint8_t fxCmd, uint8_t fxParam) {
        if (ch < 0 || ch >= MAX_CHANNELS) return;
        VoiceState& v = voices[ch];
        if (instIdx > 0 && instIdx < MAX_INSTRUMENTS) v.instIndex = instIdx;
        Instrument& inst = song.instruments[v.instIndex];

        if (note > 0 && note <= 97) {
            v.active = true;
            v.keyReleased = false;
            v.baseNote = note;
            v.transpose = transpose;
            int finalNote = std::clamp(int(note) + int(transpose), 1, 97);
            v.freqHz = 440.0 * std::pow(2.0, (finalNote - 45) / 12.0);
            v.phase = 0.0;
            v.samplePos = 0.0;
            v.channelVol = 1.0f;

            v.envFrame = 0;
            v.envPhase = 0;
            v.envVol = (inst.attackFrames <= 1) ? (inst.attackVol / 64.0f) : 0.0f;

            v.perfStep = 0;
            v.perfTick = 0;
            v.perfSpeedOverride = 0;
            v.modActive = true;
            v.vibTick = 0;
            v.waveform = (inst.perfLen > 0 && inst.perfList[0].waveform > 0) ? inst.perfList[0].waveform : 1;
            v.squarePos = std::clamp(inst.squareLower / 64.0f, 0.02f, 0.98f);

            if (inst.perfLen > 0) {
                const PerfStep& ps0 = inst.perfList[0];
                if (ps0.note > 0) {
                    int pNote = ps0.fixedNote ? int(ps0.note) : (int(note) + int(ps0.note) - 49 + int(transpose));
                    pNote = std::clamp(pNote, 1, 97);
                    v.freqHz = 440.0 * std::pow(2.0, (pNote - 45) / 12.0);
                }
                if (ps0.fx1 != 0) executeEffectCommand(v, inst, (ps0.fx1 >> 8) & 0xF, ps0.fx1 & 0xFF, true);
                if (ps0.fx2 != 0) executeEffectCommand(v, inst, (ps0.fx2 >> 8) & 0xF, ps0.fx2 & 0xFF, true);
            }
        }

        if (fxCmd > 0 || fxParam > 0) {
            executeEffectCommand(v, inst, fxCmd, fxParam, false);
        }
    }

    void releaseNote(int ch) {
        if (ch < 0 || ch >= MAX_CHANNELS) return;
        VoiceState& v = voices[ch];
        if (!v.active) return;
        Instrument& inst = song.instruments[v.instIndex];
        v.keyReleased = true;
        if (inst.releaseCut) {
            v.active = false;
            v.envVol = 0.0f;
        } else {
            v.envPhase = 3;
            v.envFrame = 0;
        }
    }

    void processTick() {
        if (!isPlaying.load()) return;

        if (tickCounter == 0) {
            uint16_t seq = std::min<uint16_t>(playSeq.load(), MAX_SEQUENCES - 1);
            uint8_t row = std::min<uint8_t>(playRow.load(), MAX_TRACK_LEN - 1);
            for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
                const TrackEntry& te = song.sequences[seq][ch];
                uint8_t patIdx = uint8_t((int(te.patternIndex) * MAX_CHANNELS + ch) % MAX_PATTERNS);
                const PatternCell& cell = song.patterns[patIdx].rows[row];
                if (cell.note > 0 || cell.instrument > 0 || cell.fxCmd > 0 || cell.fxParam > 0) {
                    triggerNote(ch, cell.note, cell.instrument, te.transpose, cell.fxCmd, cell.fxParam);
                }
            }
        }

        for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
            VoiceState& v = voices[ch];
            if (!v.active) {
                v.peakMeter *= 0.85f;
                continue;
            }
            Instrument& inst = song.instruments[v.instIndex];

            if (v.envPhase == 0) {
                float target = inst.attackVol / 64.0f;
                v.envVol += (target - v.envVol) / std::max<uint8_t>(1, inst.attackFrames);
                if (++v.envFrame >= inst.attackFrames) { v.envPhase = 1; v.envFrame = 0; }
            } else if (v.envPhase == 1) {
                float target = inst.decayVol / 64.0f;
                v.envVol += (target - v.envVol) / std::max<uint8_t>(1, inst.decayFrames);
                if (++v.envFrame >= inst.decayFrames) { v.envPhase = 2; v.envFrame = 0; }
            } else if (v.envPhase == 2) {
                if (inst.sustainFrames > 0 && ++v.envFrame >= inst.sustainFrames) {
                    if (inst.releaseCut) {
                        v.active = false;
                        v.envVol = 0.0f;
                    } else {
                        v.envPhase = 3;
                        v.envFrame = 0;
                    }
                }
            } else {
                float target = inst.releaseVol / 64.0f;
                v.envVol += (target - v.envVol) / std::max<uint8_t>(1, inst.releaseFrames);
                if (++v.envFrame >= inst.releaseFrames && inst.releaseVol == 0) {
                    v.active = false;
                    v.envVol = 0.0f;
                }
            }

            if (v.modActive) {
                v.squarePos += (inst.squareSpeed * 0.005f);
                float lo = inst.squareLower / 64.0f;
                float hi = std::max(lo + 0.05f, inst.squareUpper / 64.0f);
                if (v.squarePos > hi) v.squarePos = lo;
            }

            if (inst.perfLen > 0) {
                uint8_t spd = (v.perfSpeedOverride > 0) ? v.perfSpeedOverride : std::max<uint8_t>(1, inst.perfSpeed);
                if (++v.perfTick >= spd) {
                    v.perfTick = 0;
                    v.perfStep = (v.perfStep + 1) % inst.perfLen;
                    const PerfStep& ps = inst.perfList[v.perfStep];
                    if (ps.waveform > 0) v.waveform = ps.waveform;
                    if (ps.note > 0) {
                        int fn = ps.fixedNote ? int(ps.note) : (int(v.baseNote) + int(ps.note) - 49 + int(v.transpose));
                        fn = std::clamp(fn, 1, 97);
                        v.freqHz = 440.0 * std::pow(2.0, (fn - 45) / 12.0);
                        if (inst.hasSample) v.samplePos = 0.0;
                    }
                    if (ps.fx1 != 0) executeEffectCommand(v, inst, (ps.fx1 >> 8) & 0xF, ps.fx1 & 0xFF, true);
                    if (ps.fx2 != 0) executeEffectCommand(v, inst, (ps.fx2 >> 8) & 0xF, ps.fx2 & 0xFF, true);
                }
            }
        }

        tickCounter++;
        if (tickCounter >= currentSpeed.load()) {
            tickCounter = 0;
            uint8_t nextRow = playRow.load() + 1;
            if (nextRow >= song.trackLength) {
                nextRow = 0;
                uint16_t nextSeq = playSeq.load() + 1;
                if (nextSeq >= song.length) nextSeq = song.restart;
                playSeq = nextSeq;
            }
            playRow = nextRow;
        }
    }

    // Preserves each instrument's exact native bit resolution (4-bit through 32-bit)
    // so 4/8-bit retro samples and 16/24/32-bit studio samples coexist without bit-conversion errors
    static inline float renderAtExactBitDepth(float sample, int bitDepth) {
        int clampedBits = std::clamp(bitDepth, 4, 32);
        if (clampedBits >= 24) return sample; // Full 24-bit / 32-bit floating-point precision
        double steps = std::ldexp(1.0, clampedBits - 1);
        return float(std::round(double(sample) * steps) / steps);
    }

    void renderStereoBlock(float* outInterleaved, int numFrames, int sampleRate) {
        std::lock_guard<std::mutex> lock(engineMutex);
        double baseHz = (double(song.bpm) * 2.0) / 5.0;
        double tickRateHz = baseHz * std::max<uint8_t>(1, song.speedMultiplier);
        double samplesPerTick = double(sampleRate) / tickRateHz;

        for (int i = 0; i < numFrames; ++i) {
            samplesPerTickAccum += 1.0;
            if (samplesPerTickAccum >= samplesPerTick) {
                samplesPerTickAccum -= samplesPerTick;
                processTick();
            }

            if (isPlaying.load()) {
                if (++sampleFrameCounter >= uint32_t(sampleRate)) {
                    sampleFrameCounter = 0;
                    elapsedSeconds++;
                }
            }

            float mixL = 0.0f, mixR = 0.0f;
            for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
                VoiceState& v = voices[ch];
                if (!v.active || v.muted) continue;

                Instrument& inst = song.instruments[v.instIndex];
                float rawSample = 0.0f;
                float voicePan = 0.0f;

                if (inst.hasSample && !inst.sample.floatData.empty()) {
                    double relOffset = double(inst.sample.tuneSemitones) + double(int(inst.sample.relNote) - 48);
                    double effectiveFreq = v.freqHz * std::pow(2.0, relOffset / 12.0);
                    double pitchRatio = (effectiveFreq / 261.625565) * (double(inst.sample.sampleRate) / double(sampleRate));
                    size_t idx0 = static_cast<size_t>(v.samplePos);
                    size_t maxLen = inst.sample.floatData.size();
                    if (idx0 < maxLen) {
                        if (inst.sample.bitDepth <= 8) {
                            rawSample = inst.sample.floatData[idx0];
                        } else {
                            size_t idx1 = std::min(idx0 + 1, maxLen - 1);
                            float frac = float(v.samplePos - double(idx0));
                            rawSample = inst.sample.floatData[idx0] * (1.0f - frac) + inst.sample.floatData[idx1] * frac;
                        }
                        rawSample = renderAtExactBitDepth(rawSample, inst.sample.bitDepth);
                        rawSample *= (inst.sample.ampPercent / 100.0f);
                        voicePan = std::clamp(inst.sample.pan, -1.0f, 1.0f);
                        v.samplePos += pitchRatio;
                        if (inst.sample.loopEnabled && inst.sample.loopEnd > inst.sample.loopStart) {
                            if (v.samplePos >= double(inst.sample.loopEnd)) {
                                v.samplePos = double(inst.sample.loopStart) + std::fmod(v.samplePos - double(inst.sample.loopStart), double(inst.sample.loopEnd - inst.sample.loopStart));
                            }
                        }
                    } else {
                        v.active = false;
                    }
                } else {
                    v.phase += v.freqHz / double(sampleRate);
                    if (v.phase >= 1.0) v.phase -= 1.0;
                    float p = float(v.phase);

                    switch (v.waveform) {
                        case 1: rawSample = (p < 0.5f) ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p); break;
                        case 2: rawSample = 2.0f * p - 1.0f; break;
                        case 3: rawSample = (p < v.squarePos) ? 0.85f : -0.85f; break;
                        case 4:
                            v.rngState ^= v.rngState << 13;
                            v.rngState ^= v.rngState >> 17;
                            v.rngState ^= v.rngState << 5;
                            rawSample = ((v.rngState & 0xFFFF) / 32768.0f) - 1.0f;
                            break;
                        default: rawSample = 0.0f; break;
                    }
                }

                float outVoice = rawSample * v.envVol * v.channelVol * (inst.volume / 64.0f);
                v.peakMeter = std::max(v.peakMeter * 0.9992f, std::fabs(outVoice));

                float panL = std::sqrt(0.5f * (1.0f - voicePan));
                float panR = std::sqrt(0.5f * (1.0f + voicePan));
                mixL += outVoice * panL;
                mixR += outVoice * panR;
            }

            mixL *= masterVolume * 0.40f;
            mixR *= masterVolume * 0.40f;

            if (song.filterModel == FILTER_A500) {
                float alpha = float(2.0 * 3.14159265 * 4500.0 / (double(sampleRate) + 2.0 * 3.14159265 * 4500.0));
                globalLpL1 += alpha * (mixL - globalLpL1);
                globalLpR1 += alpha * (mixR - globalLpR1);
                mixL = globalLpL1; mixR = globalLpR1;
            } else if (song.filterModel == FILTER_A1200) {
                float alpha = float(2.0 * 3.14159265 * 14000.0 / (double(sampleRate) + 2.0 * 3.14159265 * 14000.0));
                globalLpL1 += alpha * (mixL - globalLpL1);
                globalLpR1 += alpha * (mixR - globalLpR1);
                mixL = globalLpL1; mixR = globalLpR1;
            } else if (song.filterModel == FILTER_LED_LPF) {
                float alpha = float(2.0 * 3.14159265 * 3275.0 / (double(sampleRate) + 2.0 * 3.14159265 * 3275.0));
                globalLpL1 += alpha * (mixL - globalLpL1);
                globalLpL2 += alpha * (globalLpL1 - globalLpL2);
                globalLpR1 += alpha * (mixR - globalLpR1);
                globalLpR2 += alpha * (globalLpR1 - globalLpR2);
                mixL = globalLpL2; mixR = globalLpR2;
            }

            outInterleaved[i * 2]     = std::clamp(mixL, -1.0f, 1.0f);
            outInterleaved[i * 2 + 1] = std::clamp(mixR, -1.0f, 1.0f);
        }
    }

    int optimizeSong() {
        std::lock_guard<std::mutex> lock(engineMutex);
        bool used[MAX_PATTERNS]{};
        for (uint16_t s = 0; s < song.length; ++s)
            for (int c = 0; c < MAX_CHANNELS; ++c)
                used[song.sequences[s][c].patternIndex] = true;
        int cleared = 0;
        for (int i = 0; i < MAX_PATTERNS; ++i) {
            if (!used[i]) { song.patterns[i] = TrackPattern{}; cleared++; }
        }
        return cleared;
    }

    // Complete binary serialization (.IF / native project format): saves POS, LEN, RES, TRL, SS, SSN, SSP, Patterns, and Samples
    bool saveProjectModFormat(const std::wstring& path) {
        std::lock_guard<std::mutex> lock(engineMutex);
        std::ofstream f(fs::path(path), std::ios::binary);
        if (!f) return false;

        f.write("AHXMOD27", 8);
        uint32_t tlen = uint32_t(song.title.size());
        f.write(reinterpret_cast<const char*>(&tlen), 4);
        f.write(song.title.data(), tlen);

        uint16_t curPosVal = playSeq.load();
        uint8_t curSpdVal = currentSpeed.load();
        f.write(reinterpret_cast<const char*>(&curPosVal), sizeof(curPosVal));
        f.write(reinterpret_cast<const char*>(&song.length), sizeof(song.length));
        f.write(reinterpret_cast<const char*>(&song.restart), sizeof(song.restart));
        f.write(reinterpret_cast<const char*>(&song.trackLength), sizeof(song.trackLength));
        f.write(reinterpret_cast<const char*>(&song.activeSubSong), sizeof(song.activeSubSong));
        f.write(reinterpret_cast<const char*>(&song.speedMultiplier), sizeof(song.speedMultiplier));
        f.write(reinterpret_cast<const char*>(&song.bpm), sizeof(song.bpm));
        f.write(reinterpret_cast<const char*>(&curSpdVal), sizeof(curSpdVal));
        f.write(reinterpret_cast<const char*>(&song.filterModel), sizeof(song.filterModel));
        f.write(reinterpret_cast<const char*>(song.subSongs), sizeof(song.subSongs));
        f.write(reinterpret_cast<const char*>(song.sequences), sizeof(song.sequences));
        f.write(reinterpret_cast<const char*>(song.patterns), sizeof(song.patterns));

        for (int i = 0; i < MAX_INSTRUMENTS; ++i) {
            const Instrument& inst = song.instruments[i];
            uint32_t nlen = uint32_t(inst.name.size());
            f.write(reinterpret_cast<const char*>(&nlen), 4);
            f.write(inst.name.data(), nlen);
            f.write(reinterpret_cast<const char*>(&inst.volume), sizeof(inst.volume));
            f.write(reinterpret_cast<const char*>(&inst.waveLength), sizeof(inst.waveLength));
            f.write(reinterpret_cast<const char*>(&inst.attackFrames), sizeof(inst.attackFrames));
            f.write(reinterpret_cast<const char*>(&inst.attackVol), sizeof(inst.attackVol));
            f.write(reinterpret_cast<const char*>(&inst.decayFrames), sizeof(inst.decayFrames));
            f.write(reinterpret_cast<const char*>(&inst.decayVol), sizeof(inst.decayVol));
            f.write(reinterpret_cast<const char*>(&inst.sustainFrames), sizeof(inst.sustainFrames));
            f.write(reinterpret_cast<const char*>(&inst.releaseFrames), sizeof(inst.releaseFrames));
            f.write(reinterpret_cast<const char*>(&inst.releaseVol), sizeof(inst.releaseVol));
            f.write(reinterpret_cast<const char*>(&inst.vibratoDelay), sizeof(inst.vibratoDelay));
            f.write(reinterpret_cast<const char*>(&inst.vibratoDepth), sizeof(inst.vibratoDepth));
            f.write(reinterpret_cast<const char*>(&inst.vibratoSpeed), sizeof(inst.vibratoSpeed));
            f.write(reinterpret_cast<const char*>(&inst.squareLower), sizeof(inst.squareLower));
            f.write(reinterpret_cast<const char*>(&inst.squareUpper), sizeof(inst.squareUpper));
            f.write(reinterpret_cast<const char*>(&inst.squareSpeed), sizeof(inst.squareSpeed));
            f.write(reinterpret_cast<const char*>(&inst.filterLower), sizeof(inst.filterLower));
            f.write(reinterpret_cast<const char*>(&inst.filterUpper), sizeof(inst.filterUpper));
            f.write(reinterpret_cast<const char*>(&inst.filterSpeed), sizeof(inst.filterSpeed));
            f.write(reinterpret_cast<const char*>(&inst.perfSpeed), sizeof(inst.perfSpeed));
            f.write(reinterpret_cast<const char*>(&inst.perfLen), sizeof(inst.perfLen));
            f.write(reinterpret_cast<const char*>(&inst.hardcut), sizeof(inst.hardcut));
            f.write(reinterpret_cast<const char*>(&inst.releaseCut), sizeof(inst.releaseCut));
            f.write(reinterpret_cast<const char*>(inst.perfList), sizeof(inst.perfList));

            uint8_t hasSmp = inst.hasSample ? 1 : 0;
            f.write(reinterpret_cast<const char*>(&hasSmp), 1);
            if (inst.hasSample) {
                f.write(reinterpret_cast<const char*>(&inst.sample.bitDepth), sizeof(inst.sample.bitDepth));
                f.write(reinterpret_cast<const char*>(&inst.sample.sampleRate), sizeof(inst.sample.sampleRate));
                f.write(reinterpret_cast<const char*>(&inst.sample.loopEnabled), sizeof(inst.sample.loopEnabled));
                f.write(reinterpret_cast<const char*>(&inst.sample.loopStart), sizeof(inst.sample.loopStart));
                f.write(reinterpret_cast<const char*>(&inst.sample.loopEnd), sizeof(inst.sample.loopEnd));
                f.write(reinterpret_cast<const char*>(&inst.sample.ampPercent), sizeof(inst.sample.ampPercent));
                f.write(reinterpret_cast<const char*>(&inst.sample.pan), sizeof(inst.sample.pan));
                f.write(reinterpret_cast<const char*>(&inst.sample.tuneSemitones), sizeof(inst.sample.tuneSemitones));
                f.write(reinterpret_cast<const char*>(&inst.sample.relNote), sizeof(inst.sample.relNote));
                uint32_t sCount = uint32_t(inst.sample.floatData.size());
                f.write(reinterpret_cast<const char*>(&sCount), 4);
                if (sCount > 0) {
                    f.write(reinterpret_cast<const char*>(inst.sample.floatData.data()), sCount * sizeof(float));
                }
            }
        }
        return true;
    }

    // Exclusively loads .MOD, .XM, and .IF modules: restores Samples, POS, LEN, RES, TRL, SS, SSN, SSP
    // while keeping every channel strictly independent (never cloning notes across sibling channels).
    bool loadProjectModFormat(const std::wstring& path) {
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".mod" && ext != ".xm" && ext != ".if") {
            return false;
        }

        std::ifstream f(fs::path(path), std::ios::binary);
        if (!f) return false;

        f.seekg(0, std::ios::end);
        std::streamoff endPos = f.tellg();
        if (endPos <= 16) return false;
        size_t fileSize = static_cast<size_t>(endPos);
        f.seekg(0, std::ios::beg);

        std::vector<uint8_t> modBytes(fileSize);
        if (!f.read(reinterpret_cast<char*>(modBytes.data()), fileSize)) return false;

        std::lock_guard<std::mutex> lock(engineMutex);
        stopAndResetToRowZero();

        // 1. Native AHX / .IF Module Format (AHXMOD27)
        if (std::memcmp(modBytes.data(), "AHXMOD27", 8) == 0) {
            size_t pos = 8;
            auto readBytes = [&](void* dst, size_t sz) -> bool {
                if (pos + sz > fileSize) return false;
                std::memcpy(dst, modBytes.data() + pos, sz);
                pos += sz;
                return true;
            };

            uint32_t tlen = 0;
            if (!readBytes(&tlen, 4)) return false;
            if (tlen > 1024 || pos + tlen > fileSize) return false;
            song.title.assign(reinterpret_cast<const char*>(modBytes.data() + pos), tlen);
            pos += tlen;

            uint16_t savedPosVal = 0;
            uint8_t savedSpdVal = 6;
            if (!readBytes(&savedPosVal, sizeof(savedPosVal))) return false;
            if (!readBytes(&song.length, sizeof(song.length))) return false;
            if (!readBytes(&song.restart, sizeof(song.restart))) return false;
            if (!readBytes(&song.trackLength, sizeof(song.trackLength))) return false;
            if (!readBytes(&song.activeSubSong, sizeof(song.activeSubSong))) return false;
            if (!readBytes(&song.speedMultiplier, sizeof(song.speedMultiplier))) return false;
            if (!readBytes(&song.bpm, sizeof(song.bpm))) return false;
            if (!readBytes(&savedSpdVal, sizeof(savedSpdVal))) return false;
            if (!readBytes(&song.filterModel, sizeof(song.filterModel))) return false;
            if (!readBytes(song.subSongs, sizeof(song.subSongs))) return false;
            if (!readBytes(song.sequences, sizeof(song.sequences))) return false;
            if (!readBytes(song.patterns, sizeof(song.patterns))) return false;

            song.length = std::clamp<uint16_t>(song.length, 1, MAX_SEQUENCES);
            song.restart = std::min<uint16_t>(song.restart, song.length - 1);
            song.trackLength = std::clamp<uint8_t>(song.trackLength, 1, MAX_TRACK_LEN);
            song.activeSubSong = std::min<uint8_t>(song.activeSubSong, MAX_SUBSONGS - 1);
            song.speedMultiplier = std::clamp<uint8_t>(song.speedMultiplier, 1, 16);
            song.bpm = std::clamp<uint16_t>(song.bpm, 35, 255);
            currentSpeed = std::max<uint8_t>(1, savedSpdVal);

            for (uint16_t s = 0; s < song.length; ++s) {
                ensureUniqueChannelIsolation(s);
            }

            for (int i = 0; i < MAX_INSTRUMENTS; ++i) {
                Instrument& inst = song.instruments[i];
                inst = Instrument{};
                uint32_t nlen = 0;
                if (!readBytes(&nlen, 4)) break;
                if (nlen > 1024 || pos + nlen > fileSize) break;
                inst.name.assign(reinterpret_cast<const char*>(modBytes.data() + pos), nlen);
                pos += nlen;

                if (!readBytes(&inst.volume, sizeof(inst.volume))) break;
                if (!readBytes(&inst.waveLength, sizeof(inst.waveLength))) break;
                if (!readBytes(&inst.attackFrames, sizeof(inst.attackFrames))) break;
                if (!readBytes(&inst.attackVol, sizeof(inst.attackVol))) break;
                if (!readBytes(&inst.decayFrames, sizeof(inst.decayFrames))) break;
                if (!readBytes(&inst.decayVol, sizeof(inst.decayVol))) break;
                if (!readBytes(&inst.sustainFrames, sizeof(inst.sustainFrames))) break;
                if (!readBytes(&inst.releaseFrames, sizeof(inst.releaseFrames))) break;
                if (!readBytes(&inst.releaseVol, sizeof(inst.releaseVol))) break;
                if (!readBytes(&inst.vibratoDelay, sizeof(inst.vibratoDelay))) break;
                if (!readBytes(&inst.vibratoDepth, sizeof(inst.vibratoDepth))) break;
                if (!readBytes(&inst.vibratoSpeed, sizeof(inst.vibratoSpeed))) break;
                if (!readBytes(&inst.squareLower, sizeof(inst.squareLower))) break;
                if (!readBytes(&inst.squareUpper, sizeof(inst.squareUpper))) break;
                if (!readBytes(&inst.squareSpeed, sizeof(inst.squareSpeed))) break;
                if (!readBytes(&inst.filterLower, sizeof(inst.filterLower))) break;
                if (!readBytes(&inst.filterUpper, sizeof(inst.filterUpper))) break;
                if (!readBytes(&inst.filterSpeed, sizeof(inst.filterSpeed))) break;
                if (!readBytes(&inst.perfSpeed, sizeof(inst.perfSpeed))) break;
                if (!readBytes(&inst.perfLen, sizeof(inst.perfLen))) break;
                if (!readBytes(&inst.hardcut, sizeof(inst.hardcut))) break;
                if (!readBytes(&inst.releaseCut, sizeof(inst.releaseCut))) break;
                if (!readBytes(inst.perfList, sizeof(inst.perfList))) break;

                inst.perfLen = std::min<uint8_t>(inst.perfLen, MAX_PERF_STEPS);
                inst.perfSpeed = std::max<uint8_t>(1, inst.perfSpeed);

                uint8_t hasSmp = 0;
                if (!readBytes(&hasSmp, 1)) break;
                inst.hasSample = (hasSmp != 0);
                if (inst.hasSample) {
                    if (!readBytes(&inst.sample.bitDepth, sizeof(inst.sample.bitDepth))) { inst.hasSample = false; break; }
                    if (!readBytes(&inst.sample.sampleRate, sizeof(inst.sample.sampleRate))) { inst.hasSample = false; break; }
                    if (!readBytes(&inst.sample.loopEnabled, sizeof(inst.sample.loopEnabled))) { inst.hasSample = false; break; }
                    if (!readBytes(&inst.sample.loopStart, sizeof(inst.sample.loopStart))) { inst.hasSample = false; break; }
                    if (!readBytes(&inst.sample.loopEnd, sizeof(inst.sample.loopEnd))) { inst.hasSample = false; break; }
                    if (!readBytes(&inst.sample.ampPercent, sizeof(inst.sample.ampPercent))) { inst.hasSample = false; break; }
                    if (!readBytes(&inst.sample.pan, sizeof(inst.sample.pan))) { inst.hasSample = false; break; }
                    if (!readBytes(&inst.sample.tuneSemitones, sizeof(inst.sample.tuneSemitones))) { inst.hasSample = false; break; }
                    if (!readBytes(&inst.sample.relNote, sizeof(inst.sample.relNote))) { inst.hasSample = false; break; }
                    uint32_t sCount = 0;
                    if (!readBytes(&sCount, 4)) { inst.hasSample = false; break; }
                    size_t maxFloats = (fileSize - pos) / sizeof(float);
                    if (sCount > maxFloats) {
                        inst.hasSample = false;
                        inst.sample = SampleData{};
                        break;
                    }
                    inst.sample.floatData.resize(sCount);
                    if (sCount > 0) {
                        readBytes(inst.sample.floatData.data(), size_t(sCount) * sizeof(float));
                    }
                    inst.sample.bitDepth = std::clamp(inst.sample.bitDepth, 4, 32);
                    inst.sample.sampleRate = std::clamp<uint32_t>(inst.sample.sampleRate, 4000, 192000);
                    inst.sample.loopEnd = std::min(inst.sample.loopEnd, inst.sample.floatData.size());
                    if (inst.sample.loopStart >= inst.sample.loopEnd) inst.sample.loopEnabled = false;
                } else {
                    inst.sample = SampleData{};
                }
            }
            playSeq = std::min<uint16_t>(savedPosVal, song.length - 1);
            playRow = 0;
            return true;
        }

        // 2. FastTracker II Extended Module (.XM)
        if (fileSize >= 80 && std::memcmp(modBytes.data(), "Extended Module: ", 17) == 0) {
            song = SongData{};
            char titleBuf[21]{};
            std::memcpy(titleBuf, modBytes.data() + 17, 20);
            song.title = titleBuf;

            uint32_t headerSize = 0;
            std::memcpy(&headerSize, modBytes.data() + 60, 4);
            uint16_t songLen = 0, restartPos = 0, numChans = 0, numPats = 0, numInsts = 0, defTempo = 6, defBpm = 125;
            std::memcpy(&songLen,    modBytes.data() + 64, 2);
            std::memcpy(&restartPos, modBytes.data() + 66, 2);
            std::memcpy(&numChans,   modBytes.data() + 68, 2);
            std::memcpy(&numPats,    modBytes.data() + 70, 2);
            std::memcpy(&numInsts,   modBytes.data() + 72, 2);
            std::memcpy(&defTempo,   modBytes.data() + 76, 2);
            std::memcpy(&defBpm,     modBytes.data() + 78, 2);

            song.length = std::clamp<uint16_t>(songLen, 1, MAX_SEQUENCES);
            song.restart = (restartPos < song.length) ? restartPos : 0;
            song.trackLength = 64;
            song.activeSubSong = 0;
            song.subSongs[0].startPos = 0;
            song.bpm = std::clamp<uint16_t>(defBpm, 35, 255);
            song.speedMultiplier = 1;
            currentSpeed = std::clamp<uint8_t>(uint8_t(defTempo), 1, 31);

            for (uint16_t s = 0; s < MAX_SEQUENCES; ++s) {
                assignIndependentSequencePatterns(s);
            }

            uint8_t orderTable[256]{};
            size_t orderLen = std::min<size_t>(256, fileSize > 80 ? fileSize - 80 : 0);
            std::memcpy(orderTable, modBytes.data() + 80, orderLen);

            if (headerSize < 20) headerSize = 276;
            size_t offset = 60 + size_t(headerSize);
            if (offset > fileSize) return false;
            int loadChans = std::min<int>(numChans, MAX_CHANNELS);

            for (uint16_t p = 0; p < numPats && offset + 9 <= fileSize; ++p) {
                uint32_t patHdrLen = 0;
                uint16_t numRows = 64, packedSize = 0;
                std::memcpy(&patHdrLen,  modBytes.data() + offset, 4);
                std::memcpy(&numRows,    modBytes.data() + offset + 5, 2);
                std::memcpy(&packedSize, modBytes.data() + offset + 7, 2);
                if (patHdrLen < 9) patHdrLen = 9;
                if (offset + patHdrLen > fileSize) break;
                if (p == 0 && numRows > 0) song.trackLength = std::clamp<uint8_t>(uint8_t(std::min<uint16_t>(numRows, MAX_TRACK_LEN)), 1, MAX_TRACK_LEN);

                size_t patDataOff = offset + patHdrLen;
                size_t patEnd = (packedSize <= fileSize - patDataOff) ? (patDataOff + packedSize) : fileSize;
                size_t cur = patDataOff;

                if (packedSize > 0) {
                    for (int r = 0; r < int(numRows) && cur < patEnd; ++r) {
                        for (int c = 0; c < int(numChans) && cur < patEnd; ++c) {
                            uint8_t b = modBytes[cur++];
                            uint8_t note = 0, inst = 0, vol = 0, fx = 0, param = 0;
                            if (b & 0x80) {
                                if ((b & 0x01) && cur < patEnd) note  = modBytes[cur++];
                                if ((b & 0x02) && cur < patEnd) inst  = modBytes[cur++];
                                if ((b & 0x04) && cur < patEnd) vol   = modBytes[cur++];
                                if ((b & 0x08) && cur < patEnd) fx    = modBytes[cur++];
                                if ((b & 0x10) && cur < patEnd) param = modBytes[cur++];
                            } else {
                                note = b;
                                if (cur < patEnd) inst  = modBytes[cur++];
                                if (cur < patEnd) vol   = modBytes[cur++];
                                if (cur < patEnd) fx    = modBytes[cur++];
                                if (cur < patEnd) param = modBytes[cur++];
                            }

                            if (r < MAX_TRACK_LEN && c < loadChans) {
                                for (uint16_t s = 0; s < song.length; ++s) {
                                    if (orderTable[s] == p) {
                                        uint8_t chPat = song.sequences[s][c].patternIndex;
                                        PatternCell& cell = song.patterns[chPat].rows[r];
                                        cell.note = (note >= 1 && note <= 96) ? note : 0;
                                        cell.instrument = std::min<uint8_t>(63, inst);
                                        if (fx > 0 || param > 0) {
                                            cell.fxCmd = fx & 0x0F;
                                            cell.fxParam = std::min<uint8_t>(64, param);
                                        } else if (vol >= 0x10 && vol <= 0x50) {
                                            cell.fxCmd = 0x0C;
                                            cell.fxParam = vol - 0x10;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                offset = patEnd;
            }

            for (uint16_t ins = 1; ins <= numInsts && offset + 29 <= fileSize; ++ins) {
                uint32_t insHdrSize = 0;
                uint16_t numSmps = 0;
                std::memcpy(&insHdrSize, modBytes.data() + offset, 4);
                std::memcpy(&numSmps,    modBytes.data() + offset + 27, 2);
                if (insHdrSize < 29) insHdrSize = 29;
                if (offset + insHdrSize > fileSize) break;

                if (ins < MAX_INSTRUMENTS) {
                    char iName[23]{};
                    std::memcpy(iName, modBytes.data() + offset + 4, 22);
                    song.instruments[ins].name = iName;
                }

                uint32_t smpHdrSize = 40;
                if (numSmps > 0 && insHdrSize >= 33 && offset + 33 <= fileSize) {
                    std::memcpy(&smpHdrSize, modBytes.data() + offset + 29, 4);
                    if (smpHdrSize < 18) smpHdrSize = 40;
                }
                offset += insHdrSize;

                if (numSmps == 0) continue;
                if (numSmps > 64) numSmps = 64;

                struct XmSmpMeta {
                    uint32_t lenBytes = 0, loopStart = 0, loopLen = 0;
                    uint8_t vol = 64, type = 0, relNote = 48;
                    int8_t fineTune = 0;
                };
                std::vector<XmSmpMeta> metas(numSmps);
                uint16_t readSmps = 0;
                for (uint16_t k = 0; k < numSmps && smpHdrSize <= fileSize - offset; ++k) {
                    std::memcpy(&metas[k].lenBytes,  modBytes.data() + offset, 4);
                    std::memcpy(&metas[k].loopStart, modBytes.data() + offset + 4, 4);
                    std::memcpy(&metas[k].loopLen,   modBytes.data() + offset + 8, 4);
                    metas[k].vol      = modBytes[offset + 12];
                    metas[k].fineTune = int8_t(modBytes[offset + 13]);
                    metas[k].type     = modBytes[offset + 14];
                    metas[k].relNote  = uint8_t(std::clamp<int>(int(int8_t(modBytes[offset + 16])) + 48, 0, 84));
                    offset += smpHdrSize;
                    readSmps++;
                }

                for (uint16_t k = 0; k < readSmps && offset < fileSize; ++k) {
                    size_t availBytes = fileSize - offset;
                    size_t actualBytes = std::min<size_t>(metas[k].lenBytes, availBytes);
                    if (k == 0 && ins < MAX_INSTRUMENTS && actualBytes > 0) {
                        Instrument& inst = song.instruments[ins];
                        bool is16Bit = (metas[k].type & 0x10) != 0;
                        inst.volume = std::min<uint8_t>(64, metas[k].vol);
                        inst.hasSample = true;
                        inst.sample.bitDepth = is16Bit ? 16 : 8;
                        inst.sample.sampleRate = 16574;
                        inst.sample.relNote = std::clamp<uint8_t>(metas[k].relNote, 0, 84);
                        inst.sample.tuneSemitones = std::clamp<int>(metas[k].fineTune / 16, -24, 24);

                        if (is16Bit) {
                            size_t count = actualBytes / 2;
                            inst.sample.floatData.resize(count);
                            int16_t old = 0;
                            for (size_t idx = 0; idx < count; ++idx) {
                                int16_t delta = 0;
                                std::memcpy(&delta, modBytes.data() + offset + idx * 2, 2);
                                old = static_cast<int16_t>(static_cast<uint16_t>(old) + static_cast<uint16_t>(delta));
                                inst.sample.floatData[idx] = float(old) / 32768.0f;
                            }
                            if ((metas[k].type & 0x03) != 0 && metas[k].loopLen >= 4 && count > 0) {
                                inst.sample.loopEnabled = true;
                                inst.sample.loopStart = std::min<size_t>(count - 1, metas[k].loopStart / 2);
                                inst.sample.loopEnd = std::min<size_t>(count, (size_t(metas[k].loopStart) + size_t(metas[k].loopLen)) / 2);
                                if (inst.sample.loopStart >= inst.sample.loopEnd) inst.sample.loopEnabled = false;
                            }
                        } else {
                            size_t count = actualBytes;
                            inst.sample.floatData.resize(count);
                            int8_t old = 0;
                            for (size_t idx = 0; idx < count; ++idx) {
                                old = static_cast<int8_t>(static_cast<uint8_t>(old) + modBytes[offset + idx]);
                                inst.sample.floatData[idx] = float(old) / 128.0f;
                            }
                            if ((metas[k].type & 0x03) != 0 && metas[k].loopLen >= 2 && count > 0) {
                                inst.sample.loopEnabled = true;
                                inst.sample.loopStart = std::min<size_t>(count - 1, metas[k].loopStart);
                                inst.sample.loopEnd = std::min<size_t>(count, size_t(metas[k].loopStart) + size_t(metas[k].loopLen));
                                if (inst.sample.loopStart >= inst.sample.loopEnd) inst.sample.loopEnabled = false;
                            }
                        }
                    }
                    offset += actualBytes;
                }
            }
            playSeq = 0;
            playRow = 0;
            return true;
        }

        // 3. Standard ProTracker / FastTracker / NoiseTracker .MOD Module
        if (fileSize >= 600) {
            int modChannels = 0;
            int numModSamples = 31;
            size_t songLenOffset = 950;
            size_t orderTableOffset = 952;
            size_t patDataStart = 1084;

            if (fileSize >= 1084) {
                const uint8_t* tag = modBytes.data() + 1080;
                if (std::memcmp(tag, "M.K.", 4) == 0 || std::memcmp(tag, "M!K!", 4) == 0 ||
                    std::memcmp(tag, "FLT4", 4) == 0 || std::memcmp(tag, "4CHN", 4) == 0 ||
                    std::memcmp(tag, "N.T.", 4) == 0) {
                    modChannels = 4;
                } else if (std::memcmp(tag, "6CHN", 4) == 0) {
                    modChannels = 6;
                } else if (std::memcmp(tag, "8CHN", 4) == 0 || std::memcmp(tag, "OCTA", 4) == 0 || std::memcmp(tag, "CD81", 4) == 0) {
                    modChannels = 8;
                } else if (tag[2] == 'C' && tag[3] == 'H' && tag[0] >= '1' && tag[0] <= '3' && tag[1] >= '0' && tag[1] <= '9') {
                    modChannels = (tag[0] - '0') * 10 + (tag[1] - '0');
                }
            }

            // Fallback for 15-sample Soundtracker .MOD files (no 4-byte signature at 1080)
            if (modChannels == 0 && ext == ".mod" && fileSize >= 600) {
                modChannels = 4;
                numModSamples = 15;
                songLenOffset = 470;
                orderTableOffset = 472;
                patDataStart = 600;
            }

            if (modChannels > 0) {
                song = SongData{};
                char titleBuf[21]{};
                std::memcpy(titleBuf, modBytes.data(), 20);
                song.title = titleBuf;
                song.length = std::clamp<uint16_t>(modBytes[songLenOffset], 1, MAX_SEQUENCES);
                song.restart = modBytes[songLenOffset + 1] < song.length ? modBytes[songLenOffset + 1] : 0;
                song.trackLength = 64;
                song.activeSubSong = 0;
                song.subSongs[0].startPos = 0;
                song.bpm = 125;
                song.speedMultiplier = 1;
                currentSpeed = 6;

                for (uint16_t s = 0; s < MAX_SEQUENCES; ++s) {
                    assignIndependentSequencePatterns(s);
                }

                uint8_t maxPat = 0;
                for (int s = 0; s < 128; ++s) {
                    if (modBytes[orderTableOffset + s] > maxPat) maxPat = modBytes[orderTableOffset + s];
                }

                int loadChans = std::min<int>(modChannels, MAX_CHANNELS);
                int seqReadLimit = std::min<int>(128, MAX_SEQUENCES);
                size_t offset = patDataStart;
                size_t bytesPerPattern = size_t(64) * size_t(modChannels) * 4;

                for (int p = 0; p <= int(maxPat); ++p) {
                    if (offset + bytesPerPattern > fileSize) break;
                    for (int r = 0; r < 64; ++r) {
                        for (int c = 0; c < modChannels; ++c) {
                            const uint8_t* d = modBytes.data() + offset;
                            offset += 4;
                            if (c >= loadChans) continue;

                            uint8_t instNum = (d[0] & 0xF0) | (d[2] >> 4);
                            uint16_t period = ((d[0] & 0x0F) << 8) | d[1];
                            uint8_t cmd = d[2] & 0x0F;
                            uint8_t param = d[3];

                            for (int s = 0; s < seqReadLimit; ++s) {
                                if (modBytes[orderTableOffset + s] == p) {
                                    uint8_t chPat = song.sequences[s][c].patternIndex;
                                    PatternCell& targetCell = song.patterns[chPat].rows[r];
                                    targetCell.instrument = std::min<uint8_t>(63, instNum);
                                    targetCell.fxCmd = cmd;
                                    targetCell.fxParam = std::min<uint8_t>(64, param);
                                    if (period > 0) {
                                        double midi = 69.0 + 12.0 * std::log2(3546895.0 / (double(period) * 440.0));
                                        targetCell.note = uint8_t(std::clamp(int(std::round(midi)) - 11, 1, 97));
                                    } else {
                                        targetCell.note = 0;
                                    }
                                }
                            }
                        }
                    }
                }

                for (int s = 0; s < numModSamples && offset < fileSize; ++s) {
                    size_t hdrOff = 20 + size_t(s) * 30;
                    uint32_t smpLenWords = (uint32_t(modBytes[hdrOff + 22]) << 8) | uint32_t(modBytes[hdrOff + 23]);
                    uint32_t smpBytes = smpLenWords * 2;
                    if (smpBytes == 0) continue;

                    size_t avail = fileSize - offset;
                    size_t actualBytes = std::min<size_t>(smpBytes, avail);

                    if (actualBytes > 2 && (s + 1) < MAX_INSTRUMENTS) {
                        Instrument& inst = song.instruments[s + 1];
                        char sName[23]{};
                        std::memcpy(sName, modBytes.data() + hdrOff, 22);
                        inst.name = sName;
                        inst.volume = std::min<uint8_t>(64, modBytes[hdrOff + 25]);
                        int8_t ft = int8_t(modBytes[hdrOff + 24] & 0x0F);
                        if (ft >= 8) ft -= 16;
                        inst.sample.tuneSemitones = std::clamp<int>(ft / 2, -24, 24);
                        inst.hasSample = true;
                        inst.sample.bitDepth = 8;
                        inst.sample.sampleRate = 16574;
                        inst.sample.floatData.resize(actualBytes);
                        for (size_t k = 0; k < actualBytes; ++k) {
                            inst.sample.floatData[k] = int8_t(modBytes[offset + k]) / 128.0f;
                        }
                        uint32_t repStart = ((uint32_t(modBytes[hdrOff + 26]) << 8) | uint32_t(modBytes[hdrOff + 27])) * 2;
                        uint32_t repLen   = ((uint32_t(modBytes[hdrOff + 28]) << 8) | uint32_t(modBytes[hdrOff + 29])) * 2;
                        if (repLen > 2 && repStart < actualBytes) {
                            inst.sample.loopEnabled = true;
                            inst.sample.loopStart = repStart;
                            inst.sample.loopEnd = std::min<size_t>(actualBytes, size_t(repStart) + size_t(repLen));
                            if (inst.sample.loopStart >= inst.sample.loopEnd) inst.sample.loopEnabled = false;
                        }
                    }
                    offset += actualBytes;
                }
                playSeq = 0;
                playRow = 0;
                return true;
            }
        }
        return false;
    }

    bool exportAudioFile(const std::wstring& path, int sampleRate) {
        int totalFrames = sampleRate * 30;
        std::vector<float> pcm(totalFrames * 2, 0.0f);
        renderStereoBlock(pcm.data(), totalFrames, sampleRate);

        std::ofstream f(fs::path(path), std::ios::binary);
        if (!f) return false;

        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".aiff") {
            uint32_t dataBytes = totalFrames * 4;
            uint32_t formLen = 4 + 16 + 8 + dataBytes;
            f.write("FORM", 4);
            uint32_t beForm = _byteswap_ulong(formLen);
            f.write(reinterpret_cast<const char*>(&beForm), 4);
            f.write("AIFF", 4);
            f.write("SSND", 4);
            uint32_t beData = _byteswap_ulong(dataBytes + 8);
            f.write(reinterpret_cast<const char*>(&beData), 4);
            uint32_t zero = 0;
            f.write(reinterpret_cast<const char*>(&zero), 4);
            f.write(reinterpret_cast<const char*>(&zero), 4);
            for (int i = 0; i < totalFrames; ++i) {
                int16_t l = _byteswap_ushort(static_cast<uint16_t>(int16_t(std::clamp(pcm[i * 2], -1.0f, 1.0f) * 32767.0f)));
                int16_t r = _byteswap_ushort(static_cast<uint16_t>(int16_t(std::clamp(pcm[i * 2 + 1], -1.0f, 1.0f) * 32767.0f)));
                f.write(reinterpret_cast<const char*>(&l), 2);
                f.write(reinterpret_cast<const char*>(&r), 2);
            }
        } else {
            uint32_t dataBytes = totalFrames * 4;
            uint32_t riffLen = 36 + dataBytes;
            f.write("RIFF", 4);
            f.write(reinterpret_cast<const char*>(&riffLen), 4);
            f.write("WAVE", 4);
            f.write("fmt ", 4);
            uint32_t fmtLen = 16;
            uint16_t fmtTag = 1;
            uint16_t chans = 2;
            uint32_t sr = sampleRate;
            uint32_t avgBytes = sr * 4;
            uint16_t blockAlign = 4;
            uint16_t bits = 16;
            f.write(reinterpret_cast<const char*>(&fmtLen), 4);
            f.write(reinterpret_cast<const char*>(&fmtTag), 2);
            f.write(reinterpret_cast<const char*>(&chans), 2);
            f.write(reinterpret_cast<const char*>(&sr), 4);
            f.write(reinterpret_cast<const char*>(&avgBytes), 4);
            f.write(reinterpret_cast<const char*>(&blockAlign), 2);
            f.write(reinterpret_cast<const char*>(&bits), 2);
            f.write("data", 4);
            f.write(reinterpret_cast<const char*>(&dataBytes), 4);

            for (int i = 0; i < totalFrames; ++i) {
                int16_t l = static_cast<int16_t>(std::clamp(pcm[i * 2], -1.0f, 1.0f) * 32767.0f);
                int16_t r = static_cast<int16_t>(std::clamp(pcm[i * 2 + 1], -1.0f, 1.0f) * 32767.0f);
                f.write(reinterpret_cast<const char*>(&l), 2);
                f.write(reinterpret_cast<const char*>(&r), 2);
            }
        }
        return true;
    }
};

// ============================================================================
// GLOBAL STATE & WAVE-OUT AUDIO
// ============================================================================
static AHXEngine g_engine;
static ScreenMode g_screen = SCREEN_MAIN;
static FloppyBrowserMode g_floppyMode = FLOPPY_INSTRUMENTS;
static PrefsConfig g_prefs;
static PrefsConfig g_savedPrefs;

static int g_bufferIdx = 4;     // 1024 default
static int g_sampleRateIdx = 0; // 44100 default
static AmigaPointerStyle g_pointerStyle = PTR_WB1X;

static uint8_t g_currentInst = 1;
static uint8_t g_currentOctave = 3;
static int g_activeChannel = 0;
static int g_channelPageOffset = 0;
static int g_editRow = 0;
static int g_editCol = 0;
static int g_posCursorCol = 0;
static bool g_focusPosEditor = false;
static bool g_recordMode = false;

static bool g_hasSelection = false;
static int g_selChannel = 0;
static int g_selStartRow = 0;
static int g_selEndRow = 0;
static std::vector<PatternCell> g_clipboardCells;

static float g_smpSelStart = 0.0f;
static float g_smpSelEnd   = 0.0f;
static bool  g_draggingSample = false;
static float g_smpZoom = 1.0f;
static float g_smpViewOffset = 0.0f;

static std::vector<float> g_sampleClipboard;
static std::vector<float> g_sampleUndoBuffer;
static std::vector<float> g_sampleRedoBuffer;

static int g_editingTextField = 0;
static std::string g_editTextBuffer;
static std::string g_statusMsg = "Ready";
static bool g_pinkExternPrefsExecuted = false;

static int g_perfEditRow = 0;
static int g_perfEditCol = 0;

static int g_themeSlot = 0;
static bool g_inNativeDialog = false;

// Spin button hold-acceleration state (1 click = +/-1, long press = accelerated)
static int g_heldSpinId = 0;
static int g_heldSpinParam = 0;
static uint32_t g_heldSpinTicks = 0;

// Floppy disk directory browser state
struct FloppyItem {
    std::wstring fullPath;
    std::string displayName;
    bool isDir = false;
};
static std::vector<FloppyItem> g_floppyItems;
static int g_floppyScroll = 0;
static int g_floppySelected = 0;
static std::wstring g_floppyCurrentDir;

static HCURSOR g_hCurWB1 = nullptr;
static HCURSOR g_hCurWB2 = nullptr;
static HCURSOR g_hCurMag = nullptr;

static HWAVEOUT g_hWaveOut = nullptr;
static std::atomic<bool> g_audioRunning{false};
static HANDLE g_audioThread = nullptr;

bool IsTestModeActive() {
    return (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
}

DWORD WINAPI AudioThreadProc(LPVOID) {
    int sr = kSampleRates[g_sampleRateIdx];
    int bufFrames = kBufferSizes[g_bufferIdx];

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = sr;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = sr * 4;

    if (waveOutOpen(&g_hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return 0;

    constexpr int NUM_BUFS = 3;
    std::vector<WAVEHDR> hdrs(NUM_BUFS);
    std::vector<std::vector<int16_t>> pcmBufs(NUM_BUFS, std::vector<int16_t>(bufFrames * 2, 0));
    std::vector<float> floatBuf(bufFrames * 2, 0.0f);

    for (int i = 0; i < NUM_BUFS; ++i) {
        hdrs[i].lpData = reinterpret_cast<LPSTR>(pcmBufs[i].data());
        hdrs[i].dwBufferLength = bufFrames * 4;
        waveOutPrepareHeader(g_hWaveOut, &hdrs[i], sizeof(WAVEHDR));
        waveOutWrite(g_hWaveOut, &hdrs[i], sizeof(WAVEHDR));
    }

    while (g_audioRunning.load()) {
        bool anyWritten = false;
        for (int i = 0; i < NUM_BUFS; ++i) {
            if (hdrs[i].dwFlags & WHDR_DONE) {
                LARGE_INTEGER t0, t1, freq;
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&t0);

                g_engine.renderStereoBlock(floatBuf.data(), bufFrames, sr);
                for (int s = 0; s < bufFrames * 2; ++s) {
                    pcmBufs[i][s] = static_cast<int16_t>(std::clamp(floatBuf[s], -1.0f, 1.0f) * 32767.0f);
                }

                QueryPerformanceCounter(&t1);
                double elapsedMs = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
                double budgetMs = (double(bufFrames) / double(sr)) * 1000.0;
                g_engine.cpuUsage = float(std::clamp((elapsedMs / budgetMs) * 100.0, 0.1, 99.9));

                waveOutWrite(g_hWaveOut, &hdrs[i], sizeof(WAVEHDR));
                anyWritten = true;
            }
        }
        if (!anyWritten) Sleep(1);
    }

    waveOutReset(g_hWaveOut);
    for (int i = 0; i < NUM_BUFS; ++i) waveOutUnprepareHeader(g_hWaveOut, &hdrs[i], sizeof(WAVEHDR));
    waveOutClose(g_hWaveOut);
    g_hWaveOut = nullptr;
    return 0;
}

void StopAudioOutput() {
    if (g_audioRunning.load()) {
        g_audioRunning = false;
        if (g_audioThread) {
            WaitForSingleObject(g_audioThread, 1000);
            CloseHandle(g_audioThread);
            g_audioThread = nullptr;
        }
    }
}

void RestartAudioOutput() {
    StopAudioOutput();
    g_audioRunning = true;
    g_audioThread = CreateThread(nullptr, 0, AudioThreadProc, nullptr, 0, nullptr);
}

// ============================================================================
// AMIGA HARDWARE CURSORS
// ============================================================================
HCURSOR BuildAmigaHardwareCursor(int style) {
    const int W = 16, H = 16;
    static const char* wb1Sprite[16] = {
        "OO..............","OXXO............","OXXXO...........","OXXXXO..........",
        "OXXXXXO.........","OXXXXXXO........","OXXXXXXXO.......","OXXXXXXXXO......",
        "OXXXXOOOOO......","OXXXO...........","OXXO............","OXO.............",
        "OO..............","................","................","................"
    };
    static const char* wb2Sprite[16] = {
        "OO..............","O#O.............","O##O............","O###O...........",
        "O####O..........","O#####O.........","O######O........","O#######O.......",
        "O########O......","O#####OOOO......","O##O##O.........","O#O.O##O........",
        "OO..O##O........",".....OO.........","................","................"
    };
    static const char* magSprite[16] = {
        "OO..............","OXXO............","OXXO............","OXXO............",
        "OXXO............","OXXO............","OXXO............","OXXO............",
        "OXXOOOO.........","OOXOOXOO........","O.O..O.O........","................",
        "................","................","................","................"
    };

    HDC hdc = GetDC(nullptr);
    HBITMAP hColor = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP hMask  = CreateCompatibleBitmap(hdc, W, H);
    HDC dcCol = CreateCompatibleDC(hdc);
    HDC dcMsk = CreateCompatibleDC(hdc);
    HGDIOBJ oldC = SelectObject(dcCol, hColor);
    HGDIOBJ oldM = SelectObject(dcMsk, hMask);

    const char** spr = (style == 0) ? wb1Sprite : ((style == 1) ? wb2Sprite : magSprite);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            char c = spr[y][x];
            if (c == '.') {
                SetPixel(dcMsk, x, y, RGB(255, 255, 255));
                SetPixel(dcCol, x, y, RGB(0, 0, 0));
            } else {
                SetPixel(dcMsk, x, y, RGB(0, 0, 0));
                if (c == 'O') SetPixel(dcCol, x, y, RGB(0, 0, 0));
                else if (c == 'X') SetPixel(dcCol, x, y, RGB(255, 110, 40));
                else if (c == '#') SetPixel(dcCol, x, y, RGB(245, 245, 245));
                else SetPixel(dcCol, x, y, RGB(220, 220, 220));
            }
        }
    }

    SelectObject(dcCol, oldC);
    SelectObject(dcMsk, oldM);
    DeleteDC(dcCol);
    DeleteDC(dcMsk);
    ReleaseDC(nullptr, hdc);

    ICONINFO ii{};
    ii.fIcon = FALSE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask = hMask;
    ii.hbmColor = hColor;
    HCURSOR hCur = CreateIconIndirect(&ii);

    DeleteObject(hColor);
    DeleteObject(hMask);
    return hCur;
}

void ApplyActiveCursor() {
    if (g_inNativeDialog) SetCursor(LoadCursor(nullptr, IDC_ARROW));
    else if (g_pointerStyle == PTR_WB1X && g_hCurWB1) SetCursor(g_hCurWB1);
    else if (g_pointerStyle == PTR_WB2X && g_hCurWB2) SetCursor(g_hCurWB2);
    else if (g_pointerStyle == PTR_MAGIC_MOUSE && g_hCurMag) SetCursor(g_hCurMag);
    else SetCursor(g_hCurWB1);
}

// ============================================================================
// NATIVE FRAMEBUFFER
// ============================================================================
struct AmigaNativeBuffer {
    uint32_t* pixels = nullptr;
    HBITMAP hBmp = nullptr;
    HDC memDC = nullptr;
    HGDIOBJ oldBmp = nullptr;

    void init(HDC refDC) {
        if (pixels) return;
        memDC = CreateCompatibleDC(refDC);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = NATIVE_W;
        bmi.bmiHeader.biHeight = -NATIVE_H;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        hBmp = CreateDIBSection(refDC, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        oldBmp = SelectObject(memDC, hBmp);
    }

    void destroy() {
        if (memDC) {
            if (oldBmp) SelectObject(memDC, oldBmp);
            if (hBmp) DeleteObject(hBmp);
            DeleteDC(memDC);
        }
        memDC = nullptr;
        hBmp = nullptr;
        pixels = nullptr;
    }

    static inline uint32_t toBGRA(COLORREF c) {
        return 0xFF000000u | (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
    }

    inline void fillRect(int x, int y, int w, int h, COLORREF col) {
        int x0 = std::max(0, x), y0 = std::max(0, y);
        int x1 = std::min(NATIVE_W, x + w), y1 = std::min(NATIVE_H, y + h);
        if (x0 >= x1 || y0 >= y1) return;
        uint32_t c = toBGRA(col);
        for (int py = y0; py < y1; ++py) {
            uint32_t* row = pixels + py * NATIVE_W;
            std::fill(row + x0, row + x1, c);
        }
    }

    inline void setPixel(int x, int y, COLORREF col) {
        if (x >= 0 && x < NATIVE_W && y >= 0 && y < NATIVE_H) {
            pixels[y * NATIVE_W + x] = toBGRA(col);
        }
    }

    inline void drawRectOutline(int x, int y, int w, int h, COLORREF col) {
        fillRect(x, y, w, 1, col);
        fillRect(x, y + h - 1, w, 1, col);
        fillRect(x, y, 1, h, col);
        fillRect(x + w - 1, y, 1, h, col);
    }
};

static AmigaNativeBuffer g_fb;

// ============================================================================
// PROTRACKER / AHX 8x8 FONT & COMPACT 4x6 FONT
// ============================================================================
static const uint8_t kPTFont8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},{0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x00},
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00},{0x24,0x7E,0x24,0x24,0x7E,0x24,0x00,0x00},
    {0x18,0x3E,0x58,0x3C,0x1A,0x7C,0x18,0x00},{0x62,0x64,0x08,0x10,0x26,0x46,0x00,0x00},
    {0x38,0x44,0x38,0x4E,0x44,0x3A,0x00,0x00},{0x18,0x18,0x10,0x00,0x00,0x00,0x00,0x00},
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},{0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    {0x00,0x24,0x18,0x7E,0x18,0x24,0x00,0x00},{0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},{0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},{0x02,0x04,0x08,0x10,0x20,0x40,0x00,0x00},
    {0x3C,0x66,0x6E,0x76,0x66,0x3C,0x00,0x00},{0x18,0x38,0x18,0x18,0x18,0x7E,0x00,0x00},
    {0x3C,0x66,0x06,0x1C,0x30,0x7E,0x00,0x00},{0x3C,0x66,0x1C,0x06,0x66,0x3C,0x00,0x00},
    {0x0E,0x1E,0x36,0x66,0x7F,0x06,0x00,0x00},{0x7E,0x60,0x7C,0x06,0x66,0x3C,0x00,0x00},
    {0x1C,0x30,0x7C,0x66,0x66,0x3C,0x00,0x00},{0x7E,0x06,0x0C,0x18,0x18,0x18,0x00,0x00},
    {0x3C,0x66,0x3C,0x66,0x66,0x3C,0x00,0x00},{0x3C,0x66,0x3E,0x06,0x0C,0x38,0x00,0x00},
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},{0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00},
    {0x0C,0x18,0x30,0x18,0x0C,0x00,0x00,0x00},{0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    {0x30,0x18,0x0C,0x18,0x30,0x00,0x00,0x00},{0x3C,0x66,0x0C,0x18,0x00,0x18,0x00,0x00},
    {0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x3C,0x00},{0x3C,0x66,0x66,0x7E,0x66,0x66,0x00,0x00},
    {0x7C,0x66,0x7C,0x66,0x66,0x7C,0x00,0x00},{0x3C,0x66,0x60,0x60,0x66,0x3C,0x00,0x00},
    {0x78,0x6C,0x66,0x66,0x6C,0x78,0x00,0x00},{0x7E,0x60,0x7C,0x60,0x60,0x7E,0x00,0x00},
    {0x7E,0x60,0x7C,0x60,0x60,0x60,0x00,0x00},{0x3C,0x66,0x60,0x6E,0x66,0x3E,0x00,0x00},
    {0x66,0x66,0x7E,0x66,0x66,0x66,0x00,0x00},{0x7E,0x18,0x18,0x18,0x18,0x7E,0x00,0x00},
    {0x06,0x06,0x06,0x06,0x66,0x3C,0x00,0x00},{0x66,0x6C,0x78,0x6C,0x66,0x66,0x00,0x00},
    {0x60,0x60,0x60,0x60,0x60,0x7E,0x00,0x00},{0x63,0x77,0x7F,0x6B,0x63,0x63,0x00,0x00},
    {0x66,0x76,0x7E,0x6E,0x66,0x66,0x00,0x00},{0x3C,0x66,0x66,0x66,0x66,0x3C,0x00,0x00},
    {0x7C,0x66,0x7C,0x60,0x60,0x60,0x00,0x00},{0x3C,0x66,0x66,0x6E,0x3C,0x06,0x00,0x00},
    {0x7C,0x66,0x7C,0x6C,0x66,0x66,0x00,0x00},{0x3C,0x60,0x3C,0x06,0x66,0x3C,0x00,0x00},
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x00,0x00},{0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00},
    {0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00},{0x63,0x63,0x6B,0x7F,0x77,0x63,0x00,0x00},
    {0x66,0x3C,0x18,0x3C,0x66,0x66,0x00,0x00},{0x66,0x66,0x3C,0x18,0x18,0x18,0x00,0x00},
    {0x7E,0x0C,0x18,0x30,0x60,0x7E,0x00,0x00},{0x3C,0x30,0x30,0x30,0x30,0x3C,0x00,0x00},
    {0x40,0x20,0x10,0x08,0x04,0x02,0x00,0x00},{0x3C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00},
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00},
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00},
    {0x60,0x60,0x7C,0x66,0x66,0x7C,0x00,0x00},{0x00,0x00,0x3C,0x60,0x60,0x3C,0x00,0x00},
    {0x06,0x06,0x3E,0x66,0x66,0x3E,0x00,0x00},{0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
    {0x1C,0x30,0x7C,0x30,0x30,0x30,0x00,0x00},{0x00,0x00,0x3E,0x66,0x3E,0x06,0x3C,0x00},
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x00,0x00},{0x18,0x00,0x38,0x18,0x18,0x3C,0x00,0x00},
    {0x0C,0x00,0x0C,0x0C,0x0C,0x4C,0x38,0x00},{0x60,0x60,0x66,0x6C,0x78,0x6C,0x00,0x00},
    {0x38,0x18,0x18,0x18,0x18,0x3C,0x00,0x00},{0x00,0x00,0x66,0x7F,0x7F,0x6B,0x00,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x00,0x00},{0x00,0x00,0x3C,0x66,0x66,0x3C,0x00,0x00},
    {0x00,0x00,0x7C,0x66,0x7C,0x60,0x60,0x00},{0x00,0x00,0x3E,0x66,0x3E,0x06,0x06,0x00},
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x00,0x00},{0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
    {0x30,0x30,0x7C,0x30,0x30,0x1C,0x00,0x00},{0x00,0x00,0x66,0x66,0x66,0x3E,0x00,0x00},
    {0x00,0x00,0x66,0x66,0x3C,0x18,0x00,0x00},{0x00,0x00,0x63,0x6B,0x7F,0x36,0x00,0x00},
    {0x00,0x00,0x66,0x3C,0x3C,0x66,0x00,0x00},{0x00,0x00,0x66,0x66,0x3E,0x06,0x3C,0x00},
    {0x00,0x00,0x7E,0x0C,0x30,0x7E,0x00,0x00},{0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00},{0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    {0x32,0x4C,0x00,0x00,0x00,0x00,0x00,0x00},{0x18,0x3C,0x7E,0x3C,0x18,0x00,0x00,0x00}
};

void DrawTextPT(int x, int y, const std::string& txt, COLORREF fg, bool shadow = true, int advance = 8) {
    for (size_t i = 0; i < txt.size(); ++i) {
        int cx = x + int(i) * advance;
        unsigned char ch = static_cast<unsigned char>(txt[i]);
        int idx = (ch == 0x7F) ? 95 : ((ch >= 32 && ch < 127) ? (ch - 32) : 0);
        for (int row = 0; row < 8; ++row) {
            uint8_t bits = kPTFont8x8[idx][row];
            if (!bits) continue;
            for (int bit = 0; bit < 8; ++bit) {
                if (bits & (0x80 >> bit)) {
                    if (shadow) g_fb.setPixel(cx + bit + 1, y + row + 1, g_theme.colBlack);
                    g_fb.setPixel(cx + bit, y + row, fg);
                }
            }
        }
    }
}

void DrawTextMicro(int x, int y, const std::string& txt, COLORREF fg) {
    for (size_t i = 0; i < txt.size(); ++i) {
        int cx = x + int(i) * 5;
        unsigned char ch = static_cast<unsigned char>(txt[i]);
        int idx = (ch == 0x7F) ? 95 : ((ch >= 32 && ch < 127) ? (ch - 32) : 0);
        for (int row = 0; row < 6; ++row) {
            uint8_t bits = kPTFont8x8[idx][row];
            if (!bits) continue;
            for (int bit = 0; bit < 4; ++bit) {
                uint8_t mask = (0xC0 >> (bit * 2));
                if (bits & mask) {
                    g_fb.setPixel(cx + bit, y + row, fg);
                }
            }
        }
    }
}

// ============================================================================
// PIXEL-ART ICONS
// ============================================================================
void DrawExactArrowButton(int x, int y, int w, int h, bool up) {
    g_fb.fillRect(x, y, w, h, g_theme.colGrayMain);
    g_fb.fillRect(x, y, w, 1, g_theme.colWhite);
    g_fb.fillRect(x, y, 1, h, g_theme.colWhite);
    g_fb.fillRect(x, y + h - 1, w, 1, g_theme.colGrayDark);
    g_fb.fillRect(x + w - 1, y, 1, h, g_theme.colGrayDark);

    int cx = x + w / 2;
    int cy = y + h / 2;
    if (up) {
        g_fb.setPixel(cx + 1, cy - 2, g_theme.colBlack);
        g_fb.fillRect(cx, cy - 1, 3, 1, g_theme.colBlack);
        g_fb.fillRect(cx - 1, cy, 5, 1, g_theme.colBlack);
        g_fb.fillRect(cx, cy + 1, 3, 3, g_theme.colBlack);

        g_fb.setPixel(cx, cy - 3, g_theme.colWhite);
        g_fb.fillRect(cx - 1, cy - 2, 3, 1, g_theme.colWhite);
        g_fb.fillRect(cx - 2, cy - 1, 5, 1, g_theme.colWhite);
        g_fb.fillRect(cx - 1, cy, 3, 3, g_theme.colWhite);
    } else {
        g_fb.fillRect(cx, cy - 2, 3, 3, g_theme.colBlack);
        g_fb.fillRect(cx - 1, cy + 1, 5, 1, g_theme.colBlack);
        g_fb.fillRect(cx, cy + 2, 3, 1, g_theme.colBlack);
        g_fb.setPixel(cx + 1, cy + 3, g_theme.colBlack);

        g_fb.fillRect(cx - 1, cy - 3, 3, 3, g_theme.colWhite);
        g_fb.fillRect(cx - 2, cy, 5, 1, g_theme.colWhite);
        g_fb.fillRect(cx - 1, cy + 1, 3, 1, g_theme.colWhite);
        g_fb.setPixel(cx, cy + 2, g_theme.colWhite);
    }
}

void DrawExactSpeakerButton(int x, int y, int w, int h, bool muted) {
    g_fb.fillRect(x, y, w, h, g_theme.colGrayMain);
    g_fb.fillRect(x, y, w, 1, g_theme.colWhite);
    g_fb.fillRect(x, y, 1, h, g_theme.colWhite);
    g_fb.fillRect(x, y + h - 1, w, 1, g_theme.colGrayDark);
    g_fb.fillRect(x + w - 1, y, 1, h, g_theme.colGrayDark);

    int cx = x + w / 2;
    int cy = y + h / 2;
    g_fb.fillRect(cx - 2, cy - 1, 2, 4, g_theme.colBlack);
    g_fb.fillRect(cx,     cy - 2, 1, 6, g_theme.colBlack);
    g_fb.fillRect(cx + 1, cy - 3, 2, 8, g_theme.colBlack);

    g_fb.fillRect(cx - 3, cy - 2, 2, 4, g_theme.colWhite);
    g_fb.fillRect(cx - 1, cy - 3, 1, 6, g_theme.colWhite);
    g_fb.fillRect(cx,     cy - 4, 2, 8, g_theme.colWhite);

    if (muted) {
        for (int d = -3; d <= 3; ++d) {
            g_fb.setPixel(cx + d, cy + d, RGB(220, 40, 40));
            g_fb.setPixel(cx + d, cy - d, RGB(220, 40, 40));
        }
    }
}

void DrawBevel(int x, int y, int w, int h, bool recessed, COLORREF fill) {
    g_fb.fillRect(x, y, w, h, fill);
    COLORREF tl = recessed ? g_theme.colGrayDark : g_theme.colWhite;
    COLORREF br = recessed ? g_theme.colWhite : g_theme.colGrayDark;
    g_fb.fillRect(x, y, w, 1, tl);
    g_fb.fillRect(x, y, 1, h, tl);
    g_fb.fillRect(x, y + h - 1, w, 1, br);
    g_fb.fillRect(x + w - 1, y, 1, h, br);
}

// ============================================================================
// HITBOX SYSTEM
// ============================================================================
struct HitBox {
    RECT rc;
    int id;
    int param;
    bool isSpin;
};
static std::vector<HitBox> g_hitBoxes;

enum HitID {
    HIT_NONE = 0,
    HIT_TOP_TOGGLE,
    HIT_ABYSS_DEXTER_BTN,
    HIT_DINK_BTN,
    HIT_PERF_EDITOR_BTN,
    HIT_POS_INC, HIT_POS_DEC,
    HIT_LEN_INC, HIT_LEN_DEC,
    HIT_RES_INC, HIT_RES_DEC,
    HIT_TRL_INC, HIT_TRL_DEC,
    HIT_SS_INC, HIT_SS_DEC,
    HIT_SSN_INC, HIT_SSN_DEC,
    HIT_SSP_INC, HIT_SSP_DEC,
    HIT_PLAY, HIT_STOP, HIT_INS_BTN,
    HIT_INS_INC, HIT_INS_DEC,
    HIT_LOAD_SONG, HIT_LOAD_INS,
    HIT_EDIT_SONG_NAME, HIT_EDIT_INST_NAME,
    HIT_CH_PAT_INC, HIT_CH_PAT_DEC, HIT_CH_MUTE,
    HIT_CH_PAGE_UP, HIT_CH_PAGE_DN,
    HIT_CH_SELECT,
    HIT_MAIN_BTN,
    HIT_INST_PARAM_INC, HIT_INST_PARAM_DEC,
    HIT_RELEASE_CUT, HIT_TEST_MODE,
    HIT_PERF_CELL,
    HIT_WAVE_ITEM,
    HIT_CMD_ITEM,
    HIT_THEME_SLOT,
    HIT_THEME_RGB_INC, HIT_THEME_RGB_DEC,
    HIT_THEME_RESET,
    HIT_THEME_SAVE,
    HIT_FILTER_CYCLE,
    HIT_AUDIO_BUF_SET,
    HIT_AUDIO_SR_SET,
    HIT_SMP_WAVE_AREA,
    HIT_SMP_NORM,
    HIT_SMP_REV,
    HIT_SMP_LOOP,
    HIT_SMP_FIN,
    HIT_SMP_FOUT,
    HIT_SMP_DEL,
    HIT_SMP_ZOOM_IN,
    HIT_SMP_ZOOM_OUT,
    HIT_SMP_AMP_INC, HIT_SMP_AMP_DEC,
    HIT_SMP_PAN_INC, HIT_SMP_PAN_DEC,
    HIT_SMP_TUN_INC, HIT_SMP_TUN_DEC,
    HIT_SMP_REL_INC, HIT_SMP_REL_DEC,
    HIT_SMP_PLAY_TEST,
    HIT_PREFS_SONGS_DIR, HIT_PREFS_INSTR_DIR,
    HIT_PREFS_CHK_VBL, HIT_PREFS_CHK_ALLOC, HIT_PREFS_CHK_FONT, HIT_PREFS_CHK_TRANS,
    HIT_PREFS_CYC_VMETER, HIT_PREFS_CYC_CURSOR, HIT_PREFS_CYC_RASTER,
    HIT_PREFS_SAVE, HIT_PREFS_CURRENT, HIT_PREFS_CANCEL,
    HIT_FLOPPY_ITEM, HIT_FLOPPY_PARENT, HIT_FLOPPY_SYSTEM_DLG, HIT_FLOPPY_LOAD_BTN
};

void RegisterHit(int x, int y, int w, int h, int id, int param = 0, bool isSpin = false) {
    g_hitBoxes.push_back({{x, y, x + w, y + h}, id, param, isSpin});
}

void DrawButton320(int x, int y, int w, int h, const std::string& label, int id, int param = 0, bool dither = false) {
    DrawBevel(x, y, w, h, false, g_theme.colGrayMain);
    if (dither) {
        for (int py = y + 1; py < y + h - 1; ++py) {
            for (int px = x + 1 + (py & 1); px < x + w - 1; px += 2) {
                g_fb.setPixel(px, py, g_theme.colBlack);
            }
        }
    }
    int tx = x + (w - int(label.size()) * 8) / 2;
    int ty = y + (h - 8) / 2;
    DrawTextPT(std::max(x + 1, tx), ty, label, g_theme.colWhite, true, 8);
    if (id != HIT_NONE) RegisterHit(x, y, w, h, id, param, false);
}

void DrawSpinPair320(int x, int y, int w, int h, int incId, int decId, int param = 0) {
    int hw = w / 2;
    DrawExactArrowButton(x, y, hw, h, true);
    RegisterHit(x, y, hw, h, incId, param, true);
    DrawExactArrowButton(x + hw, y, w - hw, h, false);
    RegisterHit(x + hw, y, w - hw, h, decId, param, true);
}

std::string FormatNote(uint8_t note) {
    if (note == 0) return "---";
    static const char* names[12] = {"C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"};
    int n = (note - 1) % 12;
    int oct = (note - 1) / 12;
    char buf[8];
    snprintf(buf, sizeof(buf), "%s%d", names[n], oct);
    return buf;
}

std::string Dec3(int v) { char b[8]; snprintf(b, sizeof(b), "%03d", std::clamp(v, 0, 999)); return b; }
std::string Dec2(int v) { char b[8]; snprintf(b, sizeof(b), "%02d", std::clamp(v, 0, 99)); return b; }
std::string Hex3(uint16_t v) { char b[8]; snprintf(b, sizeof(b), "%03X", v & 0xFFF); return b; }

// ============================================================================
// FLOPPY DISK DIRECTORY EMULATION LOADER
// ============================================================================
void RefreshFloppyDirectory(const std::wstring& dirPath) {
    g_floppyItems.clear();
    g_floppyScroll = 0;
    g_floppySelected = 0;

    std::error_code ec;
    fs::path p(dirPath);
    if (dirPath.empty() || dirPath == L"&:" || dirPath == L"/    Instruments" || !fs::exists(p, ec) || !fs::is_directory(p, ec)) {
        p = fs::current_path(ec);
    }
    g_floppyCurrentDir = p.wstring();

    for (const auto& entry : fs::directory_iterator(p, ec)) {
        if (ec) break;
        FloppyItem item;
        item.fullPath = entry.path().wstring();
        std::string fn = entry.path().filename().string();
        if (entry.is_directory(ec)) {
            item.isDir = true;
            item.displayName = "[DIR] " + fn;
            g_floppyItems.push_back(item);
        } else if (entry.is_regular_file(ec)) {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (g_floppyMode == FLOPPY_SONGS) {
                if (ext == ".mod" || ext == ".xm" || ext == ".if") {
                    item.isDir = false;
                    item.displayName = fn;
                    g_floppyItems.push_back(item);
                }
            } else {
                if (ext == ".wav" || ext == ".aiff" || ext == ".mp3") {
                    item.isDir = false;
                    item.displayName = fn;
                    g_floppyItems.push_back(item);
                }
            }
        }
    }

    std::sort(g_floppyItems.begin(), g_floppyItems.end(), [](const FloppyItem& a, const FloppyItem& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return a.displayName < b.displayName;
    });
}

void OpenFloppyBrowser(FloppyBrowserMode mode) {
    g_floppyMode = mode;
    std::wstring target = (mode == FLOPPY_SONGS) ? g_prefs.songsDir : g_prefs.instrDir;
    RefreshFloppyDirectory(target);
    g_screen = SCREEN_FLOPPY_BROWSER;
}

// ============================================================================
// SCREEN 1: MAIN TRACKER
// ============================================================================
void RenderMainScreen320() {
    g_fb.fillRect(0, 0, NATIVE_W, NATIVE_H, g_theme.colGrayMain);

    g_fb.fillRect(0, 0, NATIVE_W, 10, g_theme.colWhite);
    DrawTextPT(2, 1, "AHX TRACKER - ABYSS / DEXTER & PINK / QST NATIVE", g_theme.colBlack, false, 6);
    RegisterHit(172, 0, 134, 10, HIT_ABYSS_DEXTER_BTN);

    DrawBevel(306, 0, 14, 10, false, g_theme.colGrayMain);
    g_fb.fillRect(309, 2, 5, 4, g_theme.colBlack);
    g_fb.fillRect(311, 4, 6, 4, g_theme.colWhite);
    g_fb.drawRectOutline(311, 4, 6, 4, g_theme.colBlack);
    RegisterHit(306, 0, 14, 10, HIT_TOP_TOGGLE);

    uint16_t curSeq = g_engine.playSeq.load();
    struct LRow { const char* lbl; std::string val; int inc; int dec; };
    LRow lrows[7] = {
        {"POS", Dec3(curSeq), HIT_POS_INC, HIT_POS_DEC},
        {"LEN", Dec3(g_engine.song.length), HIT_LEN_INC, HIT_LEN_DEC},
        {"RES", Dec3(g_engine.song.restart), HIT_RES_INC, HIT_RES_DEC},
        {"TRL", Dec3(g_engine.song.trackLength), HIT_TRL_INC, HIT_TRL_DEC},
        {"SS",  Dec3(g_engine.song.speedMultiplier), HIT_SS_INC, HIT_SS_DEC},
        {"SSN", Dec3(g_engine.song.bpm), HIT_SSN_INC, HIT_SSN_DEC},
        {"SSP", Dec3(g_engine.song.subSongs[g_engine.song.activeSubSong].startPos), HIT_SSP_INC, HIT_SSP_DEC}
    };

    for (int i = 0; i < 7; ++i) {
        int ry = 11 + i * 11;
        DrawBevel(0, ry, 26, 11, false, g_theme.colGrayMain);
        DrawTextPT(2, ry + 2, lrows[i].lbl, g_theme.colWhite, true, 8);

        DrawBevel(26, ry, 26, 11, true, g_theme.colBlack);
        DrawTextPT(27, ry + 2, lrows[i].val, g_theme.colWhite, false, 8);
        DrawSpinPair320(52, ry, 20, 11, lrows[i].inc, lrows[i].dec);
    }

    // Sequence Matrix (000..063 built by LEN)
    DrawBevel(72, 11, 248, 77, true, g_theme.colGrayLight);
    g_fb.fillRect(98, 12, 1, 75, g_theme.colGrayDark);

    for (int r = -3; r <= 3; ++r) {
        int sIdx = int(curSeq) + r;
        int ry = 11 + (r + 3) * 11;
        bool isCenter = (r == 0);
        if (isCenter) {
            DrawBevel(73, ry, 246, 11, false, g_theme.colGrayDark);
        }
        if (sIdx >= 0 && sIdx < int(g_engine.song.length)) {
            DrawTextPT(74, ry + 2, Dec3(sIdx), g_theme.colWhite, isCenter, 8);
            for (int vc = 0; vc < VISIBLE_CHANNELS; ++vc) {
                int ch = g_channelPageOffset + vc;
                int cx = 100 + vc * 55;
                const TrackEntry& te = g_engine.song.sequences[sIdx][ch];
                char entryStr[16];
                snprintf(entryStr, sizeof(entryStr), "%03d-%02X", te.patternIndex, static_cast<uint8_t>(te.transpose));
                DrawTextPT(cx, ry + 2, entryStr, g_theme.colWhite, isCenter, 8);

                if (isCenter && (g_posCursorCol / 2 == vc)) {
                    int bx = cx + ((g_posCursorCol % 2 == 0) ? -1 : 31);
                    int bw = (g_posCursorCol % 2 == 0) ? 10 : 18;
                    g_fb.drawRectOutline(bx, ry + 1, bw, 9, g_theme.colBlueMeter);
                }
            }
        }
    }

    DrawBevel(72, 88, 34, 11, false, g_theme.colGrayMain);
    DrawTextPT(73, 90, "NAME", g_theme.colWhite, true, 8);
    DrawBevel(106, 88, 178, 11, true, g_theme.colBlack);
    std::string dispSong = (g_editingTextField == 1) ? (g_editTextBuffer + "_") : g_engine.song.title;
    DrawTextPT(108, 90, dispSong, g_theme.colWhite, false, 8);
    RegisterHit(106, 88, 178, 11, HIT_EDIT_SONG_NAME);
    DrawButton320(284, 88, 36, 11, "LOAD", HIT_LOAD_SONG);

    DrawButton320(0,  99, 36, 11, "PLAY", HIT_PLAY);
    DrawButton320(36, 99, 36, 11, "STOP", HIT_STOP);
    DrawButton320(72, 99, 26, 11, "INS",  HIT_INS_BTN);
    DrawBevel(98, 99, 18, 11, true, g_theme.colBlack);
    DrawTextPT(99, 101, Dec2(g_currentInst), g_theme.colWhite, false, 8);
    DrawSpinPair320(116, 99, 20, 11, HIT_INS_INC, HIT_INS_DEC);

    DrawBevel(136, 99, 148, 11, true, g_theme.colBlack);
    std::string dispInst = (g_editingTextField == 2) ? (g_editTextBuffer + "_") : g_engine.song.instruments[g_currentInst].name;
    DrawTextPT(138, 101, dispInst, g_theme.colWhite, false, 8);
    RegisterHit(136, 99, 148, 11, HIT_EDIT_INST_NAME);
    DrawButton320(284, 99, 36, 11, "LOAD", HIT_LOAD_INS);

    DrawBevel(0, 110, NATIVE_W, 11, false, g_theme.colGrayMain);
    std::string st = g_engine.isPlaying.load() ? "STATUS:Playing..." : (g_recordMode ? "STATUS:REC [INS]" : ("STATUS:" + g_statusMsg));
    DrawTextPT(2, 112, st, g_theme.colWhite, true, 8);

    DrawBevel(140, 111, 52, 9, true, g_theme.colBlack);
    if (g_prefs.rastertimeMode == 1 && g_engine.isPlaying.load()) {
        int barW = static_cast<int>((g_engine.cpuUsage.load() / 100.0f) * 50);
        g_fb.fillRect(141, 112, barW, 7, g_theme.colBlueMeter);
    }

    uint32_t secs = g_engine.elapsedSeconds.load();
    char statRight[32];
    snprintf(statRight, sizeof(statRight), "%02d%% %02u:%02u/%02d",
             int(g_engine.cpuUsage.load()), secs / 60, secs % 60, g_engine.currentSpeed.load());
    DrawTextPT(220, 112, statRight, g_theme.colWhite, true, 8);

    DrawSpinPair320(0, 121, 20, 12, HIT_CH_PAGE_UP, HIT_CH_PAGE_DN);
    DrawBevel(20, 121, 11, 12, true, g_theme.colBlack);
    DrawTextPT(22, 123, std::to_string((g_channelPageOffset / 4) + 1), g_theme.colWhite, false, 8);

    for (int vc = 0; vc < VISIBLE_CHANNELS; ++vc) {
        int ch = g_channelPageOffset + vc;
        int cx = 31 + vc * 72;
        uint8_t patIdx = g_engine.song.sequences[curSeq][ch].patternIndex;

        DrawBevel(cx, 121, 35, 12, true, (ch == g_activeChannel) ? g_theme.colGrayDark : g_theme.colBlack);
        DrawTextPT(cx + 2, 123, Dec3(patIdx), g_theme.colWhite, false, 8);
        RegisterHit(cx, 121, 35, 12, HIT_CH_SELECT, ch);
        DrawSpinPair320(cx + 35, 121, 22, 12, HIT_CH_PAT_INC, HIT_CH_PAT_DEC, ch);
        DrawExactSpeakerButton(cx + 57, 121, 15, 12, g_engine.voices[ch].muted);
        RegisterHit(cx + 57, 121, 15, 12, HIT_CH_MUTE, ch);
    }

    int gridY = 133;
    int rowH = 8;
    int visibleRows = 15;
    int centerRow = g_engine.isPlaying.load() ? g_engine.playRow.load() : g_editRow;
    int startRow = centerRow - 7;

    g_fb.fillRect(0, gridY, NATIVE_W, NATIVE_H - gridY, g_theme.colBlack);
    g_fb.fillRect(12, gridY, 1, NATIVE_H - gridY, g_theme.colWhite);
    g_fb.fillRect(30, gridY, 1, NATIVE_H - gridY, g_theme.colWhite);

    int selR0 = std::min(g_selStartRow, g_selEndRow);
    int selR1 = std::max(g_selStartRow, g_selEndRow);

    for (int vr = 0; vr < visibleRows; ++vr) {
        int rIdx = startRow + vr;
        int ry = gridY + 1 + vr * rowH;
        bool isCenter = (rIdx == centerRow);

        if (isCenter) {
            DrawBevel(13, ry - 1, NATIVE_W - 14, rowH + 1, false, g_theme.colGrayMain);
        }

        if (rIdx >= 0 && rIdx < g_engine.song.trackLength) {
            DrawTextPT(13, ry, Dec2(rIdx), g_theme.colWhite, isCenter, 8);

            for (int vc = 0; vc < VISIBLE_CHANNELS; ++vc) {
                int ch = g_channelPageOffset + vc;
                int cx = 31 + vc * 72;
                uint8_t dispPatIdx = g_engine.song.sequences[curSeq][ch].patternIndex;
                uint8_t patIdx = uint8_t((int(dispPatIdx) * MAX_CHANNELS + ch) % MAX_PATTERNS);
                const PatternCell& cell = g_engine.song.patterns[patIdx].rows[rIdx];

                if (g_hasSelection && ch == g_selChannel && rIdx >= selR0 && rIdx <= selR1) {
                    g_fb.fillRect(cx + 1, ry - 1, 70, rowH, g_theme.colBlueMeter);
                }

                RegisterHit(cx + 1, ry - 1, 70, rowH, HIT_CH_SELECT, ch);

                char cellTxt[16];
                snprintf(cellTxt, sizeof(cellTxt), "%s%02d%01X%02d",
                         FormatNote(cell.note).c_str(), cell.instrument, cell.fxCmd,
                         std::clamp<int>(cell.fxParam, 0, 64));
                DrawTextPT(cx + 4, ry, cellTxt, g_theme.colWhite, isCenter, 8);

                if (vc < 3) {
                    g_fb.fillRect(cx + 71, gridY, 1, NATIVE_H - gridY, g_theme.colWhite);
                }

                if (!g_focusPosEditor && isCenter && ch == g_activeChannel) {
                    int curX = cx + 4 + (g_editCol == 0 ? 0 : (g_editCol + 2) * 8);
                    int curW = (g_editCol == 0) ? 24 : 8;
                    g_fb.drawRectOutline(curX - 1, ry - 1, curW + 2, rowH + 1, g_theme.colBlueMeter);
                }
            }
        }
    }

    if (g_prefs.volumeMeterMode != 2) {
        for (int vc = 0; vc < VISIBLE_CHANNELS; ++vc) {
            int ch = g_channelPageOffset + vc;
            int cx = 31 + vc * 72;
            float lvl = g_engine.voices[ch].peakMeter;
            if (lvl > 0.01f) {
                int maxH = 7 * rowH;
                int barH = static_cast<int>(lvl * maxH);
                int barY = (gridY + 1 + 7 * rowH) - barH - 1;
                int barX = cx + 27;
                if (g_prefs.transparentVolumeMeter) {
                    for (int my = barY; my < barY + barH; my += 2)
                        g_fb.fillRect(barX, my, 17, 1, g_theme.colBlueMeter);
                } else {
                    g_fb.fillRect(barX, barY, 17, barH, g_theme.colBlueMeter);
                }
            }
        }
    }
}

// ============================================================================
// SCREEN 2: INSTRUMENT & PERFORMANCE EDITOR
// ============================================================================
void RenderInstrumentScreen320() {
    g_fb.fillRect(0, 0, NATIVE_W, NATIVE_H, g_theme.colGrayMain);
    Instrument& inst = g_engine.song.instruments[g_currentInst];

    DrawButton320(0,  0, 38, 12, "MAIN", HIT_MAIN_BTN);
    DrawBevel(38, 0, 18, 12, true, g_theme.colBlack);
    DrawTextPT(39, 2, Dec2(g_currentInst), g_theme.colWhite, false, 8);
    DrawSpinPair320(56, 0, 20, 12, HIT_INS_INC, HIT_INS_DEC);

    DrawBevel(76, 0, 148, 12, true, g_theme.colBlack);
    std::string dispInst = (g_editingTextField == 2) ? (g_editTextBuffer + "_") : inst.name;
    DrawTextPT(78, 2, dispInst, g_theme.colWhite, false, 8);
    RegisterHit(76, 0, 148, 12, HIT_EDIT_INST_NAME);

    DrawButton320(224, 0, 36, 12, "LOAD", HIT_LOAD_INS);

    g_fb.fillRect(260, 0, 44, 12, g_theme.colWhite);
    DrawTextPT(262, 2, "diNk.", g_theme.colWhite, true, 8);
    RegisterHit(260, 0, 44, 12, HIT_DINK_BTN);

    DrawBevel(304, 0, 16, 12, false, g_theme.colGrayMain);
    g_fb.fillRect(307, 2, 6, 4, g_theme.colBlack);
    g_fb.fillRect(309, 4, 7, 5, g_theme.colWhite);
    g_fb.drawRectOutline(309, 4, 7, 5, g_theme.colBlack);
    RegisterHit(304, 0, 16, 12, HIT_TOP_TOGGLE);

    struct PItem { const char* lbl; int val; int pid; bool sep; bool twoDigits; };
    PItem pitems[18] = {
        {"VOLUME",  inst.volume,        0,  false, true},
        {"WAVELEN", inst.waveLength,    1,  true,  true},
        {"ATTACK",  inst.attackFrames,  2,  false, false},
        {"VOLUME",  inst.attackVol,     3,  false, true},
        {"DECAY",   inst.decayFrames,   4,  false, false},
        {"VOLUME",  inst.decayVol,      5,  false, true},
        {"SUSTAIN", inst.sustainFrames, 6,  false, false},
        {"RELEASE", inst.releaseFrames, 7,  false, false},
        {"VOLUME",  inst.releaseVol,    8,  true,  true},
        {"DELAY",   inst.vibratoDelay,  9,  false, false},
        {"DEPTH",   inst.vibratoDepth,  10, false, false},
        {"SPEED",   inst.vibratoSpeed,  11, true,  true},
        {"LOWER L.",inst.squareLower,   12, false, false},
        {"UPPER L.",inst.squareUpper,   13, false, false},
        {"S SPEED", inst.squareSpeed,   14, true,  false},
        {"LOWER L.",inst.filterLower,   15, false, false},
        {"UPPER L.",inst.filterUpper,   16, false, false},
        {"F SPEED", inst.filterSpeed,   17, false, false}
    };

    int ly = 13;
    for (int i = 0; i < 18; ++i) {
        DrawTextPT(2, ly + 2, pitems[i].lbl, g_theme.colWhite, true, 7);
        int boxX = pitems[i].twoDigits ? 58 : 50;
        int boxW = pitems[i].twoDigits ? 18 : 26;
        DrawBevel(boxX, ly, boxW, 12, true, g_theme.colBlack);
        std::string vStr = pitems[i].twoDigits ? Dec2(pitems[i].val) : Dec3(pitems[i].val);
        DrawTextPT(boxX + 1, ly + 2, vStr, g_theme.colWhite, false, 8);
        DrawSpinPair320(76, ly, 20, 12, HIT_INST_PARAM_INC, HIT_INST_PARAM_DEC, pitems[i].pid);

        ly += 12;
        if (pitems[i].sep) {
            g_fb.fillRect(0, ly + 1, 97, 1, g_theme.colGrayDark);
            g_fb.fillRect(0, ly + 2, 97, 1, g_theme.colWhite);
            ly += 4;
        }
    }

    g_fb.fillRect(97, 12, 1, NATIVE_H - 12, g_theme.colGrayDark);
    g_fb.fillRect(98, 12, 1, NATIVE_H - 12, g_theme.colWhite);

    DrawBevel(124, 16, 164, 12, false, g_theme.colGrayDark);
    DrawTextPT(142, 18, "PERFORMANCE EDITOR", g_theme.colWhite, true, 7);
    RegisterHit(124, 16, 164, 12, HIT_PERF_EDITOR_BTN);

    int mx = 101;
    int my = 39;

    DrawTextPT(mx, my + 2, "SPEED", g_theme.colWhite, true, 7);
    DrawBevel(mx + 36, my, 26, 12, true, g_theme.colBlack);
    DrawTextPT(mx + 37, my + 2, Dec3(g_engine.song.speedMultiplier), g_theme.colWhite, false, 8);
    DrawSpinPair320(mx + 62, my, 20, 12, HIT_INST_PARAM_INC, HIT_INST_PARAM_DEC, 18);
    my += 13;

    DrawTextPT(mx, my + 2, "LEN.", g_theme.colWhite, true, 7);
    DrawBevel(mx + 36, my, 26, 12, true, g_theme.colBlack);
    DrawTextPT(mx + 37, my + 2, Dec3(g_engine.song.length), g_theme.colWhite, false, 8);
    DrawSpinPair320(mx + 62, my, 20, 12, HIT_INST_PARAM_INC, HIT_INST_PARAM_DEC, 19);
    my += 13;

    DrawTextPT(mx, my + 2, "HARDCUT", g_theme.colWhite, true, 7);
    DrawBevel(mx + 52, my, 10, 12, true, g_theme.colBlack);
    DrawTextPT(mx + 53, my + 2, std::to_string(inst.hardcut), g_theme.colWhite, false, 8);
    DrawSpinPair320(mx + 62, my, 20, 12, HIT_INST_PARAM_INC, HIT_INST_PARAM_DEC, 20);
    my += 13;

    DrawTextPT(mx, my + 2, "RELEASE CUT", g_theme.colWhite, true, 6);
    DrawBevel(mx + 71, my, 11, 11, false, g_theme.colGrayMain);
    if (inst.releaseCut) DrawTextPT(mx + 73, my + 2, "V", g_theme.colWhite, true, 6);
    RegisterHit(mx + 71, my, 11, 11, HIT_RELEASE_CUT);
    my += 12;

    bool testCaps = IsTestModeActive();
    DrawTextPT(mx, my + 1, "TEST MODE", g_theme.colWhite, true, 6);
    DrawTextPT(mx, my + 8, "(CAPS LOCK)", g_theme.colWhite, true, 6);
    DrawBevel(mx + 71, my + 2, 11, 11, false, g_theme.colGrayMain);
    if (testCaps) DrawTextPT(mx + 73, my + 4, "V", g_theme.colWhite, true, 6);
    RegisterHit(mx + 71, my + 2, 11, 11, HIT_TEST_MODE);
    my += 17;

    DrawBevel(mx, my, 82, 11, false, g_theme.colGrayDark);
    DrawTextPT(mx + 12, my + 2, "WAVEFORMS", g_theme.colWhite, true, 7);
    my += 13;
    const char* wfNames[6] = {
        "0 HOLD WAVE",
        "1 TRIANGLE",
        "2 SAWTOOTH",
        "3 SQUARE",
        "4 WH.NOISE",
        "\x7F FIXED"
    };
    for (int i = 0; i < 6; ++i) {
        DrawTextPT(mx + 2, my, wfNames[i], g_theme.colWhite, true, 6);
        RegisterHit(mx, my, 82, 8, HIT_WAVE_ITEM, i);
        my += 8;
    }
    my += 2;

    DrawBevel(mx, my, 82, 11, false, g_theme.colGrayDark);
    DrawTextPT(mx + 15, my + 2, "COMMANDS", g_theme.colWhite, true, 7);
    my += 13;
    struct CmdItem { const char* txt; uint16_t val; };
    CmdItem cmds[8] = {
        {"0 INIT FILTER", 0x000},
        {"1 SLIDE UP",    0x104},
        {"2 SLIDE DOWN",  0x204},
        {"3 INIT SQUARE", 0x320},
        {"4 TOGGLE MOD.", 0x440},
        {"5 JUMP 2 STEP", 0x500},
        {"C VOLUME",      0xC40},
        {"F SPEED",       0xF04}
    };
    for (int i = 0; i < 8; ++i) {
        DrawTextPT(mx, my, cmds[i].txt, g_theme.colWhite, true, 6);
        RegisterHit(mx, my, 82, 8, HIT_CMD_ITEM, cmds[i].val);
        my += 8;
    }

    int tx = 186;
    DrawTextPT(tx, 31, "STEP NOTE   FX1 FX2", g_theme.colWhite, true, 7);
    g_fb.setPixel(tx + 72, 33, g_theme.colWhite);
    g_fb.setPixel(tx + 73, 32, g_theme.colWhite);
    g_fb.fillRect(tx + 73, 33, 1, 4, g_theme.colWhite);
    g_fb.setPixel(tx + 72, 37, g_theme.colWhite);

    for (int r = 0; r < 22; ++r) {
        int ry = 40 + r * 9 + 4;
        if (ry + 9 > NATIVE_H) break;
        bool isCurRow = (r == g_perfEditRow);

        DrawBevel(tx, ry, 25, 9, true, isCurRow ? g_theme.colGrayDark : g_theme.colGrayLight);
        DrawTextPT(tx + 1, ry + 1, Dec3(r), g_theme.colWhite, false, 8);

        DrawBevel(tx + 25, ry, 34, 9, true, g_theme.colGrayLight);
        const PerfStep& ps = inst.perfList[r];
        std::string nStr = FormatNote(ps.note) + (ps.fixedNote ? "\x7F" : "-");
        DrawTextPT(tx + 26, ry + 1, nStr, g_theme.colWhite, false, 8);

        DrawBevel(tx + 59, ry, 11, 9, true, g_theme.colGrayLight);
        DrawTextPT(tx + 61, ry + 1, std::to_string(ps.waveform), g_theme.colWhite, false, 8);

        DrawBevel(tx + 70, ry, 26, 9, true, g_theme.colGrayLight);
        char fx1Str[8], fx2Str[8];
        snprintf(fx1Str, sizeof(fx1Str), "%01X%02d", (ps.fx1 >> 8) & 0xF, std::clamp<int>(ps.fx1 & 0xFF, 0, 64));
        snprintf(fx2Str, sizeof(fx2Str), "%01X%02d", (ps.fx2 >> 8) & 0xF, std::clamp<int>(ps.fx2 & 0xFF, 0, 64));
        DrawTextPT(tx + 71, ry + 1, fx1Str, g_theme.colWhite, false, 8);

        DrawBevel(tx + 96, ry, 26, 9, true, g_theme.colGrayLight);
        DrawTextPT(tx + 97, ry + 1, fx2Str, g_theme.colWhite, false, 8);

        RegisterHit(tx + 25, ry, 25, 9, HIT_PERF_CELL, (r << 4) | 0);
        RegisterHit(tx + 50, ry, 9,  9, HIT_PERF_CELL, (r << 4) | 1);
        RegisterHit(tx + 59, ry, 11, 9, HIT_PERF_CELL, (r << 4) | 2);
        RegisterHit(tx + 70, ry, 26, 9, HIT_PERF_CELL, (r << 4) | 3);
        RegisterHit(tx + 96, ry, 26, 9, HIT_PERF_CELL, (r << 4) | 6);

        if (isCurRow) {
            int cx = tx + 25, cw = 25;
            if (g_perfEditCol == 1)      { cx = tx + 50; cw = 9; }
            else if (g_perfEditCol == 2) { cx = tx + 59; cw = 11; }
            else if (g_perfEditCol >= 3 && g_perfEditCol <= 5) { cx = tx + 70 + (g_perfEditCol - 3) * 8; cw = 9; }
            else if (g_perfEditCol >= 6 && g_perfEditCol <= 8) { cx = tx + 96 + (g_perfEditCol - 6) * 8; cw = 9; }
            g_fb.drawRectOutline(cx, ry, cw, 9, g_theme.colTealPerf);
        }
    }
}

// ============================================================================
// SCREEN 3: PREFS REQUESTER (FLOPPY DISK DIRECTORY CONFIGURATION)
// ============================================================================
void RenderPrefsScreen320() {
    g_fb.fillRect(0, 0, NATIVE_W, NATIVE_H, g_theme.colBlack);
    DrawBevel(0, 0, NATIVE_W, NATIVE_H, false, g_theme.colBlack);

    DrawBevel(1, 1, NATIVE_W - 2, 13, false, g_theme.colGrayMain);
    DrawBevel(2, 2, 13, 11, false, g_theme.colGrayMain);
    g_fb.drawRectOutline(7, 5, 3, 5, g_theme.colBlack);
    RegisterHit(2, 2, 13, 11, HIT_MAIN_BTN);

    DrawTextPT(170, 4, "FLOPPY / PREFS", g_theme.colWhite, true, 8);
    DrawBevel(NATIVE_W - 22, 2, 20, 11, false, g_theme.colGrayMain);
    g_fb.fillRect(NATIVE_W - 19, 4, 8, 5, g_theme.colBlack);
    g_fb.fillRect(NATIVE_W - 16, 6, 10, 6, g_theme.colWhite);
    g_fb.drawRectOutline(NATIVE_W - 16, 6, 10, 6, g_theme.colBlack);
    RegisterHit(NATIVE_W - 22, 2, 20, 11, HIT_MAIN_BTN);

    DrawTextPT(10, 22, "Songs", g_theme.colGrayLight, false, 8);
    DrawBevel(76, 18, 204, 16, false, g_theme.colBlack);
    DrawBevel(78, 20, 200, 12, true, g_theme.colBlack);
    DrawTextPT(82, 22, fs::path(g_prefs.songsDir).string(), g_theme.colGrayLight, false, 7);
    RegisterHit(76, 18, 204, 16, HIT_PREFS_SONGS_DIR);
    DrawButton320(284, 18, 26, 16, "DIR", HIT_PREFS_SONGS_DIR);

    DrawTextPT(10, 40, "Instr", g_theme.colGrayLight, false, 8);
    DrawBevel(76, 36, 204, 16, false, g_theme.colBlack);
    DrawBevel(78, 38, 200, 12, true, g_theme.colBlack);
    DrawTextPT(82, 40, fs::path(g_prefs.instrDir).string(), g_theme.colGrayLight, false, 7);
    RegisterHit(76, 36, 204, 16, HIT_PREFS_INSTR_DIR);
    DrawButton320(284, 36, 26, 16, "DIR", HIT_PREFS_INSTR_DIR);

    struct Chk { const char* lbl; bool val; int id; };
    Chk chks[4] = {
        {"Explicit VBL Startup",    g_prefs.explicitVblStartup,     HIT_PREFS_CHK_VBL},
        {"Allocate Audio-Channels", g_prefs.allocateAudioChannels,  HIT_PREFS_CHK_ALLOC},
        {"pt.font in Requesters",   g_prefs.ptFontInRequesters,     HIT_PREFS_CHK_FONT},
        {"Transparent VolumeMeter", g_prefs.transparentVolumeMeter, HIT_PREFS_CHK_TRANS}
    };
    for (int i = 0; i < 4; ++i) {
        int cy = 56 + i * 16;
        DrawTextPT(42, cy + 4, chks[i].lbl, g_theme.colGrayLight, false, 8);
        DrawBevel(276, cy, 34, 15, false, g_theme.colBlack);
        if (chks[i].val) {
            g_fb.setPixel(287, cy + 8, g_theme.colGrayLight);
            g_fb.setPixel(288, cy + 9, g_theme.colGrayLight);
            g_fb.setPixel(289, cy + 10, g_theme.colGrayLight);
            g_fb.setPixel(290, cy + 11, g_theme.colGrayLight);
            for (int k = 0; k < 7; ++k)
                g_fb.setPixel(291 + k, cy + 10 - k, g_theme.colGrayLight);
        }
        RegisterHit(276, cy, 34, 15, chks[i].id);
    }

    const char* vmStr[3]  = {"Flashing", "Solid", "Off"};
    const char* curStr[2] = {"Flashing", "Solid"};
    const char* rasStr[2] = {"<none>", "Copper Bar"};
    struct Cyc { const char* lbl; const char* val; int id; };
    Cyc cycs[3] = {
        {"Volume Meter", vmStr[g_prefs.volumeMeterMode % 3], HIT_PREFS_CYC_VMETER},
        {"Cursor",       curStr[g_prefs.cursorMode % 2],     HIT_PREFS_CYC_CURSOR},
        {"Rastertime",   rasStr[g_prefs.rastertimeMode % 2], HIT_PREFS_CYC_RASTER}
    };
    for (int i = 0; i < 3; ++i) {
        int cy = 122 + i * 18;
        DrawTextPT(48, cy + 4, cycs[i].lbl, g_theme.colGrayLight, false, 8);
        DrawBevel(154, cy, 156, 16, false, g_theme.colBlack);
        g_fb.drawRectOutline(164, cy + 4, 6, 8, g_theme.colGrayLight);
        g_fb.fillRect(169, cy + 6, 2, 3, g_theme.colBlack);
        g_fb.setPixel(168, cy + 8, g_theme.colGrayLight);
        g_fb.setPixel(170, cy + 8, g_theme.colGrayLight);
        g_fb.fillRect(175, cy + 3, 1, 10, g_theme.colGrayLight);

        DrawTextPT(184, cy + 4, cycs[i].val, g_theme.colGrayLight, false, 8);
        RegisterHit(154, cy, 156, 16, cycs[i].id);
    }

    int by = 228;
    DrawBevel(10, by, 96, 16, false, g_theme.colBlack);
    DrawTextPT(42, by + 4, "Save", g_theme.colGrayLight, false, 8);
    RegisterHit(10, by, 96, 16, HIT_PREFS_SAVE);

    DrawBevel(112, by, 96, 16, false, g_theme.colBlack);
    if (!g_pinkExternPrefsExecuted) {
        for (int py = by + 2; py < by + 14; ++py)
            for (int px = 114 + (py & 1); px < 206; px += 2)
                g_fb.setPixel(px, py, g_theme.colGrayLight);
    }
    DrawTextPT(132, by + 4, "Current", g_theme.colGrayLight, true, 8);
    RegisterHit(112, by, 96, 16, HIT_PREFS_CURRENT);

    DrawBevel(214, by, 96, 16, false, g_theme.colBlack);
    DrawTextPT(238, by + 4, "Cancel", g_theme.colGrayLight, false, 8);
    RegisterHit(214, by, 96, 16, HIT_PREFS_CANCEL);
}

// ============================================================================
// SCREEN 4: ABYSS / DEXTER MENU
// ============================================================================
COLORREF* GetThemeSlotPtr(int slot) {
    switch (slot) {
        case 0: return &g_theme.colGrayMain;
        case 1: return &g_theme.colGrayLight;
        case 2: return &g_theme.colGrayDark;
        case 3: return &g_theme.colBlack;
        case 4: return &g_theme.colWhite;
        case 5: return &g_theme.colBlueMeter;
        case 6: return &g_theme.colTealPerf;
        default: return &g_theme.colGrayMain;
    }
}

void RenderThemeAndFilterScreen320() {
    g_fb.fillRect(0, 0, NATIVE_W, NATIVE_H, g_theme.colGrayMain);
    DrawBevel(0, 0, NATIVE_W, NATIVE_H, false, g_theme.colGrayMain);

    DrawButton320(2, 2, 44, 12, "MAIN", HIT_MAIN_BTN);
    DrawTextPT(56, 4, "ABYSS / DEXTER - THEME & FILTER", g_theme.colWhite, true, 7);

    DrawBevel(6, 18, 308, 30, true, g_theme.colGrayDark);
    DrawTextPT(12, 22, "AMIGA HARDWARE FILTER:", g_theme.colWhite, true, 7);
    const char* fltNames[4] = {"OFF (PURE)", "AMIGA 500 (4.5K)", "AMIGA 1200", "LED LPF 3.3K"};
    DrawButton320(168, 20, 140, 13, fltNames[g_engine.song.filterModel % 4], HIT_FILTER_CYCLE);
    DrawTextPT(12, 37, "AUDIO OUTPUT: CENTER MONO-BALANCED", g_theme.colWhite, true, 7);

    DrawBevel(6, 52, 308, 174, true, g_theme.colBlack);
    DrawTextPT(12, 56, "CUSTOM THEME EDITOR (AUTO-SAVED):", g_theme.colWhite, false, 7);

    const char* slotNames[7] = {
        "1. MAIN PANEL", "2. LIGHT FIELD", "3. DARK BEVEL",
        "4. BACKGROUND", "5. TEXT / LINE", "6. METER / CUR", "7. PERF CURSOR"
    };

    for (int i = 0; i < 7; ++i) {
        int sy = 70 + i * 15;
        bool sel = (i == g_themeSlot);
        if (sel) g_fb.fillRect(10, sy - 1, 165, 13, g_theme.colGrayDark);
        DrawTextPT(14, sy + 1, slotNames[i], g_theme.colWhite, false, 7);
        COLORREF c = *GetThemeSlotPtr(i);
        DrawBevel(135, sy, 34, 11, false, c);
        RegisterHit(10, sy - 1, 165, 13, HIT_THEME_SLOT, i);
    }

    COLORREF curCol = *GetThemeSlotPtr(g_themeSlot);
    int rgb[3] = { GetRValue(curCol), GetGValue(curCol), GetBValue(curCol) };
    const char* rgbLbl[3] = { "R", "G", "B" };
    for (int k = 0; k < 3; ++k) {
        int ry = 82 + k * 28;
        DrawTextPT(184, ry + 2, rgbLbl[k], g_theme.colWhite, false, 8);
        DrawBevel(196, ry, 30, 12, true, g_theme.colGrayDark);
        DrawTextPT(198, ry + 2, Dec3(rgb[k]), g_theme.colWhite, false, 8);
        DrawSpinPair320(228, ry, 24, 12, HIT_THEME_RGB_INC, HIT_THEME_RGB_DEC, k);
    }

    DrawButton320(184, 175, 120, 14, "RESET AMIGA", HIT_THEME_RESET);
    DrawButton320(184, 195, 120, 14, "SAVE & APPLY", HIT_THEME_SAVE);
    DrawButton320(6, 232, 308, 18, "SAVE THEME & RETURN TO TRACKER", HIT_THEME_SAVE);
}

// ============================================================================
// SCREEN 5: DINK. MENU (SMALL COMPACT QUICK GUIDE FONT)
// ============================================================================
void RenderAudioAndHelpScreen320() {
    g_fb.fillRect(0, 0, NATIVE_W, NATIVE_H, g_theme.colGrayMain);
    DrawBevel(0, 0, NATIVE_W, NATIVE_H, false, g_theme.colGrayMain);

    DrawButton320(2, 2, 44, 12, "MAIN", HIT_MAIN_BTN);
    DrawButton320(48, 2, 38, 12, "INS", HIT_INS_BTN);
    DrawTextPT(92, 4, "DINK. AUDIO & QUICK GUIDE", g_theme.colWhite, true, 7);

    DrawTextPT(6, 18, "BUFFER:", g_theme.colWhite, true, 7);
    for (int i = 0; i < 6; ++i) {
        int bx = 58 + i * 43;
        bool active = (i == g_bufferIdx);
        DrawBevel(bx, 16, 41, 13, active, active ? g_theme.colBlueMeter : g_theme.colGrayMain);
        DrawTextPT(bx + 3, 19, std::to_string(kBufferSizes[i]), g_theme.colWhite, true, 7);
        RegisterHit(bx, 16, 41, 13, HIT_AUDIO_BUF_SET, i);
    }

    DrawTextPT(6, 34, "S.RATE:", g_theme.colWhite, true, 7);
    for (int i = 0; i < 4; ++i) {
        int sx = 58 + i * 64;
        bool active = (i == g_sampleRateIdx);
        DrawBevel(sx, 32, 62, 13, active, active ? g_theme.colBlueMeter : g_theme.colGrayMain);
        DrawTextPT(sx + 8, 35, std::to_string(kSampleRates[i]), g_theme.colWhite, true, 8);
        RegisterHit(sx, 32, 62, 13, HIT_AUDIO_SR_SET, i);
    }

    DrawBevel(6, 49, 308, 202, true, g_theme.colBlack);
    const char* helpLines[17] = {
        "AHX TRACKER QUICK GUIDE (ENGLISH):",
        "- [SPACE]: Play/Stop (Resets row to 00, keeps channel patterns).",
        "- [INS]: Toggle REC Mode (Records each note played and steps forward).",
        "- Z-M / Q-U: 2-Octave Keyboard Note Input (Auditions Sample Editor too).",
        "- CHANNELS: Fully independent per track (never cloned across channels).",
        "- SHIFT+Arrows: Select Rows. SHIFT+C/V: Copy/Paste cells.",
        "- DEL: Delete selection or row.",
        "- POS (000..099): Manual Pattern selector per channel.",
        "- LEN (001..064): Builds Song Sequences (00..63) independently.",
        "- F1: Main Tracker | F2: Instrument | F3: Floppy / Prefs.",
        "- F5: Optimize Song | F6: Amiga Cursor | F11: Fullscreen.",
        "- F7: Save Project (.IF) | F8: Export Audio (.WAV/.AIFF).",
        "- FX1/FX2 & Commands (00..64 Range: 00=OFF, 64=100% ON):",
        "  0=Filter, 1=SlideUp, 2=SlideDn, 3=Square, 4=Mod, 5=Step, 8=Pan,",
        "  9=Offset, A=VolSlide, B=SeqJump, C=VOL(00..64), D=Break, F=Speed.",
        "- Samples: Exclusively loads .WAV, .AIFF, .MP3 at max quality (4..32-bit).",
        "- Modules: Exclusively loads .MOD, .XM, .IF (POS/LEN/RES/TRL/SS/SSN/SSP)."
    };

    for (int i = 0; i < 17; ++i) {
        COLORREF col = (i == 0) ? g_theme.colBlueMeter : g_theme.colWhite;
        DrawTextMicro(10, 53 + i * 11, helpLines[i], col);
    }
}

// ============================================================================
// SCREEN 6: SAMPLE EDITOR (LIVE KEYBOARD AUDITION + 4..32-BIT PRECISION)
// ============================================================================
void RenderSampleEditorScreen320() {
    g_fb.fillRect(0, 0, NATIVE_W, NATIVE_H, g_theme.colGrayMain);
    DrawBevel(0, 0, NATIVE_W, NATIVE_H, false, g_theme.colGrayMain);

    Instrument& inst = g_engine.song.instruments[g_currentInst];

    DrawButton320(2, 2, 38, 12, "BACK", HIT_INS_BTN);
    DrawButton320(42, 2, 38, 12, "LOAD", HIT_LOAD_INS);
    DrawButton320(82, 2, 42, 12, "TEST", HIT_SMP_PLAY_TEST);
    DrawTextPT(130, 4, inst.name, g_theme.colWhite, true, 7);

    DrawButton320(266, 2, 25, 12, "Z-", HIT_SMP_ZOOM_OUT);
    DrawButton320(293, 2, 25, 12, "Z+", HIT_SMP_ZOOM_IN);

    int wx = 6, wy = 16, ww = 308, wh = 104;
    DrawBevel(wx, wy, ww, wh, true, g_theme.colBlack);
    g_fb.fillRect(wx + 1, wy + wh / 2, ww - 2, 1, g_theme.colGrayDark);

    if (inst.hasSample && !inst.sample.floatData.empty()) {
        size_t total = inst.sample.floatData.size();
        size_t viewLen = std::max<size_t>(16, size_t(double(total) / double(g_smpZoom)));
        size_t viewStart = std::min<size_t>(total - viewLen, size_t(double(g_smpViewOffset) * double(total - viewLen)));
        size_t viewEnd = std::min(total, viewStart + viewLen);

        int s0 = int(std::min(g_smpSelStart, g_smpSelEnd) * (ww - 2));
        int s1 = int(std::max(g_smpSelStart, g_smpSelEnd) * (ww - 2));
        if (s1 > s0) {
            g_fb.fillRect(wx + 1 + s0, wy + 1, s1 - s0, wh - 2, g_theme.colGrayDark);
        }

        if (inst.sample.loopEnabled && inst.sample.loopEnd > inst.sample.loopStart) {
            if (inst.sample.loopStart >= viewStart && inst.sample.loopStart <= viewEnd) {
                int lx0 = wx + 1 + int((double(inst.sample.loopStart - viewStart) / double(viewLen)) * (ww - 2));
                g_fb.fillRect(lx0, wy + 1, 1, wh - 2, g_theme.colTealPerf);
            }
            if (inst.sample.loopEnd >= viewStart && inst.sample.loopEnd <= viewEnd) {
                int lx1 = wx + 1 + int((double(inst.sample.loopEnd - viewStart) / double(viewLen)) * (ww - 2));
                g_fb.fillRect(lx1, wy + 1, 1, wh - 2, g_theme.colTealPerf);
            }
        }

        int midY = wy + wh / 2;
        int halfH = (wh / 2) - 4;
        for (int px = 0; px < ww - 2; ++px) {
            size_t i0 = viewStart + (size_t(px) * viewLen) / size_t(ww - 2);
            size_t i1 = std::min(viewEnd, viewStart + ((size_t(px + 1) * viewLen) / size_t(ww - 2)) + 1);
            if (i0 >= total) break;
            float mn = 0.0f, mx = 0.0f;
            for (size_t k = i0; k < i1; ++k) {
                float v = inst.sample.floatData[k];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            int yTop = midY - int(mx * halfH);
            int yBot = midY - int(mn * halfH);
            if (yBot <= yTop) yBot = yTop + 1;
            bool inSel = (s1 > s0 && px >= s0 && px <= s1);
            g_fb.fillRect(wx + 1 + px, yTop, 1, yBot - yTop, inSel ? g_theme.colWhite : g_theme.colBlueMeter);
        }
    } else {
        DrawTextPT(52, wy + 48, "LOAD .WAV / .AIFF / .MP3 SAMPLE", g_theme.colGrayLight, false, 7);
    }
    RegisterHit(wx, wy, ww, wh, HIT_SMP_WAVE_AREA);

    int zBarY = 122;
    DrawBevel(6, zBarY, 308, 10, true, g_theme.colBlack);
    int thumbW = std::max(16, int(306.0f / g_smpZoom));
    int thumbX = 7 + int(g_smpViewOffset * (306 - thumbW));
    g_fb.fillRect(thumbX, zBarY + 1, thumbW, 8, g_theme.colBlueMeter);
    char zTxt[56];
    snprintf(zTxt, sizeof(zTxt), "ZOOM:%.1fx %d-BIT %uHZ LEN:%zu",
             g_smpZoom, inst.sample.bitDepth, inst.sample.sampleRate, inst.sample.floatData.size());
    DrawTextPT(10, zBarY + 1, zTxt, g_theme.colWhite, false, 6);

    int r1y = 136;
    DrawButton320(6,   r1y, 48, 16, "NORM",  HIT_SMP_NORM);
    DrawButton320(56,  r1y, 48, 16, "REV",   HIT_SMP_REV);
    DrawButton320(106, r1y, 52, 16, inst.sample.loopEnabled ? "LOOP:ON" : "LOOP", HIT_SMP_LOOP);
    DrawButton320(160, r1y, 48, 16, "F.IN",  HIT_SMP_FIN);
    DrawButton320(210, r1y, 50, 16, "F.OUT", HIT_SMP_FOUT);
    DrawButton320(262, r1y, 52, 16, "DEL",   HIT_SMP_DEL);

    int r2y = 158;
    DrawTextPT(8, r2y + 4, "AMP:", g_theme.colWhite, true, 8);
    DrawBevel(44, r2y, 46, 16, true, g_theme.colBlack);
    DrawTextPT(47, r2y + 4, std::to_string(int(inst.sample.ampPercent)) + "%", g_theme.colWhite, false, 8);
    DrawSpinPair320(92, r2y, 26, 16, HIT_SMP_AMP_INC, HIT_SMP_AMP_DEC);

    DrawTextPT(134, r2y + 4, "PAN:", g_theme.colWhite, true, 8);
    DrawBevel(170, r2y, 68, 16, true, g_theme.colBlack);
    int panPct = int(std::round(inst.sample.pan * 100.0f));
    std::string panStr = (panPct == 0) ? "CENTER" : ((panPct < 0) ? ("L " + std::to_string(-panPct)) : ("R " + std::to_string(panPct)));
    DrawTextPT(174, r2y + 4, panStr, g_theme.colWhite, false, 8);
    DrawSpinPair320(240, r2y, 26, 16, HIT_SMP_PAN_INC, HIT_SMP_PAN_DEC);

    int r3y = 180;
    DrawTextPT(8, r3y + 4, "TUN:", g_theme.colWhite, true, 8);
    DrawBevel(44, r3y, 46, 16, true, g_theme.colBlack);
    char tunBuf[12];
    snprintf(tunBuf, sizeof(tunBuf), "%+03d", inst.sample.tuneSemitones);
    DrawTextPT(48, r3y + 4, tunBuf, g_theme.colWhite, false, 8);
    DrawSpinPair320(92, r3y, 26, 16, HIT_SMP_TUN_INC, HIT_SMP_TUN_DEC);

    DrawTextPT(134, r3y + 4, "REL:", g_theme.colWhite, true, 8);
    DrawBevel(170, r3y, 68, 16, true, g_theme.colBlack);
    DrawTextPT(186, r3y + 4, FormatNote(inst.sample.relNote + 1), g_theme.colWhite, false, 8);
    DrawSpinPair320(240, r3y, 26, 16, HIT_SMP_REL_INC, HIT_SMP_REL_DEC);

    DrawBevel(6, 204, 308, 44, true, g_theme.colBlack);
    DrawTextPT(10, 210, "KEYS Z-M / Q-U: PLAY SAMPLE NOTES LIVE", g_theme.colWhite, false, 6);
    DrawTextPT(10, 222, "SHIFT+C/V: COPY/PASTE | DEL: CUT SELECTION", g_theme.colGrayLight, false, 6);
    DrawTextPT(10, 234, "FORMATS: .WAV / .AIFF / .MP3 (4..32-BIT NATIVE)", g_theme.colGrayLight, false, 6);
}

// ============================================================================
// SCREEN 7: AMIGA FLOPPY DISK EMULATION FILE BROWSER
// ============================================================================
void RenderFloppyBrowserScreen320() {
    g_fb.fillRect(0, 0, NATIVE_W, NATIVE_H, g_theme.colGrayMain);
    DrawBevel(0, 0, NATIVE_W, NATIVE_H, false, g_theme.colGrayMain);

    DrawButton320(2, 2, 42, 12, "BACK", (g_floppyMode == FLOPPY_SONGS) ? HIT_MAIN_BTN : HIT_INS_BTN);
    DrawTextPT(48, 4, (g_floppyMode == FLOPPY_SONGS) ? "DF0: MOD/XM/IF DISK" : "DF1: WAV/AIFF/MP3", g_theme.colWhite, true, 7);
    DrawButton320(216, 2, 48, 12, "PARENT", HIT_FLOPPY_PARENT);
    DrawButton320(266, 2, 52, 12, "WIN DLG", HIT_FLOPPY_SYSTEM_DLG);

    DrawBevel(6, 16, 308, 12, true, g_theme.colBlack);
    std::string dirStr = fs::path(g_floppyCurrentDir).string();
    if (dirStr.size() > 54) dirStr = "..." + dirStr.substr(dirStr.size() - 51);
    DrawTextMicro(10, 19, dirStr, g_theme.colGrayLight);

    DrawBevel(6, 30, 308, 200, true, g_theme.colBlack);
    int visibleCount = 19;
    if (g_floppyItems.empty()) {
        DrawTextPT(42, 118, "NO COMPATIBLE FILES IN DISK", g_theme.colGrayLight, false, 8);
    } else {
        for (int i = 0; i < visibleCount; ++i) {
            int idx = g_floppyScroll + i;
            if (idx >= int(g_floppyItems.size())) break;
            int ry = 33 + i * 10;
            bool sel = (idx == g_floppySelected);
            if (sel) {
                g_fb.fillRect(8, ry - 1, 304, 10, g_theme.colBlueMeter);
            }
            COLORREF col = g_floppyItems[idx].isDir ? g_theme.colTealPerf : g_theme.colWhite;
            if (sel) col = g_theme.colWhite;
            std::string name = g_floppyItems[idx].displayName;
            if (name.size() > 56) name.resize(56);
            DrawTextMicro(12, ry + 1, name, col);
            RegisterHit(8, ry - 1, 304, 10, HIT_FLOPPY_ITEM, idx);
        }
    }

    DrawButton320(6, 234, 150, 16, "LOAD SELECTED FILE", HIT_FLOPPY_LOAD_BTN);
    DrawButton320(164, 234, 150, 16, "CHANGE FOLDER (PREFS)", (g_floppyMode == FLOPPY_SONGS) ? HIT_PREFS_SONGS_DIR : HIT_PREFS_INSTR_DIR);
}

void GetSampleSelectionIndices(const Instrument& inst, size_t& outI0, size_t& outI1, bool& hasRange) {
    size_t total = inst.sample.floatData.size();
    if (total == 0) { outI0 = 0; outI1 = 0; hasRange = false; return; }
    size_t viewLen = std::max<size_t>(16, size_t(double(total) / double(g_smpZoom)));
    size_t viewStart = std::min<size_t>(total - viewLen, size_t(double(g_smpViewOffset) * double(total - viewLen)));

    float sMin = std::min(g_smpSelStart, g_smpSelEnd);
    float sMax = std::max(g_smpSelStart, g_smpSelEnd);
    if ((sMax - sMin) > 0.01f) {
        hasRange = true;
        outI0 = std::min(total, viewStart + size_t(sMin * viewLen));
        outI1 = std::min(total, viewStart + size_t(sMax * viewLen));
        if (outI1 <= outI0) hasRange = false;
    } else {
        hasRange = false;
        outI0 = 0;
        outI1 = total;
    }
}

void ApplySampleDSP(int op) {
    std::lock_guard<std::mutex> lock(g_engine.engineMutex);
    Instrument& inst = g_engine.song.instruments[g_currentInst];
    if (!inst.hasSample || inst.sample.floatData.empty()) return;

    g_sampleUndoBuffer = inst.sample.floatData;
    g_sampleRedoBuffer.clear();

    size_t i0 = 0, i1 = 0;
    bool hasRange = false;
    GetSampleSelectionIndices(inst, i0, i1, hasRange);

    if (op == 0) {
        float peak = 0.0f;
        for (size_t i = i0; i < i1; ++i) peak = std::max(peak, std::fabs(inst.sample.floatData[i]));
        if (peak > 0.0001f) {
            float gain = 0.98f / peak;
            for (size_t i = i0; i < i1; ++i) inst.sample.floatData[i] = std::clamp(inst.sample.floatData[i] * gain, -1.0f, 1.0f);
        }
    } else if (op == 1) {
        std::reverse(inst.sample.floatData.begin() + i0, inst.sample.floatData.begin() + i1);
    } else if (op == 2) {
        inst.sample.loopEnabled = !inst.sample.loopEnabled;
        if (inst.sample.loopEnabled) {
            inst.sample.loopStart = i0;
            inst.sample.loopEnd = i1;
            g_smpSelStart = std::max(g_smpSelStart, g_smpSelEnd);
            g_smpSelEnd = g_smpSelStart;
        } else {
            inst.sample.loopStart = 0;
            inst.sample.loopEnd = inst.sample.floatData.size();
            g_smpSelStart = 0.0f;
            g_smpSelEnd = 0.0f;
        }
    } else if (op == 3) {
        size_t len = i1 - i0;
        for (size_t i = 0; i < len; ++i) {
            float g = float(i) / float(std::max<size_t>(1, len - 1));
            inst.sample.floatData[i0 + i] *= g;
        }
    } else if (op == 4) {
        size_t len = i1 - i0;
        for (size_t i = 0; i < len; ++i) {
            float g = 1.0f - (float(i) / float(std::max<size_t>(1, len - 1)));
            inst.sample.floatData[i0 + i] *= g;
        }
    } else if (op == 5) {
        if (hasRange && i1 > i0) {
            inst.sample.floatData.erase(inst.sample.floatData.begin() + i0, inst.sample.floatData.begin() + i1);
            if (inst.sample.floatData.empty()) {
                inst.hasSample = false;
                inst.sample.loopEnabled = false;
            } else {
                inst.sample.loopEnd = std::min(inst.sample.loopEnd, inst.sample.floatData.size());
                if (inst.sample.loopStart >= inst.sample.loopEnd) inst.sample.loopStart = 0;
            }
            float rightAnchor = std::min(g_smpSelStart, g_smpSelEnd);
            g_smpSelStart = rightAnchor;
            g_smpSelEnd = rightAnchor;
        } else {
            inst.sample.floatData.clear();
            inst.hasSample = false;
            inst.sample.loopEnabled = false;
            g_smpSelStart = 0.0f;
            g_smpSelEnd = 0.0f;
        }
    }
}

// ============================================================================
// NATIVE DIALOGS WITH FLOPPY DISK FOLDER BROWSING
// ============================================================================
std::wstring BrowseAmigaFolder(HWND hwnd, const wchar_t* title) {
    g_inNativeDialog = true;
    ApplyActiveCursor();
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    std::wstring res;
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) res = path;
        CoTaskMemFree(pidl);
    }
    g_inNativeDialog = false;
    ApplyActiveCursor();
    return res;
}

void ActionSaveProject(HWND hwnd) {
    g_inNativeDialog = true;
    ApplyActiveCursor();
    wchar_t file[MAX_PATH] = L"song.if";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"AHX Native Module (*.if)\0*.if\0All Files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = g_prefs.songsDir != L"&:" ? g_prefs.songsDir.c_str() : nullptr;
    ofn.lpstrDefExt = L"if";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameW(&ofn)) {
        if (g_engine.saveProjectModFormat(file)) g_statusMsg = "Module Saved.";
    }
    g_inNativeDialog = false;
    ApplyActiveCursor();
}

void ActionExportAudio(HWND hwnd) {
    g_inNativeDialog = true;
    ApplyActiveCursor();
    wchar_t file[MAX_PATH] = L"export.wav";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Audio Files (*.wav;*.aiff)\0*.wav;*.aiff\0All Files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = g_prefs.songsDir != L"&:" ? g_prefs.songsDir.c_str() : nullptr;
    ofn.lpstrDefExt = L"wav";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameW(&ofn)) {
        int sr = kSampleRates[g_sampleRateIdx];
        if (g_engine.exportAudioFile(file, sr)) g_statusMsg = "Audio Exported.";
    }
    g_inNativeDialog = false;
    ApplyActiveCursor();
}

void ActionLoadProjectOrModuleDialog(HWND hwnd) {
    g_inNativeDialog = true;
    ApplyActiveCursor();
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Tracker Modules (*.mod;*.xm;*.if)\0*.mod;*.xm;*.if\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = g_prefs.songsDir != L"&:" ? g_prefs.songsDir.c_str() : nullptr;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        if (g_engine.loadProjectModFormat(file)) {
            g_editRow = 0;
            g_statusMsg = "Module Loaded.";
            g_screen = SCREEN_MAIN;
        } else {
            g_statusMsg = "Invalid Module.";
        }
    }
    g_inNativeDialog = false;
    ApplyActiveCursor();
}

void ActionLoadSampleDialog(HWND hwnd) {
    g_inNativeDialog = true;
    ApplyActiveCursor();
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Audio Samples (*.wav;*.aiff;*.mp3)\0*.wav;*.aiff;*.mp3\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = g_prefs.instrDir != L"/    Instruments" ? g_prefs.instrDir.c_str() : nullptr;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        if (g_engine.loadSampleFile(g_currentInst, file)) {
            g_smpSelStart = 0.0f;
            g_smpSelEnd = 0.0f;
            g_statusMsg = "Sample Loaded.";
        } else {
            g_statusMsg = "Only WAV/AIFF/MP3.";
        }
    }
    g_inNativeDialog = false;
    ApplyActiveCursor();
}

void ExecuteFloppySelection(HWND hwnd) {
    if (g_floppySelected < 0 || g_floppySelected >= int(g_floppyItems.size())) return;
    const FloppyItem& item = g_floppyItems[g_floppySelected];
    if (item.isDir) {
        RefreshFloppyDirectory(item.fullPath);
        return;
    }
    if (g_floppyMode == FLOPPY_SONGS) {
        if (g_engine.loadProjectModFormat(item.fullPath)) {
            g_editRow = 0;
            g_statusMsg = "Module Loaded.";
            g_screen = SCREEN_MAIN;
        }
    } else {
        if (g_engine.loadSampleFile(g_currentInst, item.fullPath)) {
            g_smpSelStart = 0.0f;
            g_smpSelEnd = 0.0f;
            g_statusMsg = "Sample Loaded.";
            g_screen = SCREEN_INSTRUMENT;
        }
    }
    (void)hwnd;
}

// ============================================================================
// SPIN CONTROL EXECUTION (SINGLE CLICK = +/-1, HOLD = ACCELERATED)
// ============================================================================
void ExecuteSpinAction(int id, int param, int stepVal) {
    Instrument& inst = g_engine.song.instruments[g_currentInst];
    switch (id) {
        case HIT_POS_INC: {
            std::lock_guard<std::mutex> lock(g_engine.engineMutex);
            uint16_t maxSeq = (g_engine.song.length > 0) ? std::min<uint16_t>(99, g_engine.song.length - 1) : 0;
            uint16_t nextS = std::min<uint16_t>(maxSeq, g_engine.playSeq.load() + stepVal);
            g_engine.playSeq = nextS;
            break;
        }
        case HIT_POS_DEC: {
            std::lock_guard<std::mutex> lock(g_engine.engineMutex);
            uint16_t cur = g_engine.playSeq.load();
            g_engine.playSeq = (cur >= stepVal) ? (cur - stepVal) : 0;
            break;
        }
        case HIT_LEN_INC: {
            std::lock_guard<std::mutex> lock(g_engine.engineMutex);
            g_engine.setSequenceLength(std::min<uint16_t>(64, g_engine.song.length + stepVal));
            break;
        }
        case HIT_LEN_DEC: {
            std::lock_guard<std::mutex> lock(g_engine.engineMutex);
            uint16_t nextLen = (g_engine.song.length > stepVal) ? (g_engine.song.length - stepVal) : 1;
            g_engine.setSequenceLength(nextLen);
            break;
        }
        case HIT_RES_INC:
            if (g_engine.song.restart + stepVal < g_engine.song.length) g_engine.song.restart += stepVal;
            else if (g_engine.song.length > 0) g_engine.song.restart = g_engine.song.length - 1;
            break;
        case HIT_RES_DEC:
            g_engine.song.restart = (g_engine.song.restart >= stepVal) ? (g_engine.song.restart - stepVal) : 0;
            break;
        case HIT_TRL_INC:
            g_engine.song.trackLength = std::min<uint8_t>(MAX_TRACK_LEN, g_engine.song.trackLength + stepVal);
            break;
        case HIT_TRL_DEC:
            g_engine.song.trackLength = (g_engine.song.trackLength > stepVal) ? (g_engine.song.trackLength - stepVal) : 1;
            if (g_editRow >= g_engine.song.trackLength) g_editRow = g_engine.song.trackLength - 1;
            break;
        case HIT_SS_INC:
            g_engine.song.speedMultiplier = std::min<uint8_t>(16, g_engine.song.speedMultiplier + stepVal);
            break;
        case HIT_SS_DEC:
            g_engine.song.speedMultiplier = (g_engine.song.speedMultiplier > stepVal) ? (g_engine.song.speedMultiplier - stepVal) : 1;
            break;
        case HIT_SSN_INC:
            g_engine.song.bpm = std::min<uint16_t>(255, g_engine.song.bpm + stepVal);
            break;
        case HIT_SSN_DEC:
            g_engine.song.bpm = (g_engine.song.bpm > 35 + stepVal) ? (g_engine.song.bpm - stepVal) : 35;
            break;
        case HIT_SSP_INC: {
            uint16_t& sp = g_engine.song.subSongs[g_engine.song.activeSubSong].startPos;
            sp = std::min<uint16_t>(MAX_SEQUENCES - 1, sp + stepVal);
            break;
        }
        case HIT_SSP_DEC: {
            uint16_t& sp = g_engine.song.subSongs[g_engine.song.activeSubSong].startPos;
            sp = (sp >= stepVal) ? (sp - stepVal) : 0;
            break;
        }
        case HIT_INS_INC:
            g_currentInst = std::min<uint8_t>(MAX_INSTRUMENTS - 1, g_currentInst + stepVal);
            break;
        case HIT_INS_DEC:
            g_currentInst = (g_currentInst > stepVal) ? (g_currentInst - stepVal) : 1;
            break;
        case HIT_CH_PAT_INC: {
            uint16_t s = g_engine.playSeq.load();
            int curPat = g_engine.song.sequences[s][param].patternIndex;
            g_engine.song.sequences[s][param].patternIndex = uint8_t(std::min(MAX_PATTERNS - 1, curPat + stepVal));
            break;
        }
        case HIT_CH_PAT_DEC: {
            uint16_t s = g_engine.playSeq.load();
            int curPat = g_engine.song.sequences[s][param].patternIndex;
            g_engine.song.sequences[s][param].patternIndex = uint8_t(std::max(0, curPat - stepVal));
            break;
        }
        case HIT_INST_PARAM_INC:
        case HIT_INST_PARAM_DEC: {
            int d = (id == HIT_INST_PARAM_INC) ? stepVal : -stepVal;
            switch (param) {
                case 0: inst.volume = std::clamp<int>(inst.volume + d, 0, 64); break;
                case 1: inst.waveLength = std::clamp<int>(inst.waveLength + d, 0, 5); break;
                case 2: inst.attackFrames = std::clamp<int>(inst.attackFrames + d, 1, 255); break;
                case 3: inst.attackVol = std::clamp<int>(inst.attackVol + d, 0, 64); break;
                case 4: inst.decayFrames = std::clamp<int>(inst.decayFrames + d, 1, 255); break;
                case 5: inst.decayVol = std::clamp<int>(inst.decayVol + d, 0, 64); break;
                case 6: inst.sustainFrames = std::clamp<int>(inst.sustainFrames + d, 0, 255); break;
                case 7: inst.releaseFrames = std::clamp<int>(inst.releaseFrames + d, 1, 255); break;
                case 8: inst.releaseVol = std::clamp<int>(inst.releaseVol + d, 0, 64); break;
                case 9: inst.vibratoDelay = std::clamp<int>(inst.vibratoDelay + d, 0, 255); break;
                case 10: inst.vibratoDepth = std::clamp<int>(inst.vibratoDepth + d, 0, 64); break;
                case 11: inst.vibratoSpeed = std::clamp<int>(inst.vibratoSpeed + d, 0, 64); break;
                case 12: inst.squareLower = std::clamp<int>(inst.squareLower + d, 1, 63); break;
                case 13: inst.squareUpper = std::clamp<int>(inst.squareUpper + d, 1, 63); break;
                case 14: inst.squareSpeed = std::clamp<int>(inst.squareSpeed + d, 0, 64); break;
                case 15: inst.filterLower = std::clamp<int>(inst.filterLower + d, 0, 63); break;
                case 16: inst.filterUpper = std::clamp<int>(inst.filterUpper + d, 0, 63); break;
                case 17: inst.filterSpeed = std::clamp<int>(inst.filterSpeed + d, 0, 64); break;
                case 18: g_engine.song.speedMultiplier = std::clamp<int>(g_engine.song.speedMultiplier + d, 1, 16); break;
                case 19: {
                    std::lock_guard<std::mutex> lock(g_engine.engineMutex);
                    int nextL = std::clamp<int>(int(g_engine.song.length) + d, 1, 64);
                    g_engine.setSequenceLength(uint16_t(nextL));
                    break;
                }
                case 20: inst.hardcut = std::clamp<int>(inst.hardcut + d, 0, 7); break;
            }
            break;
        }
        case HIT_THEME_RGB_INC:
        case HIT_THEME_RGB_DEC: {
            int delta = ((id == HIT_THEME_RGB_INC) ? 1 : -1) * stepVal;
            COLORREF* ptr = GetThemeSlotPtr(g_themeSlot);
            int r = GetRValue(*ptr), g = GetGValue(*ptr), b = GetBValue(*ptr);
            if (param == 0) r = std::clamp(r + delta, 0, 255);
            else if (param == 1) g = std::clamp(g + delta, 0, 255);
            else b = std::clamp(b + delta, 0, 255);
            *ptr = RGB(r, g, b);
            SaveThemeConfig();
            break;
        }
        case HIT_SMP_AMP_INC: inst.sample.ampPercent = std::clamp(inst.sample.ampPercent + float(stepVal), 0.0f, 200.0f); break;
        case HIT_SMP_AMP_DEC: inst.sample.ampPercent = std::clamp(inst.sample.ampPercent - float(stepVal), 0.0f, 200.0f); break;
        case HIT_SMP_PAN_INC: inst.sample.pan = std::clamp(inst.sample.pan + 0.05f * stepVal, -1.0f, 1.0f); break;
        case HIT_SMP_PAN_DEC: inst.sample.pan = std::clamp(inst.sample.pan - 0.05f * stepVal, -1.0f, 1.0f); break;
        case HIT_SMP_TUN_INC: inst.sample.tuneSemitones = std::clamp(inst.sample.tuneSemitones + stepVal, -24, 24); break;
        case HIT_SMP_TUN_DEC: inst.sample.tuneSemitones = std::clamp(inst.sample.tuneSemitones - stepVal, -24, 24); break;
        case HIT_SMP_REL_INC: inst.sample.relNote = std::clamp<int>(inst.sample.relNote + stepVal, 0, 84); break;
        case HIT_SMP_REL_DEC: inst.sample.relNote = std::clamp<int>(inst.sample.relNote - stepVal, 0, 84); break;
        default: break;
    }
}

// ============================================================================
// INPUT HANDLING & KEYBOARD SHORTCUTS
// ============================================================================
void UpdateChannelPagination() {
    g_activeChannel = std::clamp(g_activeChannel, 0, MAX_CHANNELS - 1);
    g_channelPageOffset = (g_activeChannel / VISIBLE_CHANNELS) * VISIBLE_CHANNELS;
}

int KeyToSemitone(WPARAM vk) {
    switch (vk) {
        case 'Z': return 0;  case 'S': return 1;  case 'X': return 2;  case 'D': return 3;
        case 'C': return 4;  case 'V': return 5;  case 'G': return 6;  case 'B': return 7;
        case 'H': return 8;  case 'N': return 9;  case 'J': return 10; case 'M': return 11;
        case 'Q': return 12; case '2': return 13; case 'W': return 14; case '3': return 15;
        case 'E': return 16; case 'R': return 17; case '5': return 18; case 'T': return 19;
        case '6': return 20; case 'Y': return 21; case '7': return 22; case 'U': return 23;
        default: return -1;
    }
}

int HexCharToVal(WPARAM vk) {
    if (vk >= '0' && vk <= '9') return int(vk - '0');
    if (vk >= 'A' && vk <= 'F') return int(vk - 'A' + 10);
    return -1;
}

int DecCharToVal(WPARAM vk) {
    if (vk >= '0' && vk <= '9') return int(vk - '0');
    return -1;
}

void HandleCharInput(WPARAM ch) {
    if (g_editingTextField == 0) return;
    if (ch == '\r') {
        std::lock_guard<std::mutex> lock(g_engine.engineMutex);
        if (g_editingTextField == 1) g_engine.song.title = g_editTextBuffer;
        else if (g_editingTextField == 2) g_engine.song.instruments[g_currentInst].name = g_editTextBuffer;
        g_editingTextField = 0;
    } else if (ch == 27) {
        g_editingTextField = 0;
    } else if (ch == '\b') {
        if (!g_editTextBuffer.empty()) g_editTextBuffer.pop_back();
    } else if (ch >= 32 && ch < 127 && g_editTextBuffer.size() < 20) {
        g_editTextBuffer.push_back(static_cast<char>(ch));
    }
}

void HandleKeyDown(HWND hwnd, WPARAM vk) {
    if (g_editingTextField != 0) return;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;

    if (alt && vk == VK_F1) {
        if (g_currentOctave < 8) g_currentOctave++;
        g_statusMsg = "OCT: C-" + std::to_string(g_currentOctave);
        return;
    }
    if (alt && vk == VK_F2) {
        if (g_currentOctave > 0) g_currentOctave--;
        g_statusMsg = "OCT: C-" + std::to_string(g_currentOctave);
        return;
    }

    if (vk == VK_F1) { g_screen = SCREEN_MAIN; return; }
    if (vk == VK_F2) { g_screen = SCREEN_INSTRUMENT; return; }
    if (vk == VK_F3) { g_savedPrefs = g_prefs; g_screen = SCREEN_PREFS; return; }
    if (vk == VK_F4) { g_pinkExternPrefsExecuted = true; g_statusMsg = "Pink Prefs OK"; return; }
    if (vk == VK_F5) { g_engine.optimizeSong(); g_statusMsg = "Optimized."; return; }
    if (vk == VK_F6) {
        g_pointerStyle = static_cast<AmigaPointerStyle>((g_pointerStyle + 1) % 3);
        ApplyActiveCursor();
        return;
    }
    if (vk == VK_F7) { ActionSaveProject(hwnd); return; }
    if (vk == VK_F8) { ActionExportAudio(hwnd); return; }
    if (vk == VK_F11) {
        static WINDOWPLACEMENT wpPrev = { sizeof(wpPrev) };
        DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
        if (style & WS_OVERLAPPEDWINDOW) {
            MONITORINFO mi = { sizeof(mi) };
            if (GetWindowPlacement(hwnd, &wpPrev) && GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
                SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
                SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top,
                             SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            }
        } else {
            SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
            SetWindowPlacement(hwnd, &wpPrev);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        return;
    }

    if (g_screen == SCREEN_FLOPPY_BROWSER) {
        if (vk == VK_UP) {
            if (g_floppySelected > 0) g_floppySelected--;
            if (g_floppySelected < g_floppyScroll) g_floppyScroll = g_floppySelected;
            return;
        }
        if (vk == VK_DOWN) {
            if (g_floppySelected + 1 < int(g_floppyItems.size())) g_floppySelected++;
            if (g_floppySelected >= g_floppyScroll + 19) g_floppyScroll = g_floppySelected - 18;
            return;
        }
        if (vk == VK_RETURN) {
            ExecuteFloppySelection(hwnd);
            return;
        }
        if (vk == VK_BACK) {
            fs::path parent = fs::path(g_floppyCurrentDir).parent_path();
            if (!parent.empty()) RefreshFloppyDirectory(parent.wstring());
            return;
        }
    }

    if (vk == VK_INSERT) {
        g_recordMode = !g_recordMode;
        if (g_recordMode) {
            std::lock_guard<std::mutex> lock(g_engine.engineMutex);
            g_engine.stopAndResetToRowZero();
            g_editRow = 0;
            g_statusMsg = "REC [INS]";
        } else {
            g_statusMsg = "REC OFF";
        }
        return;
    }

    if (vk == VK_RETURN) {
        if (g_recordMode) {
            std::lock_guard<std::mutex> lock(g_engine.engineMutex);
            g_recordMode = false;
            g_engine.playRow = 0;
            g_editRow = 0;
            g_engine.tickCounter = 0;
            g_engine.isPlaying = true;
        }
        return;
    }

    if (vk == VK_SPACE) {
        std::lock_guard<std::mutex> lock(g_engine.engineMutex);
        if (g_engine.isPlaying.load()) {
            g_engine.stopAndResetToRowZero();
            g_editRow = 0;
        } else {
            g_recordMode = false;
            g_engine.playRow = 0;
            g_editRow = 0;
            g_engine.tickCounter = 0;
            g_engine.elapsedSeconds = 0;
            g_engine.isPlaying = true;
        }
        return;
    }

    if (g_screen == SCREEN_SAMPLE_EDITOR) {
        // Live keyboard note audition inside Sample Editor
        int semi = KeyToSemitone(vk);
        if (semi >= 0 && !shift) {
            std::lock_guard<std::mutex> lock(g_engine.engineMutex);
            uint8_t noteVal = static_cast<uint8_t>(std::clamp(g_currentOctave * 12 + semi + 1, 1, 97));
            g_engine.triggerNote(0, noteVal, g_currentInst, 0, 0, 0);
            return;
        }

        std::lock_guard<std::mutex> lock(g_engine.engineMutex);
        Instrument& inst = g_engine.song.instruments[g_currentInst];
        if (shift && vk == 'A') {
            if (!g_sampleUndoBuffer.empty()) {
                g_sampleRedoBuffer = inst.sample.floatData;
                inst.sample.floatData = g_sampleUndoBuffer;
                g_statusMsg = "Sample Undo.";
            }
            return;
        }
        if (shift && vk == 'Z') {
            if (!g_sampleRedoBuffer.empty()) {
                g_sampleUndoBuffer = inst.sample.floatData;
                inst.sample.floatData = g_sampleRedoBuffer;
                g_statusMsg = "Sample Redo.";
            }
            return;
        }
        if (shift && vk == 'C') {
            if (inst.hasSample && !inst.sample.floatData.empty()) {
                size_t i0 = 0, i1 = 0;
                bool hasRange = false;
                GetSampleSelectionIndices(inst, i0, i1, hasRange);
                if (hasRange && i1 > i0) {
                    g_sampleClipboard.assign(inst.sample.floatData.begin() + i0, inst.sample.floatData.begin() + i1);
                    g_statusMsg = "Selection Copied.";
                }
            }
            return;
        }
        if (shift && vk == 'V') {
            if (!g_sampleClipboard.empty()) {
                g_sampleUndoBuffer = inst.sample.floatData;
                size_t insertPos = inst.sample.floatData.size();
                size_t i0 = 0, i1 = 0;
                bool hasRange = false;
                GetSampleSelectionIndices(inst, i0, i1, hasRange);
                if (hasRange) insertPos = i1;
                inst.sample.floatData.insert(inst.sample.floatData.begin() + insertPos, g_sampleClipboard.begin(), g_sampleClipboard.end());
                inst.hasSample = true;
                g_statusMsg = "Slice Pasted.";
            }
            return;
        }
        if (vk == VK_DELETE) {
            ApplySampleDSP(5);
            return;
        }
    }

    if (g_screen == SCREEN_MAIN) {
        if (vk == VK_TAB) {
            if (shift) g_activeChannel = (g_activeChannel > 0) ? (g_activeChannel - 1) : (MAX_CHANNELS - 1);
            else g_activeChannel = (g_activeChannel + 1) % MAX_CHANNELS;
            UpdateChannelPagination();
            return;
        }
        if (vk == VK_PRIOR) { g_editRow = std::max(0, g_editRow - 8); return; }
        if (vk == VK_NEXT)  { g_editRow = std::min<int>(g_engine.song.trackLength - 1, g_editRow + 8); return; }

        if (shift && vk == 'C') {
            if (g_hasSelection) {
                uint16_t seq = g_engine.playSeq.load();
                uint8_t pat = uint8_t((int(g_engine.song.sequences[seq][g_selChannel].patternIndex) * MAX_CHANNELS + g_selChannel) % MAX_PATTERNS);
                int r0 = std::min(g_selStartRow, g_selEndRow);
                int r1 = std::max(g_selStartRow, g_selEndRow);
                g_clipboardCells.clear();
                for (int r = r0; r <= r1; ++r) g_clipboardCells.push_back(g_engine.song.patterns[pat].rows[r]);
                g_statusMsg = "Copied.";
            }
            return;
        }
        if (shift && vk == 'V') {
            if (!g_clipboardCells.empty()) {
                std::lock_guard<std::mutex> lock(g_engine.engineMutex);
                uint16_t seq = g_engine.playSeq.load();
                g_engine.ensureUniqueChannelIsolation(seq);
                uint8_t pat = uint8_t((int(g_engine.song.sequences[seq][g_activeChannel].patternIndex) * MAX_CHANNELS + g_activeChannel) % MAX_PATTERNS);
                for (size_t i = 0; i < g_clipboardCells.size(); ++i) {
                    int tr = g_editRow + int(i);
                    if (tr < g_engine.song.trackLength) g_engine.song.patterns[pat].rows[tr] = g_clipboardCells[i];
                }
                g_statusMsg = "Pasted.";
                g_hasSelection = false;
            }
            return;
        }

        if (vk == VK_UP || vk == VK_DOWN) {
            int delta = (vk == VK_UP) ? -1 : 1;
            if (shift) {
                if (!g_hasSelection || g_selChannel != g_activeChannel) {
                    g_hasSelection = true;
                    g_selChannel = g_activeChannel;
                    g_selStartRow = g_editRow;
                }
                g_editRow = std::clamp<int>(g_editRow + delta, 0, g_engine.song.trackLength - 1);
                g_selEndRow = g_editRow;
            } else {
                g_hasSelection = false;
                g_editRow = std::clamp<int>(g_editRow + delta, 0, g_engine.song.trackLength - 1);
            }
            return;
        }

        if (vk == VK_LEFT || vk == VK_RIGHT) {
            int delta = (vk == VK_LEFT) ? -1 : 1;
            g_editCol += delta;
            if (g_editCol < 0) {
                g_editCol = 5; g_activeChannel = std::max(0, g_activeChannel - 1); UpdateChannelPagination();
            } else if (g_editCol > 5) {
                g_editCol = 0; g_activeChannel = std::min(MAX_CHANNELS - 1, g_activeChannel + 1); UpdateChannelPagination();
            }
            return;
        }

        if (vk == VK_DELETE) {
            std::lock_guard<std::mutex> lock(g_engine.engineMutex);
            uint16_t seq = g_engine.playSeq.load();
            g_engine.ensureUniqueChannelIsolation(seq);
            uint8_t pat = uint8_t((int(g_engine.song.sequences[seq][g_activeChannel].patternIndex) * MAX_CHANNELS + g_activeChannel) % MAX_PATTERNS);
            if (g_hasSelection) {
                int r0 = std::min(g_selStartRow, g_selEndRow);
                int r1 = std::max(g_selStartRow, g_selEndRow);
                for (int r = r0; r <= r1; ++r) {
                    PatternCell& cell = g_engine.song.patterns[pat].rows[r];
                    cell.note = 0; cell.instrument = 0; cell.fxCmd = 0; cell.fxParam = 0;
                }
                g_hasSelection = false;
                g_statusMsg = "Selection Deleted.";
            } else {
                PatternCell& cell = g_engine.song.patterns[pat].rows[g_editRow];
                if (g_editCol == 0) { cell.note = 0; cell.instrument = 0; }
                else { cell.fxCmd = 0; cell.fxParam = 0; }
                g_editRow = (g_editRow + 1) % g_engine.song.trackLength;
            }
            return;
        }

        uint16_t seq = g_engine.playSeq.load();
        g_engine.ensureUniqueChannelIsolation(seq);
        uint8_t pat = uint8_t((int(g_engine.song.sequences[seq][g_activeChannel].patternIndex) * MAX_CHANNELS + g_activeChannel) % MAX_PATTERNS);
        PatternCell& cell = g_engine.song.patterns[pat].rows[g_editRow];

        if (g_editCol == 0) {
            int semi = KeyToSemitone(vk);
            if (semi >= 0) {
                std::lock_guard<std::mutex> lock(g_engine.engineMutex);
                uint8_t noteVal = static_cast<uint8_t>(std::clamp(g_currentOctave * 12 + semi + 1, 1, 97));
                g_engine.triggerNote(g_activeChannel, noteVal, g_currentInst,
                                     g_engine.song.sequences[seq][g_activeChannel].transpose, 0, 0);
                // Writes exclusively into the active channel pattern and advances linearly to next row
                if (!IsTestModeActive() && (g_recordMode || !g_engine.isPlaying.load())) {
                    cell.note = noteVal;
                    cell.instrument = g_currentInst;
                    g_editRow = (g_editRow + 1) % g_engine.song.trackLength;
                    if (g_recordMode) {
                        g_engine.playRow = uint8_t(g_editRow);
                    }
                }
            }
        } else {
            int hv = HexCharToVal(vk);
            int dv = DecCharToVal(vk);
            if (g_editCol == 1 && dv >= 0) {
                std::lock_guard<std::mutex> lock(g_engine.engineMutex);
                cell.instrument = std::clamp<int>((dv * 10) + (cell.instrument % 10), 0, 63);
                g_editRow = (g_editRow + 1) % g_engine.song.trackLength;
            } else if (g_editCol == 2 && dv >= 0) {
                std::lock_guard<std::mutex> lock(g_engine.engineMutex);
                cell.instrument = std::clamp<int>((cell.instrument / 10) * 10 + dv, 0, 63);
                g_editRow = (g_editRow + 1) % g_engine.song.trackLength;
            } else if (g_editCol == 3 && hv >= 0) {
                std::lock_guard<std::mutex> lock(g_engine.engineMutex);
                cell.fxCmd = uint8_t(hv);
                g_editRow = (g_editRow + 1) % g_engine.song.trackLength;
            } else if (g_editCol == 4 && dv >= 0) {
                std::lock_guard<std::mutex> lock(g_engine.engineMutex);
                int curVal = std::clamp<int>(cell.fxParam, 0, 64);
                cell.fxParam = uint8_t(std::clamp<int>((dv * 10) + (curVal % 10), 0, 64));
                g_editRow = (g_editRow + 1) % g_engine.song.trackLength;
            } else if (g_editCol == 5 && dv >= 0) {
                std::lock_guard<std::mutex> lock(g_engine.engineMutex);
                int curVal = std::clamp<int>(cell.fxParam, 0, 64);
                cell.fxParam = uint8_t(std::clamp<int>((curVal / 10) * 10 + dv, 0, 64));
                g_editRow = (g_editRow + 1) % g_engine.song.trackLength;
            }
        }
    } else if (g_screen == SCREEN_INSTRUMENT) {
        Instrument& inst = g_engine.song.instruments[g_currentInst];
        if (vk == VK_UP)   { g_perfEditRow = std::max(0, g_perfEditRow - 1); return; }
        if (vk == VK_DOWN) { g_perfEditRow = std::min(21, g_perfEditRow + 1); return; }
        if (vk == VK_LEFT) { g_perfEditCol = std::max(0, g_perfEditCol - 1); return; }
        if (vk == VK_RIGHT){ g_perfEditCol = std::min(8, g_perfEditCol + 1); return; }

        if (vk == VK_DELETE) {
            PerfStep& ps = inst.perfList[g_perfEditRow];
            if (g_perfEditCol <= 1) { ps.note = 0; ps.fixedNote = false; }
            else if (g_perfEditCol == 2) { ps.waveform = 0; }
            else if (g_perfEditCol >= 3 && g_perfEditCol <= 5) { ps.fx1 = 0; }
            else { ps.fx2 = 0; }
            return;
        }

        if (g_perfEditCol == 0) {
            int semi = KeyToSemitone(vk);
            if (semi >= 0) {
                std::lock_guard<std::mutex> lock(g_engine.engineMutex);
                uint8_t noteVal = static_cast<uint8_t>(std::clamp(g_currentOctave * 12 + semi + 1, 1, 97));
                g_engine.triggerNote(0, noteVal, g_currentInst, 0, 0, 0);
                if (!IsTestModeActive()) {
                    inst.perfList[g_perfEditRow].note = noteVal;
                    if (g_perfEditRow >= inst.perfLen) inst.perfLen = uint8_t(g_perfEditRow + 1);
                    g_perfEditRow = std::min(21, g_perfEditRow + 1);
                }
            }
        } else if (g_perfEditCol == 1 && vk == VK_SPACE) {
            inst.perfList[g_perfEditRow].fixedNote = !inst.perfList[g_perfEditRow].fixedNote;
        } else if (g_perfEditCol == 2 && vk >= '0' && vk <= '4') {
            inst.perfList[g_perfEditRow].waveform = uint8_t(vk - '0');
            if (g_perfEditRow >= inst.perfLen) inst.perfLen = uint8_t(g_perfEditRow + 1);
            g_perfEditRow = std::min(21, g_perfEditRow + 1);
        } else if (g_perfEditCol >= 3) {
            int hv = HexCharToVal(vk);
            int dv = DecCharToVal(vk);
            PerfStep& ps = inst.perfList[g_perfEditRow];
            if (g_perfEditCol == 3 && hv >= 0) {
                ps.fx1 = (ps.fx1 & 0x0FF) | (hv << 8);
                g_perfEditCol = 4;
            } else if (g_perfEditCol == 4 && dv >= 0) {
                int p = std::clamp<int>(ps.fx1 & 0xFF, 0, 64);
                int np = std::clamp<int>((dv * 10) + (p % 10), 0, 64);
                ps.fx1 = (ps.fx1 & 0xF00) | uint16_t(np);
                g_perfEditCol = 5;
            } else if (g_perfEditCol == 5 && dv >= 0) {
                int p = std::clamp<int>(ps.fx1 & 0xFF, 0, 64);
                int np = std::clamp<int>((p / 10) * 10 + dv, 0, 64);
                ps.fx1 = (ps.fx1 & 0xF00) | uint16_t(np);
                g_perfEditCol = 3;
            } else if (g_perfEditCol == 6 && hv >= 0) {
                ps.fx2 = (ps.fx2 & 0x0FF) | (hv << 8);
                g_perfEditCol = 7;
            } else if (g_perfEditCol == 7 && dv >= 0) {
                int p = std::clamp<int>(ps.fx2 & 0xFF, 0, 64);
                int np = std::clamp<int>((dv * 10) + (p % 10), 0, 64);
                ps.fx2 = (ps.fx2 & 0xF00) | uint16_t(np);
                g_perfEditCol = 8;
            } else if (g_perfEditCol == 8 && dv >= 0) {
                int p = std::clamp<int>(ps.fx2 & 0xFF, 0, 64);
                int np = std::clamp<int>((p / 10) * 10 + dv, 0, 64);
                ps.fx2 = (ps.fx2 & 0xF00) | uint16_t(np);
                g_perfEditCol = 6;
            }
            if (g_perfEditRow >= inst.perfLen) inst.perfLen = uint8_t(g_perfEditRow + 1);
        }
    }
}

void HandleKeyUp(WPARAM vk) {
    int semi = KeyToSemitone(vk);
    if (semi >= 0) {
        std::lock_guard<std::mutex> lock(g_engine.engineMutex);
        g_engine.releaseNote(g_screen == SCREEN_MAIN ? g_activeChannel : 0);
    }
}

void HandleClickScaled(HWND hwnd, int winX, int winY) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int winW = std::max(1, int(rc.right - rc.left));
    int winH = std::max(1, int(rc.bottom - rc.top));

    int x = (winX * NATIVE_W) / winW;
    int y = (winY * NATIVE_H) / winH;

    if (g_editingTextField != 0) {
        std::lock_guard<std::mutex> lock(g_engine.engineMutex);
        if (g_editingTextField == 1) g_engine.song.title = g_editTextBuffer;
        else if (g_editingTextField == 2) g_engine.song.instruments[g_currentInst].name = g_editTextBuffer;
        g_editingTextField = 0;
    }

    for (const auto& hb : g_hitBoxes) {
        if (x >= hb.rc.left && x < hb.rc.right && y >= hb.rc.top && y < hb.rc.bottom) {
            Instrument& inst = g_engine.song.instruments[g_currentInst];

            if (hb.isSpin) {
                ExecuteSpinAction(hb.id, hb.param, 1);
                g_heldSpinId = hb.id;
                g_heldSpinParam = hb.param;
                g_heldSpinTicks = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }

            switch (hb.id) {
                case HIT_TOP_TOGGLE:
                    g_screen = (g_screen == SCREEN_MAIN) ? SCREEN_INSTRUMENT : SCREEN_MAIN;
                    break;
                case HIT_ABYSS_DEXTER_BTN:
                    g_screen = SCREEN_THEME_FILTER;
                    break;
                case HIT_DINK_BTN:
                    g_screen = SCREEN_AUDIO_HELP;
                    break;
                case HIT_PERF_EDITOR_BTN:
                    g_screen = SCREEN_SAMPLE_EDITOR;
                    break;
                case HIT_MAIN_BTN:
                    g_screen = SCREEN_MAIN;
                    break;
                case HIT_INS_BTN:
                    g_screen = SCREEN_INSTRUMENT;
                    break;
                case HIT_LOAD_SONG:
                    if (g_prefs.songsDir != L"&:" && fs::exists(fs::path(g_prefs.songsDir))) {
                        OpenFloppyBrowser(FLOPPY_SONGS);
                    } else {
                        ActionLoadProjectOrModuleDialog(hwnd);
                    }
                    break;
                case HIT_LOAD_INS:
                    if (g_prefs.instrDir != L"/    Instruments" && fs::exists(fs::path(g_prefs.instrDir))) {
                        OpenFloppyBrowser(FLOPPY_INSTRUMENTS);
                    } else {
                        ActionLoadSampleDialog(hwnd);
                    }
                    break;
                case HIT_EDIT_SONG_NAME:
                    g_editingTextField = 1;
                    g_editTextBuffer = g_engine.song.title;
                    break;
                case HIT_EDIT_INST_NAME:
                    g_editingTextField = 2;
                    g_editTextBuffer = inst.name;
                    break;
                case HIT_PLAY: {
                    std::lock_guard<std::mutex> lock(g_engine.engineMutex);
                    g_recordMode = false;
                    g_engine.tickCounter = 0;
                    g_engine.isPlaying = true;
                    break;
                }
                case HIT_STOP: {
                    std::lock_guard<std::mutex> lock(g_engine.engineMutex);
                    g_recordMode = false;
                    g_engine.stopAndResetToRowZero();
                    g_editRow = 0;
                    break;
                }
                case HIT_CH_SELECT:
                    g_activeChannel = hb.param;
                    break;
                case HIT_CH_MUTE:
                    g_engine.voices[hb.param].muted = !g_engine.voices[hb.param].muted;
                    break;
                case HIT_CH_PAGE_UP:
                    g_activeChannel = std::max(0, g_activeChannel - 4);
                    UpdateChannelPagination();
                    break;
                case HIT_CH_PAGE_DN:
                    g_activeChannel = std::min(MAX_CHANNELS - 1, g_activeChannel + 4);
                    UpdateChannelPagination();
                    break;
                case HIT_RELEASE_CUT: inst.releaseCut = !inst.releaseCut; break;
                case HIT_TEST_MODE: {
                    keybd_event(VK_CAPITAL, 0x45, KEYEVENTF_EXTENDEDKEY, 0);
                    keybd_event(VK_CAPITAL, 0x45, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
                    break;
                }
                case HIT_PERF_CELL:
                    g_perfEditRow = (hb.param >> 4) & 0x3F;
                    g_perfEditCol = hb.param & 0x0F;
                    if (g_perfEditCol == 1) {
                        inst.perfList[g_perfEditRow].fixedNote = !inst.perfList[g_perfEditRow].fixedNote;
                    } else if (g_perfEditCol == 2) {
                        inst.perfList[g_perfEditRow].waveform = (inst.perfList[g_perfEditRow].waveform + 1) % 5;
                    }
                    break;
                case HIT_WAVE_ITEM:
                    if (hb.param == 5) {
                        inst.perfList[g_perfEditRow].fixedNote = !inst.perfList[g_perfEditRow].fixedNote;
                    } else {
                        inst.perfList[g_perfEditRow].waveform = uint8_t(hb.param);
                        if (g_perfEditRow >= inst.perfLen) inst.perfLen = uint8_t(g_perfEditRow + 1);
                    }
                    break;
                case HIT_CMD_ITEM:
                    if (g_perfEditCol >= 6) inst.perfList[g_perfEditRow].fx2 = uint16_t(hb.param);
                    else inst.perfList[g_perfEditRow].fx1 = uint16_t(hb.param);
                    if (g_perfEditRow >= inst.perfLen) inst.perfLen = uint8_t(g_perfEditRow + 1);
                    break;

                case HIT_FILTER_CYCLE:
                    g_engine.song.filterModel = static_cast<AmigaFilterModel>((g_engine.song.filterModel + 1) % 4);
                    break;
                case HIT_THEME_SLOT:
                    g_themeSlot = hb.param;
                    break;
                case HIT_THEME_RESET:
                    g_theme = ThemeColors{};
                    SaveThemeConfig();
                    break;
                case HIT_THEME_SAVE:
                    SaveThemeConfig();
                    g_screen = SCREEN_MAIN;
                    break;

                case HIT_AUDIO_BUF_SET:
                    g_bufferIdx = hb.param;
                    RestartAudioOutput();
                    break;
                case HIT_AUDIO_SR_SET:
                    g_sampleRateIdx = hb.param;
                    RestartAudioOutput();
                    break;

                case HIT_SMP_WAVE_AREA: {
                    float relX = std::clamp(float(x - hb.rc.left) / float(std::max<LONG>(1, hb.rc.right - hb.rc.left)), 0.0f, 1.0f);
                    g_smpSelStart = relX;
                    g_smpSelEnd = relX;
                    g_draggingSample = true;
                    break;
                }
                case HIT_SMP_ZOOM_IN:
                    g_smpZoom = std::min(16.0f, g_smpZoom * 2.0f);
                    break;
                case HIT_SMP_ZOOM_OUT:
                    g_smpZoom = std::max(1.0f, g_smpZoom / 2.0f);
                    if (g_smpZoom <= 1.0f) g_smpViewOffset = 0.0f;
                    break;
                case HIT_SMP_NORM: ApplySampleDSP(0); break;
                case HIT_SMP_REV:  ApplySampleDSP(1); break;
                case HIT_SMP_LOOP: ApplySampleDSP(2); break;
                case HIT_SMP_FIN:  ApplySampleDSP(3); break;
                case HIT_SMP_FOUT: ApplySampleDSP(4); break;
                case HIT_SMP_DEL:  ApplySampleDSP(5); break;
                case HIT_SMP_PLAY_TEST: {
                    std::lock_guard<std::mutex> lock(g_engine.engineMutex);
                    g_engine.triggerNote(0, 49, g_currentInst, 0, 0, 0);
                    break;
                }

                case HIT_PREFS_SONGS_DIR: {
                    std::wstring dir = BrowseAmigaFolder(hwnd, L"Select Songs Floppy Disk Folder");
                    if (!dir.empty()) {
                        g_prefs.songsDir = dir;
                        OpenFloppyBrowser(FLOPPY_SONGS);
                    }
                    break;
                }
                case HIT_PREFS_INSTR_DIR: {
                    std::wstring dir = BrowseAmigaFolder(hwnd, L"Select Samples Floppy Disk Folder");
                    if (!dir.empty()) {
                        g_prefs.instrDir = dir;
                        OpenFloppyBrowser(FLOPPY_INSTRUMENTS);
                    }
                    break;
                }
                case HIT_PREFS_CHK_VBL:    g_prefs.explicitVblStartup = !g_prefs.explicitVblStartup; break;
                case HIT_PREFS_CHK_ALLOC:  g_prefs.allocateAudioChannels = !g_prefs.allocateAudioChannels; break;
                case HIT_PREFS_CHK_FONT:   g_prefs.ptFontInRequesters = !g_prefs.ptFontInRequesters; break;
                case HIT_PREFS_CHK_TRANS:  g_prefs.transparentVolumeMeter = !g_prefs.transparentVolumeMeter; break;
                case HIT_PREFS_CYC_VMETER: g_prefs.volumeMeterMode = (g_prefs.volumeMeterMode + 1) % 3; break;
                case HIT_PREFS_CYC_CURSOR: g_prefs.cursorMode = (g_prefs.cursorMode + 1) % 2; break;
                case HIT_PREFS_CYC_RASTER: g_prefs.rastertimeMode = (g_prefs.rastertimeMode + 1) % 2; break;
                case HIT_PREFS_SAVE:
                    g_savedPrefs = g_prefs;
                    g_engine.song.ciaTiming = !g_prefs.explicitVblStartup;
                    g_screen = SCREEN_MAIN;
                    break;
                case HIT_PREFS_CURRENT:
                    if (g_pinkExternPrefsExecuted) {
                        g_engine.song.ciaTiming = !g_prefs.explicitVblStartup;
                        g_screen = SCREEN_MAIN;
                    } else {
                        g_statusMsg = "Press F4 First";
                    }
                    break;
                case HIT_PREFS_CANCEL:
                    g_prefs = g_savedPrefs;
                    g_screen = SCREEN_MAIN;
                    break;

                case HIT_FLOPPY_ITEM:
                    if (g_floppySelected == hb.param) {
                        ExecuteFloppySelection(hwnd);
                    } else {
                        g_floppySelected = hb.param;
                    }
                    break;
                case HIT_FLOPPY_PARENT: {
                    fs::path parent = fs::path(g_floppyCurrentDir).parent_path();
                    if (!parent.empty()) RefreshFloppyDirectory(parent.wstring());
                    break;
                }
                case HIT_FLOPPY_SYSTEM_DLG:
                    if (g_floppyMode == FLOPPY_SONGS) ActionLoadProjectOrModuleDialog(hwnd);
                    else { ActionLoadSampleDialog(hwnd); g_screen = SCREEN_INSTRUMENT; }
                    break;
                case HIT_FLOPPY_LOAD_BTN:
                    ExecuteFloppySelection(hwnd);
                    break;
                default:
                    break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, 1, 20, nullptr);
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                ApplyActiveCursor();
                return TRUE;
            }
            break;

        case WM_TIMER: {
            bool needRepaint = false;
            if (g_heldSpinId != 0) {
                if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
                    g_heldSpinTicks++;
                    if (g_heldSpinTicks > 14 && (g_heldSpinTicks % 2 == 0)) {
                        int accel = (g_heldSpinTicks > 50) ? 4 : 2;
                        ExecuteSpinAction(g_heldSpinId, g_heldSpinParam, accel);
                        needRepaint = true;
                    }
                } else {
                    g_heldSpinId = 0;
                    g_heldSpinTicks = 0;
                }
            }
            if (g_engine.isPlaying.load() || g_editingTextField != 0 || needRepaint) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_CHAR:
            HandleCharInput(wParam);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_SYSKEYDOWN:
            if (wParam == VK_F1 || wParam == VK_F2) {
                HandleKeyDown(hwnd, wParam);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_KEYDOWN:
            HandleKeyDown(hwnd, wParam);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_KEYUP:
            HandleKeyUp(wParam);
            return 0;

        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            HandleClickScaled(hwnd, LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_MOUSEMOVE:
            if (g_draggingSample && g_screen == SCREEN_SAMPLE_EDITOR) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int winW = std::max(1, int(rc.right - rc.left));
                int x = (LOWORD(lParam) * NATIVE_W) / winW;
                g_smpSelEnd = std::clamp(float(x - 6) / 308.0f, 0.0f, 1.0f);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            g_draggingSample = false;
            g_heldSpinId = 0;
            g_heldSpinTicks = 0;
            ReleaseCapture();
            return 0;

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (g_screen == SCREEN_FLOPPY_BROWSER) {
                int maxScroll = std::max(0, int(g_floppyItems.size()) - 19);
                if (delta > 0) g_floppyScroll = std::max(0, g_floppyScroll - 2);
                else g_floppyScroll = std::min(maxScroll, g_floppyScroll + 2);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (g_screen == SCREEN_SAMPLE_EDITOR && g_smpZoom > 1.0f) {
                if (delta > 0) g_smpViewOffset = std::max(0.0f, g_smpViewOffset - 0.1f);
                else g_smpViewOffset = std::min(1.0f, g_smpViewOffset + 0.1f);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (g_screen == SCREEN_MAIN) {
                if (delta > 0) g_editRow = std::max(0, g_editRow - 2);
                else g_editRow = std::min<int>(g_engine.song.trackLength - 1, g_editRow + 2);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            g_fb.init(hdc);
            g_hitBoxes.clear();

            if (g_screen == SCREEN_MAIN) RenderMainScreen320();
            else if (g_screen == SCREEN_INSTRUMENT) RenderInstrumentScreen320();
            else if (g_screen == SCREEN_PREFS) RenderPrefsScreen320();
            else if (g_screen == SCREEN_THEME_FILTER) RenderThemeAndFilterScreen320();
            else if (g_screen == SCREEN_AUDIO_HELP) RenderAudioAndHelpScreen320();
            else if (g_screen == SCREEN_SAMPLE_EDITOR) RenderSampleEditorScreen320();
            else if (g_screen == SCREEN_FLOPPY_BROWSER) RenderFloppyBrowserScreen320();

            RECT rc;
            GetClientRect(hwnd, &rc);
            SetStretchBltMode(hdc, COLORONCOLOR);
            StretchBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                       g_fb.memDC, 0, 0, NATIVE_W, NATIVE_H, SRCCOPY);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, 1);
            StopAudioOutput();
            g_fb.destroy();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    LoadThemeConfig();

    g_hCurWB1 = BuildAmigaHardwareCursor(0);
    g_hCurWB2 = BuildAmigaHardwareCursor(1);
    g_hCurMag = BuildAmigaHardwareCursor(2);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = g_hCurWB1 ? g_hCurWB1 : LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AHXTrackerNativeWin32Class";
    RegisterClassExW(&wc);

    RECT wr = { 0, 0, 960, 768 };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName,
        L"AHX Tracker v2.7 - Abyss / Dexter & Pink / QST Native",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInstance, nullptr
    );

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    RestartAudioOutput();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hCurWB1) DestroyCursor(g_hCurWB1);
    if (g_hCurWB2) DestroyCursor(g_hCurWB2);
    if (g_hCurMag) DestroyCursor(g_hCurMag);
    MFShutdown();
    CoUninitialize();
    return 0;
}
