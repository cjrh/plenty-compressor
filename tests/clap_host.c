#define _GNU_SOURCE
#include <clap/clap.h>
#include <stddef.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "CLAP test failed at %s:%d: %s\n", __FILE__, __LINE__, #test); \
    abort(); \
} } while (0)
/* Keep the manually declared Plenty ABI tied to the pinned native headers. */
_Static_assert(sizeof(clap_plugin_entry_t) == 40, "entry layout");
_Static_assert(sizeof(clap_plugin_t) == 96, "plugin layout");
_Static_assert(sizeof(clap_plugin_descriptor_t) == 88, "descriptor layout");
_Static_assert(offsetof(clap_host_t, get_extension) == 56, "host callback layout");
_Static_assert(offsetof(clap_process_t, in_events) == 48, "process layout");
_Static_assert(offsetof(clap_audio_buffer_t, constant_mask) == 24, "audio layout");
_Static_assert(sizeof(clap_event_param_value_t) == 56, "parameter event layout");
_Static_assert(offsetof(clap_event_param_value_t, value) == 48, "event value layout");
_Static_assert(offsetof(clap_param_info_t, min_value) == 1296, "parameter info layout");
_Static_assert(offsetof(clap_audio_port_info_t, port_type) == 272, "port layout");

#define FRAMES 32
#define STATE_CAPACITY 4096

static pthread_t main_thread;
static atomic_uint rescans;
static atomic_uint host_queries;
static const double defaults[4] = {-18.0, 4.0, 10.0, 100.0};
static const double lows[4] = {-60.0, 1.0, 0.1, 1.0};
static const double highs[4] = {0.0, 20.0, 200.0, 2000.0};

static void CLAP_ABI rescan(const clap_host_t *host, clap_param_rescan_flags flags) {
    (void)host;
    CHECK(pthread_equal(pthread_self(), main_thread));
    CHECK(flags & CLAP_PARAM_RESCAN_VALUES);
    atomic_fetch_add(&rescans, 1);
}
static void CLAP_ABI clear(const clap_host_t *host, clap_id id, clap_param_clear_flags flags) {
    (void)host; (void)id; (void)flags;
    CHECK(pthread_equal(pthread_self(), main_thread));
}
static void CLAP_ABI request_flush(const clap_host_t *host) { (void)host; }
static const clap_host_params_t host_params = {rescan, clear, request_flush};
static const void *CLAP_ABI host_extension(const clap_host_t *host, const char *id) {
    (void)host;
    atomic_fetch_add(&host_queries, 1);
    return !strcmp(id, CLAP_EXT_PARAMS) ? &host_params : NULL;
}
static void CLAP_ABI request(const clap_host_t *host) { (void)host; }
static const clap_host_t host = {
    CLAP_VERSION_INIT, NULL, "Plenty native test host", "Plenty", "", "1",
    host_extension, request, request, request
};
static const void *CLAP_ABI no_extensions(const clap_host_t *h, const char *id) {
    (void)h; (void)id; return NULL;
}

