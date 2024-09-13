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

#define EXIT_ON_ERROR(result)                                           \
    if (FAILED((result))) {                                             \
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

constexpr int frequency = 440;
constexpr int latency = 50;
constexpr int duration = 5;

static double square(double theta) {
    return (4 / M_PI) * (sin(theta) + sin(3 * theta) / 3 + sin(5 * theta) / 5);
}

template<typename T>
static void generate_samples(
    unsigned char* buffer,
    size_t length,
    unsigned long frequency,
    unsigned short channel_count,
    unsigned long samples_per_second,
    double* initial_angle,
    double (*generator)(double)
) {
    double increment = (frequency * M_PI * 2) / (double)samples_per_second;
    T* data = reinterpret_cast<T*>(buffer);
    double theta = initial_angle != nullptr ? *initial_angle : 0;

    for (size_t i = 0; i < length / sizeof(T); i += channel_count) {
        double value = generator(theta);

        for (size_t j = 0; j < channel_count; ++j) {
            if constexpr (std::is_same_v<T, float>) {
                data[i + j] = (float)value;
            }
            else if constexpr (std::is_same_v<T, short>) {
                data[i + j] = (short)(value * _I16_MAX);
            }
        }

        theta += increment;
    }

    if (initial_angle != nullptr) {
        *initial_angle = theta;
    }
}

int main()
{
    static plog::ColorConsoleAppender<plog::TxtFormatter> console_appender;
    plog::init(plog::verbose, &console_appender);

    EXIT_ON_ERROR(CoInitializeEx(nullptr, COINITBASE_MULTITHREADED));

    IMMDeviceEnumerator* enumerator = nullptr;
    EXIT_ON_ERROR(CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        0,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void* *)&enumerator)
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

    unsigned int buffer_size = 0;
    EXIT_ON_ERROR(client->GetBufferSize(&buffer_size));

    long long default_period;
    EXIT_ON_ERROR(client->GetDevicePeriod(&default_period, nullptr));

    unsigned int buffer_size_per_period = (default_period / 1e7) * format->nSamplesPerSec + 0.5;
    unsigned int buffer_size_bytes = buffer_size_per_period * format->nBlockAlign;
    size_t data_length = (format->nSamplesPerSec * duration * format->nBlockAlign) + (buffer_size_bytes - 1);
    size_t buffer_count = data_length / buffer_size_bytes;

    unsigned char* render_buffer = new (std::nothrow) unsigned char[buffer_count * buffer_size_bytes];
    if (render_buffer == nullptr) {
        PLOG_ERROR << "Unable to allocate render buffer";
        exit(EXIT_FAILURE);
    }

    double theta = 0;

    for (size_t i = 0; i < buffer_count; ++i) {
        switch (sample_type) {
            case RenderSampleType::Float:
                generate_samples<float>(
                    render_buffer + i * buffer_size_bytes,
                    buffer_size_bytes,
                    frequency,
                    format->nChannels,
                    format->nSamplesPerSec,
                    &theta,
                    sin
                );
                break;
            case RenderSampleType::PCM16bit:
                generate_samples<short>(
                    render_buffer + i * buffer_size_bytes,
                    buffer_size_bytes,
                    frequency,
                    format->nChannels,
                    format->nSamplesPerSec,
                    &theta,
                    square
                );
                break;
        }
    }

    EXIT_ON_ERROR(client->Start());

    IAudioRenderClient* render = nullptr;
    EXIT_ON_ERROR(client->GetService(__uuidof(IAudioRenderClient), (void**)&render));

    // One buffer's worth of silence to avoid glitches at the start
    {
        WaitForSingleObject(samples_ready_event, INFINITE);
        unsigned char* data;
        EXIT_ON_ERROR(render->GetBuffer(buffer_size, &data));
        EXIT_ON_ERROR(render->ReleaseBuffer(buffer_size, AUDCLNT_BUFFERFLAGS_SILENT));
    }

    size_t written_buffers = 0;
    unsigned char* data;
    unsigned int padding;
    unsigned int frames_available;


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
                unsigned int frames_to_write = buffer_size_bytes / format->nBlockAlign;

                EXIT_ON_ERROR(render->GetBuffer(frames_to_write, &data));
                memcpy(data, render_buffer + written_buffers * buffer_size_bytes, buffer_size_bytes);
                EXIT_ON_ERROR(render->ReleaseBuffer(frames_to_write, 0));

                written_buffers += 1;
            }
        break;
        }
    }

    // One buffer's worth of silence to avoid glitches at the end
    //{
    //    WaitForSingleObject(samples_ready_event, INFINITE);
    //    unsigned char* data;
    //    EXIT_ON_ERROR(client->GetCurrentPadding(&padding));
    //    EXIT_ON_ERROR(render->GetBuffer(buffer_size - padding, &data));
    //    EXIT_ON_ERROR(render->ReleaseBuffer(buffer_size, AUDCLNT_BUFFERFLAGS_SILENT));
    //}

    EXIT_ON_ERROR(client->Stop());
    CoUninitialize();

    CloseHandle(samples_ready_event);

    delete[] render_buffer;
    CoTaskMemFree(format);
    SAFE_RELEASE(enumerator);
    SAFE_RELEASE(device);
    SAFE_RELEASE(client);
    SAFE_RELEASE(render);
}
