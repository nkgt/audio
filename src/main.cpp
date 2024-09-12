#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmeapi.h>
#include <mmreg.h>

#define _USE_MATH_DEFINES
#include <math.h>

#define PLOG_ENABLE_WCHAR_INPUT 1
#include <plog/Log.h>
#include <plog/Init.h>
#include <plog/Formatters/TxtFormatter.h>
#include <plog/Appenders/ColorConsoleAppender.h>

#define SAFE_RELEASE(x)   \
    if ((x) != nullptr) { \
        (x)->Release();   \
        (x) = nullptr;    \
    }

enum class RenderSampleType {
    Float,
    PCM16bit
};

struct RenderBuffer {
    RenderBuffer* next = nullptr;
    unsigned int length = 0;
    unsigned char* buffer = nullptr;

    ~RenderBuffer() {
        if (buffer != nullptr) delete[] buffer;
    }
};

constexpr int frequency = 440;
constexpr int latency = 50;
constexpr int duration = 5;

static void exit_on_error(HRESULT result)
{
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

    if (FAILED(result)) {
        wchar_t buf[256] {};

        FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            result,
            locale_id,
            (wchar_t*)&buf,
            sizeof(buf) / sizeof(buf[0]),
            nullptr
        );

        PLOG_ERROR << buf;
        exit(EXIT_FAILURE);
    }
}

template<typename T> inline T convert(double Value);

template<>
inline float convert<float>(double value) {
    return (float)value;
}

template<>
inline short convert<short>(double value) {
    return (short(value * _I16_MAX));
}

template<typename T>
static void generate_sin_samples(
    unsigned char* buffer,
    size_t length,
    unsigned long frequency,
    unsigned short channel_count,
    unsigned long samples_per_second,
    double* initial_angle
) {
    double increment = (frequency * (M_PI * 2)) / (double)samples_per_second;
    T* data = reinterpret_cast<T *>(buffer);
    double theta = initial_angle != nullptr ? *initial_angle : 0;

    for (size_t i = 0; i < length / sizeof(T); i += channel_count) {
        double value = sin(theta);
        
        for (size_t j = 0; j < channel_count; ++j) {
            data[i + j] = convert<T>(value);
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

    exit_on_error(CoInitializeEx(nullptr, COINITBASE_MULTITHREADED));

    IMMDeviceEnumerator* enumerator = nullptr;
    exit_on_error(CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        0,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void* *)&enumerator)
    );

    IMMDevice* device = nullptr;
    exit_on_error(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));

    IAudioClient* client = nullptr;
    exit_on_error(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void* *)&client));

    WAVEFORMATEX* format = nullptr;
    exit_on_error(client->GetMixFormat(&format));

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

    exit_on_error(client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_NOPERSIST,
        latency * 10000,
        0,
        format,
        nullptr
    ));

    unsigned int buffer_size = 0;
    exit_on_error(client->GetBufferSize(&buffer_size));

    unsigned int buffer_size_bytes = (buffer_size / 4) * format->nBlockAlign;
    size_t data_length = (format->nSamplesPerSec * duration * format->nBlockAlign) + (buffer_size_bytes - 1);
    size_t buffer_count = data_length / buffer_size_bytes;

    RenderBuffer* render_queue = nullptr;
    RenderBuffer** render_queue_tail = &render_queue;

    double theta = 0;

    for (size_t i = 0; i < buffer_count; ++i) {
        RenderBuffer* buffer = new RenderBuffer();
        if (buffer == nullptr) {
            PLOG_ERROR << "Unable to allocate RenderBuffer";
            exit(EXIT_FAILURE);
        }

        buffer->length = buffer_size_bytes;
        buffer->buffer = new unsigned char[buffer_size_bytes];
        if (buffer->buffer == nullptr) {
            PLOG_ERROR << "Unable to allocate RenderBuffer buffer";
            exit(EXIT_FAILURE);
        }

        switch (sample_type) {
            case RenderSampleType::Float:
                generate_sin_samples<float>(
                    buffer->buffer,
                    buffer->length,
                    frequency,
                    format->nChannels,
                    format->nSamplesPerSec,
                    &theta
            );
                break;
            case RenderSampleType::PCM16bit:
                generate_sin_samples<short>(
                    buffer->buffer,
                    buffer->length,
                    frequency,
                    format->nChannels,
                    format->nSamplesPerSec,
                    &theta
            );
                break;
        }

        *render_queue_tail = buffer;
        render_queue_tail = &buffer->next;
    }

    IAudioRenderClient* render = nullptr;
    exit_on_error(client->GetService(__uuidof(IAudioRenderClient), (void* *)&render));

    // One buffer's worth of silence to avoid glitches at the start
    {
        unsigned char* data;
        exit_on_error(render->GetBuffer(buffer_size, &data));
        exit_on_error(render->ReleaseBuffer(buffer_size, AUDCLNT_BUFFERFLAGS_SILENT));
    }

    bool still_playing = true;

    exit_on_error(client->Start());

    while (still_playing) {
        Sleep(latency / 2);

        if (render_queue == nullptr) {
            still_playing = false;
        }
        else {
            unsigned char* data;
            unsigned int padding;
            unsigned int frames_available;

            exit_on_error(client->GetCurrentPadding(&padding));

            frames_available = buffer_size - padding;

            while (render_queue != nullptr && (render_queue->length <= frames_available * format->nBlockAlign)) {
                RenderBuffer* buffer = render_queue;
                render_queue = buffer->next;

                unsigned int frames_to_write = buffer->length / format->nBlockAlign;

                exit_on_error(render->GetBuffer(frames_to_write, &data));

                CopyMemory(data, buffer->buffer, frames_to_write * format->nBlockAlign);
                exit_on_error(render->ReleaseBuffer(frames_to_write, 0));

                delete buffer;

                exit_on_error(client->GetCurrentPadding(&padding));
                frames_available = buffer_size - padding;
            }
        }
    }

    exit_on_error(client->Stop());
    CoUninitialize();

    CoTaskMemFree(format);
    SAFE_RELEASE(enumerator);
    SAFE_RELEASE(device);
    SAFE_RELEASE(client);
    SAFE_RELEASE(render);
}