struct events { clap_event_param_value_t values[8]; uint32_t count; };
static uint32_t CLAP_ABI event_count(const clap_input_events_t *input) {
    return ((const struct events *)input->ctx)->count;
}
static const clap_event_header_t *CLAP_ABI event_get(const clap_input_events_t *input, uint32_t i) {
    const struct events *events = input->ctx;
    CHECK(i < events->count);
    return &events->values[i].header;
}
static bool CLAP_ABI event_push(const clap_output_events_t *output, const clap_event_header_t *event) {
    (void)output;
    CHECK(event && event->size >= sizeof(*event));
    /* No GUI-originated events are required; exercise a full output queue. */
    return false;
}
static clap_event_param_value_t parameter(clap_id id, double value, uint32_t time) {
    clap_event_param_value_t event = {0};
    event.header.size = sizeof(event);
    event.header.time = time;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = CLAP_EVENT_PARAM_VALUE;
    event.param_id = id;
    event.note_id = -1; event.port_index = -1; event.channel = -1; event.key = -1;
    event.value = value;
    return event;
}
static void set_controls(const clap_plugin_t *plugin, const clap_plugin_params_t *params,
                         const double values[4]) {
    struct events events = {.count = 4};
    for (uint32_t i = 0; i < 4; ++i) events.values[i] = parameter(i, values[i], 0);
    const clap_input_events_t input = {&events, event_count, event_get};
    const clap_output_events_t output = {NULL, event_push};
    params->flush(plugin, &input, &output);
}
static void expect_controls(const clap_plugin_t *plugin, const clap_plugin_params_t *params,
                            const double values[4]) {
    for (uint32_t i = 0; i < 4; ++i) {
        double value = NAN;
        CHECK(params->get_value(plugin, i, &value));
        CHECK(value == values[i]);
    }
}
struct audio {
    float left[FRAMES], right[FRAMES], out_left[FRAMES], out_right[FRAMES];
    float *inputs[2], *outputs[2];
    clap_audio_buffer_t input, output;
};
static void prepare_audio(struct audio *audio) {
    memset(audio, 0, sizeof(*audio));
    audio->inputs[0] = audio->left; audio->inputs[1] = audio->right;
    audio->outputs[0] = audio->out_left; audio->outputs[1] = audio->out_right;
    audio->input.data32 = audio->inputs; audio->input.channel_count = 2;
    audio->output.data32 = audio->outputs; audio->output.channel_count = 2;
    for (unsigned i = 0; i < FRAMES; ++i) {
        audio->left[i] = 0.8f; audio->right[i] = -0.4f;
    }
}
static clap_process_status process(const clap_plugin_t *plugin, struct audio *audio,
                                   struct events *events, int64_t time) {
    const clap_input_events_t input_events = {events, event_count, event_get};
    const clap_output_events_t output_events = {NULL, event_push};
    const clap_process_t block = {
        time, FRAMES, NULL, &audio->input, &audio->output, 1, 1,
        &input_events, &output_events
    };
    return plugin->process(plugin, &block);
}
static void audio_tests(const clap_plugin_t *plugin, const clap_plugin_params_t *params) {
    const double controls[4] = {-60.0, 1.0, 0.1, 100.0};
    set_controls(plugin, params, controls);
    CHECK(plugin->activate(plugin, 48000.0, 1, FRAMES));
    CHECK(plugin->start_processing(plugin));
    struct audio audio;
    prepare_audio(&audio);
    struct events events = {.count = 1};
    events.values[0] = parameter(1, 20.0, 8);
    CHECK(process(plugin, &audio, &events, 0) != CLAP_PROCESS_ERROR);
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(fabsf(audio.out_left[i] - audio.left[i]) < 0.000001f);
        CHECK(fabsf(audio.out_right[i] - audio.right[i]) < 0.000001f);
    }
    CHECK(audio.out_left[8] > 0.0f && audio.out_left[8] < 0.08f);
    CHECK(fabsf(audio.out_right[8] / audio.out_left[8] + 0.5f) < 0.000001f);
    double ratio;
    CHECK(params->get_value(plugin, 1, &ratio) && ratio == 20.0);
    /* Same-time events retain list order; reset preserves parameter values. */
    plugin->reset(plugin);
    CHECK(params->get_value(plugin, 1, &ratio) && ratio == 20.0);
    events.count = 2;
    events.values[0] = parameter(1, 20.0, 0);
    events.values[1] = parameter(1, 1.0, 0);
    prepare_audio(&audio);
    CHECK(process(plugin, &audio, &events, FRAMES) != CLAP_PROCESS_ERROR);
    for (unsigned i = 0; i < FRAMES; ++i) CHECK(audio.out_left[i] == audio.left[i]);
    /* Ignore unknown namespaces/types, undersized values and non-global targets. */
    events.count = 7;
    for (unsigned i = 0; i < events.count; ++i) events.values[i] = parameter(1, 20.0, 0);
    events.values[0].header.space_id = 99;
    events.values[1].header.type = CLAP_EVENT_PARAM_MOD;
    events.values[2].header.size = sizeof(clap_event_header_t);
    events.values[3].note_id = 4;
    events.values[4].param_id = CLAP_INVALID_ID;
    events.values[5].value = NAN;
    events.values[6].value = INFINITY;
    prepare_audio(&audio);
    CHECK(process(plugin, &audio, &events, 2 * FRAMES) != CLAP_PROCESS_ERROR);
    CHECK(params->get_value(plugin, 1, &ratio) && ratio == 1.0);
    for (unsigned i = 0; i < FRAMES; ++i) CHECK(audio.out_left[i] == audio.left[i]);
    /* Active flush runs on the audio thread between process calls. */
    set_controls(plugin, params, controls);
    /* Exact and crossed aliases retain both original channel samples. */
    events.count = 0;
    for (int cross = 0; cross < 2; ++cross) {
        prepare_audio(&audio);
        audio.outputs[0] = cross ? audio.right : audio.left;
        audio.outputs[1] = cross ? audio.left : audio.right;
        CHECK(process(plugin, &audio, &events, (2 + cross) * FRAMES) != CLAP_PROCESS_ERROR);
        for (unsigned i = 0; i < FRAMES; ++i) {
            CHECK(audio.outputs[0][i] == 0.8f);
            CHECK(audio.outputs[1][i] == -0.4f);
        }
    }
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
}

