#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmeapi.h>
#include <mmreg.h>

#define _USE_MATH_DEFINES
#include <math.h>

#include <plog/Log.h>
#include <plog/Init.h>
#include <plog/Formatters/TxtFormatter.h>
#include <plog/Appenders/ColorConsoleAppender.h>

#define SAFE_RELEASE(x)   \
    if ((x) != nullptr) { \
        (x)->Release();   \
        (x) = nullptr;    \
    }

#define EXIT_ON_ERROR(x)                                                \
    if (HRESULT result = (x); FAILED(result)) {                         \
        wchar_t buf[256]{};                                             \
                                                                        \
        FormatMessageW(                                                 \
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, \
            nullptr,                                                    \
            (result),                                                   \
            locale_id,                                                  \
            (wchar_t*)&buf,                                             \
            sizeof(buf) / sizeof(buf[0]),                               \
            nullptr                                                     \
        );                                                              \
                                                                        \
        PLOG_ERROR << buf;                                              \
        exit(EXIT_FAILURE);                                             \
    }                                                                   \

enum class RenderSampleType {
    Float,
    PCM16bit
};

static unsigned long locale_id = []() {
    wchar_t locale[32];
    GetLocaleInfoEx(
        LOCALE_NAME_USER_DEFAULT,
        LOCALE_ILANGUAGE,
        locale,
        sizeof(locale) / sizeof(locale[0])
    );

    return wcstoul(locale, nullptr, 16);
}();

double sign(double val) {
    return (0.0 < val) - (val < 0.0);
}

static double sine(double frequency, double time) {
    return sin(2 * M_PI * frequency * time);
}

static double triangle(double frequency, double time) {
    return 2 * abs(2 * (time * frequency - floor(time * frequency + 0.5))) - 1;
}

static double square(double frequency, double time) {
    return sign(sin(2 * M_PI * frequency * time));
}

static double sawtooth(double frequency, double time) {
    return 2 * (time * frequency - floor(time * frequency + 0.5));
}

template<typename T>
static void generate_samples(
    unsigned char* buffer,
    size_t length,
    uint16_t frequency,
    double volume,
    uint16_t channel_count,
    uint32_t samples_per_second,
    double* initial_time,
    double (*generator)(double, double)
) {
    double increment = 1.0 / samples_per_second;
    T* data = reinterpret_cast<T*>(buffer);
    double time = initial_time != nullptr ? *initial_time : 0;

    for (size_t i = 0; i < length / sizeof(T); i += channel_count) {
        double value = generator(frequency, time);

        for (size_t j = 0; j < channel_count; ++j) {
            if constexpr (std::is_same_v<T, float>) {
                data[i + j] = (float)(volume * value);
            }
            else if constexpr (std::is_same_v<T, short>) {
                data[i + j] = (short)(volume * value * _I16_MAX);
            }
        }

        time += increment;
    }

    if (initial_time != nullptr) {
        *initial_time = time;
    }
}

