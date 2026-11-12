/*
 * plugin_ops -- drive the Surge dsp.so the way the chain host does, from a
 * script of operations, so the plugin contract can be checked without a Move.
 *
 * Every plugin call (create, render, set, get, destroy) is made from ONE thread
 * at SCHED_FIFO 70 when the process may have it -- the host's SPI callback --
 * so a worker the plugin starts inherits what it would inherit on the device.
 *
 *   plugin_ops <dsp.so> <module_dir> op...
 *
 *   create:NAME            new instance, becomes current
 *   use:NAME               make NAME current
 *   destroy:NAME           destroy, print how long it took
 *   render:MS              render every live instance for MS ms of audio, in
 *                          128-frame blocks, as fast as possible; print the
 *                          slowest and mean block time per instance
 *   set:KEY=VALUE          set_param on the current instance
 *   get:KEY                print GET KEY=VALUE
 *   state:FILE[:CAP]       get_param("state") into FILE, print its size or -1;
 *                          CAP is the buffer size offered (default 65536)
 *   restore:FILE           set_param("state", FILE's contents)
 *   getkeys:KEYS:OUT       get_param every key listed in KEYS (one per line)
 *                          and write KEY<TAB>VALUE lines to OUT
 *   threads                print every thread: name, policy, priority, CPUs
 *   sleep:MS               wall-clock pause (rendering runs faster than real
 *                          time, so a worker needs this to catch up)
 *   note:on|off:NOTE:VEL   a note on channel 1
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <dlfcn.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>

extern "C" {
typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void *(*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;
}

static bool g_quiet_log = getenv("PLUGIN_OPS_LOG") == nullptr;
static void host_log(const char *msg) {
    if (!g_quiet_log || strstr(msg, "remote") || strstr(msg, "state") || strstr(msg, "ERROR"))
        fprintf(stderr, "[log] %s\n", msg);
}

static double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

struct Ctx {
    plugin_api_v2_t *api;
    const char *module_dir;
    std::vector<std::string> ops;
    int rc = 0;
};

static std::string read_file(const char *path) {
    std::string s;
    FILE *f = fopen(path, "rb");
    if (!f) return s;
    char b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

static void print_threads() {
#ifdef __linux__
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const pid_t tid = atoi(e->d_name);
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        std::string name = read_file(path);
        while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) name.pop_back();
        const int pol = sched_getscheduler(tid);
        sched_param sp{};
        sched_getparam(tid, &sp);
        cpu_set_t set;
        CPU_ZERO(&set);
        std::string cpus;
        if (sched_getaffinity(tid, sizeof(set), &set) == 0)
            for (int c = 0; c < CPU_SETSIZE && c < 64; c++)
                if (CPU_ISSET(c, &set)) cpus += (cpus.empty() ? "" : ",") + std::to_string(c);
        printf("THREAD %s policy=%s prio=%d cpus=%s\n", name.c_str(),
               pol == SCHED_FIFO ? "FIFO" : pol == SCHED_RR ? "RR" : pol == SCHED_OTHER ? "OTHER" : "?",
               sp.sched_priority, cpus.c_str());
    }
    closedir(d);
#endif
}

static void *run(void *arg) {
    Ctx *c = (Ctx *)arg;
    plugin_api_v2_t *api = c->api;
    std::map<std::string, void *> insts;
    std::vector<std::string> order;
    void *cur = nullptr;
    static char buf[1 << 17];
    int16_t audio[128 * 2];

    for (const std::string &op : c->ops) {
        const size_t colon = op.find(':');
        const std::string verb = op.substr(0, colon);
        const std::string arg = colon == std::string::npos ? "" : op.substr(colon + 1);
        if (verb == "create") {
            const double t0 = now_s();
            void *in = api->create_instance(c->module_dir, "{}");
            printf("CREATE %s %s %.3fs\n", arg.c_str(), in ? "ok" : "FAILED", now_s() - t0);
            if (!in) { c->rc = 1; return nullptr; }
            insts[arg] = in;
            order.push_back(arg);
            cur = in;
        } else if (verb == "use") {
            cur = insts[arg];
        } else if (verb == "destroy") {
            const double t0 = now_s();
            api->destroy_instance(insts[arg]);
            printf("DESTROY %s %.1fms\n", arg.c_str(), (now_s() - t0) * 1000);
            if (cur == insts[arg]) cur = nullptr;
            insts.erase(arg);
            order.erase(std::find(order.begin(), order.end(), arg));
        } else if (verb == "render") {
            const int blocks = (int)(atof(arg.c_str()) / 1000.0 * 44100 / 128 + 0.5);
            for (const std::string &n : order) {
                std::vector<double> t(blocks);
                double sum = 0;
                for (int b = 0; b < blocks; b++) {
                    const double t0 = now_s();
                    api->render_block(insts[n], audio, 128);
                    t[b] = now_s() - t0;
                    sum += t[b];
                }
                int worst_i = 0;
                for (int b = 1; b < blocks; b++) if (t[b] > t[worst_i]) worst_i = b;
                std::vector<double> s = t;
                std::sort(s.begin(), s.end());
                printf("RENDER %s blocks=%d mean=%.0fus p50=%.0fus p99=%.0fus worst=%.0fus (block %d)\n", n.c_str(),
                       blocks, blocks ? sum / blocks * 1e6 : 0.0, blocks ? s[blocks / 2] * 1e6 : 0.0,
                       blocks ? s[(size_t)(blocks * 0.99)] * 1e6 : 0.0, blocks ? s.back() * 1e6 : 0.0, worst_i);
            }
        } else if (verb == "set") {
            const size_t eq = arg.find('=');
            api->set_param(cur, arg.substr(0, eq).c_str(), arg.substr(eq + 1).c_str());
        } else if (verb == "get") {
            const int n = api->get_param(cur, arg.c_str(), buf, sizeof(buf));
            printf("GET %s=%s\n", arg.c_str(), n >= 0 ? buf : "<-1>");
        } else if (verb == "state") {
            /* the host's own buffer is the 64 KB parameter value; ask with
             * that unless FILE:CAP names a smaller one */
            std::string file = arg;
            int cap = 65536;
            const size_t c2 = arg.rfind(':');
            if (c2 != std::string::npos) { cap = atoi(arg.c_str() + c2 + 1); file = arg.substr(0, c2); }
            const int n = api->get_param(cur, "state", buf, cap);
            printf("STATE %s %d\n", file.c_str(), n);
            FILE *f = fopen(file.c_str(), "wb");
            if (f) { if (n > 0) fwrite(buf, 1, (size_t)n, f); fclose(f); }
        } else if (verb == "restore") {
            const std::string s = read_file(arg.c_str());
            const double t0 = now_s();
            api->set_param(cur, "state", s.c_str());
            printf("RESTORE %s %zu bytes %.1fms\n", arg.c_str(), s.size(), (now_s() - t0) * 1000);
        } else if (verb == "getkeys") {
            const size_t c2 = arg.find(':');
            const std::string keys = read_file(arg.substr(0, c2).c_str());
            FILE *f = fopen(arg.substr(c2 + 1).c_str(), "w");
            size_t pos = 0;
            int count = 0;
            while (pos < keys.size()) {
                size_t nl = keys.find('\n', pos);
                if (nl == std::string::npos) nl = keys.size();
                const std::string k = keys.substr(pos, nl - pos);
                pos = nl + 1;
                if (k.empty()) continue;
                const int n = api->get_param(cur, k.c_str(), buf, sizeof(buf));
                fprintf(f, "%s\t%s\n", k.c_str(), n >= 0 ? buf : "<-1>");
                count++;
            }
            fclose(f);
            printf("GETKEYS %d\n", count);
        } else if (verb == "threads") {
            print_threads();
        } else if (verb == "sleep") {
            /* wall time, for a worker to catch up: render runs faster than real time */
            usleep((useconds_t)(atof(arg.c_str()) * 1000));
        } else if (verb == "note") {
            const bool on = arg.rfind("on:", 0) == 0;
            int note = 60, vel = 100;
            sscanf(arg.c_str() + (on ? 3 : 4), "%d:%d", &note, &vel);
            uint8_t m[3] = {(uint8_t)(on ? 0x90 : 0x80), (uint8_t)note, (uint8_t)vel};
            api->on_midi(cur, m, 3, 0);
        } else {
            fprintf(stderr, "unknown op %s\n", op.c_str());
            c->rc = 2;
        }
        fflush(stdout);
    }
    for (const std::string &n : order) api->destroy_instance(insts[n]);
    return nullptr;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: plugin_ops <dsp.so> <module_dir> op...\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto init = (plugin_api_v2_t * (*)(const host_api_v1_t *)) dlsym(h, "move_plugin_init_v2");
    if (!init) { fprintf(stderr, "no move_plugin_init_v2\n"); return 1; }
    static host_api_v1_t host{};
    host.api_version = 1;
    host.sample_rate = 44100;
    host.frames_per_block = 128;
    host.log = host_log;
    Ctx c;
    c.api = init(&host);
    c.module_dir = argv[2];
    for (int i = 3; i < argc; i++) c.ops.push_back(argv[i]);

    /* the host's callback: SCHED_FIFO 70 if we may, else say so */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    sched_param sp{};
    sp.sched_priority = 70;
    pthread_attr_setschedparam(&attr, &sp);
    pthread_t th;
    if (pthread_create(&th, &attr, run, &c) != 0) {
        printf("CALLER SCHED_OTHER (SCHED_FIFO 70 not permitted here)\n");
        pthread_attr_destroy(&attr);
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8 << 20);
        pthread_create(&th, &attr, run, &c);
    } else {
        printf("CALLER SCHED_FIFO 70\n");
    }
    pthread_join(th, nullptr);
    pthread_attr_destroy(&attr);
    return c.rc;
}