struct stream {
    unsigned char bytes[STATE_CAPACITY];
    uint64_t size, position, chunk, fail_at;
    bool zero_progress;
};
static int64_t CLAP_ABI stream_write(const clap_ostream_t *output, const void *buffer, uint64_t size) {
    struct stream *stream = output->ctx;
    CHECK(pthread_equal(pthread_self(), main_thread));
    if (stream->zero_progress) return 0;
    if (stream->position >= stream->fail_at) return -1;
    uint64_t count = size < stream->chunk ? size : stream->chunk;
    if (count > stream->fail_at - stream->position) count = stream->fail_at - stream->position;
    CHECK(count <= STATE_CAPACITY - stream->position);
    memcpy(stream->bytes + stream->position, buffer, (size_t)count);
    stream->position += count;
    stream->size = stream->position;
    return (int64_t)count;
}
static int64_t CLAP_ABI stream_read(const clap_istream_t *input, void *buffer, uint64_t size) {
    struct stream *stream = input->ctx;
    CHECK(pthread_equal(pthread_self(), main_thread));
    if (stream->position >= stream->fail_at) return -1;
    uint64_t count = size < stream->chunk ? size : stream->chunk;
    if (count > stream->size - stream->position) count = stream->size - stream->position;
    if (count > stream->fail_at - stream->position) count = stream->fail_at - stream->position;
    memcpy(buffer, stream->bytes + stream->position, (size_t)count);
    stream->position += count;
    return (int64_t)count;
}
static struct stream empty_stream(void) {
    struct stream stream = {.chunk = 3, .fail_at = UINT64_MAX};
    return stream;
}
static bool save(const clap_plugin_t *plugin, const clap_plugin_state_t *state, struct stream *stream) {
    const clap_ostream_t output = {stream, stream_write};
    return state->save(plugin, &output);
}
static bool load(const clap_plugin_t *plugin, const clap_plugin_state_t *state, struct stream *stream) {
    const clap_istream_t input = {stream, stream_read};
    stream->position = 0;
    return state->load(plugin, &input);
}
static void state_tests(const clap_plugin_t *plugin, const clap_plugin_params_t *params,
                        const clap_plugin_state_t *state) {
    set_controls(plugin, params, defaults);
    struct stream original = empty_stream();
    CHECK(save(plugin, state, &original));
    CHECK(original.size > 0 && original.size < STATE_CAPACITY);
    const double changed[4] = {-30.0, 7.0, 13.0, 250.0};
    set_controls(plugin, params, changed);
    struct stream broken = original;
    broken.size--;
    CHECK(!load(plugin, state, &broken));
    expect_controls(plugin, params, changed);
    broken = original; broken.fail_at = original.size / 2;
    CHECK(!load(plugin, state, &broken));
    expect_controls(plugin, params, changed);
    broken = original;
    memset(broken.bytes, 0xff, (size_t)broken.size);
    CHECK(!load(plugin, state, &broken));
    expect_controls(plugin, params, changed);
    unsigned before = atomic_load(&rescans);
    CHECK(load(plugin, state, &original));
    CHECK(atomic_load(&rescans) > before);
    expect_controls(plugin, params, defaults);
    struct stream restored = empty_stream();
    CHECK(save(plugin, state, &restored));
    CHECK(restored.size == original.size);
    CHECK(!memcmp(restored.bytes, original.bytes, (size_t)original.size));
    broken = empty_stream(); broken.fail_at = 1;
    CHECK(!save(plugin, state, &broken));
    broken = empty_stream(); broken.zero_progress = true;
    CHECK(!save(plugin, state, &broken));
}
struct concurrent {
    const clap_plugin_t *plugin;
    atomic_bool started, done;
    atomic_uint queried;
};
static void *audio_worker(void *data) {
    struct concurrent *concurrent = data;
    CHECK(concurrent->plugin->start_processing(concurrent->plugin));
    atomic_store(&concurrent->started, true);
    unsigned round = 0;
    do {
        struct audio audio;
        prepare_audio(&audio);
        struct events events = {.count = 1};
        events.values[0] = parameter(1, (round & 1) ? 2.0 : 8.0, 0);
        CHECK(process(concurrent->plugin, &audio, &events, (int64_t)round * FRAMES) != CLAP_PROCESS_ERROR);
        for (unsigned i = 0; i < FRAMES; ++i) {
            CHECK(isfinite(audio.out_left[i]) && isfinite(audio.out_right[i]));
        }
        ++round;
    } while (round < 100 || atomic_load(&concurrent->queried) < 30);
    concurrent->plugin->stop_processing(concurrent->plugin);
    atomic_store(&concurrent->done, true);
    return NULL;
}
static void concurrency_tests(const clap_plugin_t *plugin, const clap_plugin_params_t *params,
                              const clap_plugin_state_t *state) {
    CHECK(plugin->activate(plugin, 48000.0, 1, FRAMES));
    struct concurrent concurrent = {.plugin = plugin};
    pthread_t worker;
    CHECK(!pthread_create(&worker, NULL, audio_worker, &concurrent));
    while (!atomic_load(&concurrent.started)) sched_yield();
    do {
        for (uint32_t i = 0; i < 4; ++i) {
            double value = NAN;
            CHECK(params->get_value(plugin, i, &value));
            CHECK(isfinite(value) && value >= lows[i] && value <= highs[i]);
        }
        struct stream snapshot = empty_stream();
        CHECK(save(plugin, state, &snapshot));
        /* State callbacks are main-thread operations even while audio runs. */
        CHECK(load(plugin, state, &snapshot));
        atomic_fetch_add(&concurrent.queried, 1);
    } while (!atomic_load(&concurrent.done));
    CHECK(!pthread_join(worker, NULL));
    /* A new OS thread may become this instance's audio thread after handoff. */
    atomic_store(&concurrent.started, false);
    atomic_store(&concurrent.done, false);
    CHECK(!pthread_create(&worker, NULL, audio_worker, &concurrent));
    CHECK(!pthread_join(worker, NULL));
    plugin->deactivate(plugin);
}
static void metadata_tests(const clap_plugin_t *plugin, const clap_plugin_params_t *params) {
    CHECK(params->count(plugin) == 4);
    for (uint32_t i = 0; i < 4; ++i) {
        clap_param_info_t info;
        memset(&info, 0xa5, sizeof(info));
        CHECK(params->get_info(plugin, i, &info));
        CHECK(info.id == i && info.flags & CLAP_PARAM_IS_AUTOMATABLE);
        CHECK(info.min_value == lows[i] && info.max_value == highs[i]);
        CHECK(info.default_value == defaults[i]);
        CHECK(info.name[0] && memchr(info.name, 0, sizeof(info.name)));
        CHECK(memchr(info.module, 0, sizeof(info.module)));
        char text[128]; memset(text, 0xa5, sizeof(text));
        CHECK(params->value_to_text(plugin, i, defaults[i], text, sizeof(text)));
        CHECK(memchr(text, 0, sizeof(text)));
        double parsed = NAN;
        CHECK(params->text_to_value(plugin, i, text, &parsed));
        CHECK(fabs(parsed - defaults[i]) < 0.000001);
        struct { unsigned char before; char text[1]; unsigned char after; } tiny = {0x12, {0}, 0x34};
        CHECK(!params->value_to_text(plugin, i, defaults[i], tiny.text, 1));
        CHECK(tiny.before == 0x12 && tiny.after == 0x34);
    }
    /* Rounded decimal formatting respects the precise NUL-inclusive capacity. */
    const struct { clap_id id; double value; const char *text; } formats[] = {
        {1, 9.9999, "10.000"}, {2, 0.1, "0.100"}, {0, -0.0001, "-0.000"}
    };
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        unsigned char guarded[32];
        memset(guarded, 0xa5, sizeof(guarded));
        uint32_t capacity = (uint32_t)strlen(formats[i].text) + 1;
        CHECK(params->value_to_text(plugin, formats[i].id, formats[i].value,
                                    (char *)&guarded[1], capacity));
        CHECK(!strcmp((char *)&guarded[1], formats[i].text));
        CHECK(guarded[0] == 0xa5 && guarded[capacity + 1] == 0xa5);
        double parsed = NAN;
        CHECK(params->text_to_value(plugin, formats[i].id, (char *)&guarded[1], &parsed));
        CHECK(fabs(parsed - formats[i].value) <= 0.0005);
        memset(guarded, 0xa5, sizeof(guarded));
        CHECK(!params->value_to_text(plugin, formats[i].id, formats[i].value,
                                     (char *)&guarded[1], capacity - 1));
        CHECK(guarded[0] == 0xa5 && guarded[capacity] == 0xa5);
        memset(guarded, 0xa5, sizeof(guarded));
        CHECK(!params->value_to_text(plugin, formats[i].id, formats[i].value,
                                     (char *)&guarded[1], 0));
        for (unsigned j = 0; j < sizeof(guarded); ++j) CHECK(guarded[j] == 0xa5);
    }
    clap_param_info_t info;
    double value;
    CHECK(!params->get_info(plugin, 4, &info));
    CHECK(!params->get_value(plugin, CLAP_INVALID_ID, &value));
    const clap_plugin_audio_ports_t *ports = plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS);
    CHECK(ports && ports->count(plugin, true) == 1 && ports->count(plugin, false) == 1);
    clap_audio_port_info_t input, output;
    CHECK(ports->get(plugin, 0, true, &input));
    CHECK(ports->get(plugin, 0, false, &output));
    CHECK(input.channel_count == 2 && output.channel_count == 2);
    CHECK(input.flags & CLAP_AUDIO_PORT_IS_MAIN);
    CHECK(output.flags & CLAP_AUDIO_PORT_IS_MAIN);
    CHECK(!(input.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS));
    CHECK(!(output.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS));
    CHECK(input.port_type && !strcmp(input.port_type, CLAP_PORT_STEREO));
    CHECK(output.port_type && !strcmp(output.port_type, CLAP_PORT_STEREO));
    CHECK(input.in_place_pair == output.id && output.in_place_pair == input.id);
    CHECK(!ports->get(plugin, 1, true, &input));
}
static bool mapped(const char *path) {
    FILE *maps = fopen("/proc/self/maps", "r"); CHECK(maps);
    char line[8192]; bool found = false;
    while (fgets(line, sizeof(line), maps)) if (strstr(line, path)) found = true;
    CHECK(!ferror(maps)); CHECK(!fclose(maps)); return found;
}
/* Remain alive after using Plenty, but retain no live plugin reference. This
 * detects library TLS destructors that pin mappings or jump into unloaded code. */