int main()
{
    const uint16_t frequency = 220;
    const uint8_t duration = 5;
    const double volume = 0.3;

    static plog::ColorConsoleAppender<plog::TxtFormatter> console_appender;
    plog::init(plog::verbose, &console_appender);

    EXIT_ON_ERROR(CoInitializeEx(nullptr, COINITBASE_MULTITHREADED));

    IMMDeviceEnumerator* enumerator = nullptr;
    EXIT_ON_ERROR(CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        0,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator)
    );

    IMMDevice* device = nullptr;
    EXIT_ON_ERROR(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));

    IAudioClient* client = nullptr;
    EXIT_ON_ERROR(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void* *)&client));

    WAVEFORMATEX* format = nullptr;
    EXIT_ON_ERROR(client->GetMixFormat(&format));

    RenderSampleType sample_type;

    if (format->wFormatTag == WAVE_FORMAT_PCM || (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)) {
        if (format->wBitsPerSample == 16) {
            PLOG_INFO << "Sample type: 16bit PCM";
            sample_type = RenderSampleType::PCM16bit;
        }
        else {
            PLOG_ERROR << "Unknown PCM integer type";
            exit(EXIT_FAILURE);
        }
    }
    else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
        PLOG_INFO << "Sample type: float";
        sample_type = RenderSampleType::Float;
    }
    else {
        PLOG_ERROR << "Unknown device format";
        exit(EXIT_FAILURE);
    }

    EXIT_ON_ERROR(client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
        0, // Get smallet possible buffer from the audio engine to minimize latency
        0,
        format,
        nullptr
    ));

    HANDLE samples_ready_event = CreateEventEx(nullptr, nullptr, 0, SYNCHRONIZE | EVENT_MODIFY_STATE);
    if (samples_ready_event == nullptr) {
        EXIT_ON_ERROR(HRESULT_FROM_WIN32(GetLastError()));
    }
    EXIT_ON_ERROR(client->SetEventHandle(samples_ready_event));

    uint32_t buffer_size = 0;
    EXIT_ON_ERROR(client->GetBufferSize(&buffer_size));

    int64_t default_period;
    EXIT_ON_ERROR(client->GetDevicePeriod(&default_period, nullptr));

    uint32_t buffer_size_per_period = static_cast<uint32_t>((default_period / 1e7) * format->nSamplesPerSec + 0.5);
    uint32_t buffer_size_bytes = buffer_size_per_period * format->nBlockAlign;
    size_t data_length = (format->nSamplesPerSec * duration * format->nBlockAlign) + (buffer_size_bytes - 1);
    size_t buffer_count = data_length / buffer_size_bytes;

    size_t written_buffers = 0;
    unsigned char* data;
    uint32_t padding;
    uint32_t frames_available;
    double time = 0;

    EXIT_ON_ERROR(client->Start());

    IAudioRenderClient* render = nullptr;
    EXIT_ON_ERROR(client->GetService(__uuidof(IAudioRenderClient), (void**)&render));

    // One buffer's worth of silence to avoid glitches at the start
    {
        WaitForSingleObject(samples_ready_event, INFINITE);
        unsigned char* tmp;
        EXIT_ON_ERROR(client->GetCurrentPadding(&padding));
        EXIT_ON_ERROR(render->GetBuffer(buffer_size - padding, &tmp));
        EXIT_ON_ERROR(render->ReleaseBuffer(buffer_size - padding, AUDCLNT_BUFFERFLAGS_SILENT));
    }

    while (written_buffers < buffer_count) {
        unsigned long wait_result = WaitForSingleObject(samples_ready_event, INFINITE);

        switch (wait_result) {
        case WAIT_FAILED:
            EXIT_ON_ERROR(HRESULT_FROM_WIN32(GetLastError()));
            break;
        case WAIT_OBJECT_0:
            EXIT_ON_ERROR(client->GetCurrentPadding(&padding));
            frames_available = buffer_size - padding;
            if (buffer_size_bytes <= frames_available * format->nBlockAlign) {
                uint32_t frames_to_write = buffer_size_bytes / format->nBlockAlign;

                EXIT_ON_ERROR(render->GetBuffer(frames_to_write, &data));
                switch (sample_type) {
                    case RenderSampleType::Float:
                        generate_samples<float>(
                            data,
                            buffer_size_bytes,
                            frequency,
                            volume,
                            format->nChannels,
                            format->nSamplesPerSec,
                            &time,
                            sawtooth
                        );
                        break;
                    case RenderSampleType::PCM16bit:
                        generate_samples<short>(
                            data,
                            buffer_size_bytes,
                            frequency,
                            volume,
                            format->nChannels,
                            format->nSamplesPerSec,
                            &time,
                            square
                        );
                        break;
                }
                EXIT_ON_ERROR(render->ReleaseBuffer(frames_to_write, 0));

                written_buffers += 1;
            }
        break;
        }
    }

    // One buffer's worth of silence to avoid glitches at the end
    {
        WaitForSingleObject(samples_ready_event, INFINITE);
        unsigned char* tmp;
        EXIT_ON_ERROR(client->GetCurrentPadding(&padding));
        EXIT_ON_ERROR(render->GetBuffer(buffer_size - padding, &tmp));
        EXIT_ON_ERROR(render->ReleaseBuffer(buffer_size - padding, AUDCLNT_BUFFERFLAGS_SILENT));
    }

    EXIT_ON_ERROR(client->Stop());
    CoUninitialize();

    CloseHandle(samples_ready_event);

    CoTaskMemFree(format);
    SAFE_RELEASE(enumerator);
    SAFE_RELEASE(device);
    SAFE_RELEASE(client);
    SAFE_RELEASE(render);
}