struct persistent_worker {
    const clap_plugin_t *plugin;
    atomic_bool quiescent, release;
};
static void *persistent_audio(void *data) {
    struct persistent_worker *worker = data;
    CHECK(worker->plugin->start_processing(worker->plugin));
    struct audio audio;
    prepare_audio(&audio);
    struct events events = {.count = 0};
    CHECK(process(worker->plugin, &audio, &events, 0) != CLAP_PROCESS_ERROR);
    worker->plugin->stop_processing(worker->plugin);
    worker->plugin = NULL;
    atomic_store(&worker->quiescent, true);
    while (!atomic_load(&worker->release)) sched_yield();
    return NULL;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    alarm(60); /* A zero-progress stream must not hang a test indefinitely. */
    main_thread = pthread_self();
    char *path = realpath(argv[1], NULL); CHECK(path);
    for (int cycle = 0; cycle < 3; ++cycle) {
        void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!library) fprintf(stderr, "%s\n", dlerror());
        CHECK(library && mapped(path));
        const clap_plugin_entry_t *entry = dlsym(library, "clap_entry");
        CHECK(entry && clap_version_is_compatible(entry->clap_version));
        CHECK(entry->init(path)); CHECK(entry->init(path));
        CHECK(!entry->get_factory("unknown.factory"));
        const clap_plugin_factory_t *factory = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
        CHECK(factory && factory->get_plugin_count(factory) == 1);
        const clap_plugin_descriptor_t *descriptor = factory->get_plugin_descriptor(factory, 0);
        CHECK(descriptor && !strcmp(descriptor->id, "org.plenty.compressor"));
        CHECK(descriptor->name && descriptor->name[0]);
        CHECK(descriptor->features);
        size_t feature = 0;
        while (feature < 16 && descriptor->features[feature]) ++feature;
        CHECK(feature < 16);
        CHECK(!factory->get_plugin_descriptor(factory, 1));
        CHECK(!factory->create_plugin(factory, &host, "unknown.plugin"));
        unsigned queries = atomic_load(&host_queries);
        const clap_plugin_t *plugin = factory->create_plugin(factory, &host, descriptor->id);
        CHECK(plugin && atomic_load(&host_queries) == queries);
        CHECK(plugin->init(plugin));
        CHECK(plugin->desc == descriptor);
        CHECK(!plugin->get_extension(plugin, "unknown.extension"));
        const clap_plugin_params_t *params = plugin->get_extension(plugin, CLAP_EXT_PARAMS);
        const clap_plugin_state_t *state = plugin->get_extension(plugin, CLAP_EXT_STATE);
        CHECK(params && state);
        metadata_tests(plugin, params);
        expect_controls(plugin, params, defaults);
        state_tests(plugin, params, state);
        audio_tests(plugin, params);
        concurrency_tests(plugin, params, state);
        clap_host_t minimal = host; minimal.get_extension = no_extensions;
        const clap_plugin_t *second = factory->create_plugin(factory, &minimal, descriptor->id);
        CHECK(second && second != plugin && second->init(second));
        const clap_plugin_params_t *second_params = second->get_extension(second, CLAP_EXT_PARAMS);
        expect_controls(second, second_params, defaults);
        const double independent[4] = {-42.0, 3.0, 4.0, 300.0};
        set_controls(second, second_params, independent);
        double first;
        CHECK(params->get_value(plugin, 0, &first) && first != independent[0]);
        second->destroy(second);
        CHECK(plugin->activate(plugin, 48000.0, 1, FRAMES));
        struct persistent_worker persistent = {.plugin = plugin};
        pthread_t resident;
        CHECK(!pthread_create(&resident, NULL, persistent_audio, &persistent));
        while (!atomic_load(&persistent.quiescent)) sched_yield();
        CHECK(!persistent.plugin);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
        entry->deinit(); entry->deinit();
        CHECK(!dlclose(library));
        CHECK(!dlopen(path, RTLD_NOW | RTLD_NOLOAD));
        CHECK(!mapped(path));
        atomic_store(&persistent.release, true);
        CHECK(!pthread_join(resident, NULL));
    }
    free(path);
    puts("CLAP host: lifecycle, metadata, automation, aliases, state, concurrency and unload passed");
    return 0;
}
