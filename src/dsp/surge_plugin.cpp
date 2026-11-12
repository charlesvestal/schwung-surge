/*
 * Surge XT DSP Plugin for Move Anything
 *
 * Hybrid synthesizer based on Surge XT by the Surge Synth Team.
 * GPL-3.0 License - see LICENSE file.
 *
 * https://github.com/surge-synthesizer/surge
 *
 * V2 API only - instance-based for multi-instance support
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include <string>
#include <algorithm>
#include <cctype>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <optional>
#include <atomic>
#include <cstdarg>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

/* Plugin API definitions */
extern "C" {
#include <stdint.h>
#include <time.h>

#define MOVE_PLUGIN_API_VERSION 1
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128
#define MOVE_MIDI_SOURCE_INTERNAL 0
#define MOVE_MIDI_SOURCE_EXTERNAL 2

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

#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

typedef plugin_api_v2_t* (*move_plugin_init_v2_fn)(const host_api_v1_t *host);
#define MOVE_PLUGIN_INIT_V2_SYMBOL "move_plugin_init_v2"
}

/* Surge XT engine */
#include "SurgeSynthesizer.h"
#include "SurgeStorage.h"
#include "Parameter.h"
#include "Oscillator.h"
#include "sst/filters.h"
#include <sst/filters/HalfRateFilter.h>

/* Surge block size is 32, Move block size is 128, so we call process() 4 times */
static_assert(MOVE_FRAMES_PER_BLOCK % BLOCK_SIZE == 0,
    "Move block size must be a multiple of Surge block size");
#define SURGE_CALLS_PER_MOVE_BLOCK (MOVE_FRAMES_PER_BLOCK / BLOCK_SIZE)

/* Host API reference */
static const host_api_v1_t *g_host = nullptr;

/* =====================================================================
 * PluginLayer stub (required by SurgeSynthesizer)
 * ===================================================================== */

class MovePluginLayer : public SurgeSynthesizer::PluginLayer {
public:
    void surgeParameterUpdated(const SurgeSynthesizer::ID &, float) override {}
    void surgeMacroUpdated(long, float) override {}
};

/* =====================================================================
 * Parameter registry - maps string keys to Surge parameter IDs
 * ===================================================================== */

/* Every Surge parameter: 273 per scene x 2 scenes + 219 global (11 + 16 FX
 * slots x 13). Scene A comes FIRST in the registry, as it always did, so the
 * mod-slot destination indices in saved states keep meaning what they meant.
 *
 * THREADS. Every plugin API call (create, destroy, set_param, get_param,
 * on_midi, render_block) arrives on the host's SPI audio callback, one at a
 * time; so does everything in this file that reads or writes Surge or the
 * registry. The registry is built once in create_instance and its identity
 * fields (key, surge_id, scene, param_id_in_scene, group) never change after
 * that; the type-dependent fields (valtype, range, ctrltype, options) are
 * refreshed on the audio thread as Surge re-types a parameter. The Remote UI's
 * worker thread reads none of it -- see "Remote UI files" below. */
#define MAX_SURGE_PARAMS 1024

/* The chain host keeps a synth's state in a 16384-byte field
 * (MAX_SYNTH_STATE_LEN, schwung chain_internal.h) and DROPS a state that does
 * not fit, so a set reload would lose every edit. State is therefore written as
 * what differs from the loaded preset, and refused rather than truncated past
 * this size; the margin under 16384 is for the terminator and the host's copy. */
#define SURGE_STATE_MAX 16000

struct surge_param_entry {
    char key[48];             /* Parameter key, e.g. "osc1_pitch", "b_osc1_pitch", "fx0_p3", "g_volume" */
    char display_name[48];    /* Display name, e.g. "Osc 1 Pitch" */
    SurgeSynthesizer::ID surge_id;
    int valtype;              /* 0=int, 1=bool, 2=float */
    float min_val;
    float max_val;
    char options_json[4096];
    int param_id_in_scene;
    int ctrltype;             /* Surge ctrltypes */
    int scene;                /* 0 global, 1 A, 2 B */
    int ctrlgroup;            /* Surge ControlGroup */
    int ctrlgroup_entry;
};

struct RemoteFiles;
struct RemoteBuild;

/* =====================================================================
 * Instance structure
 * ===================================================================== */

typedef struct {
    bool enabled;
    int source_idx;
    int dest_idx;
    float amount;
    bool is_active;
    long active_ptag;
    int active_modsource;
} surge_mod_slot;

#define NUM_MOD_SLOTS 6

typedef struct {
    char module_dir[256];
    char error_msg[256];

    MovePluginLayer *plugin_layer;
    SurgeSynthesizer *synth;

    int current_preset;
    int preset_count;
    int octave_transpose;
    float output_gain;
    char preset_name[64];

    /* Dynamic parameter registry */
    surge_param_entry params[MAX_SURGE_PARAMS];
    int param_count;

    surge_mod_slot mod_slots[NUM_MOD_SLOTS];

    /* Patch Categories */
    std::map<int, std::string> *category_id_to_name;
    struct category_entry {
        char name[64];
        int first_idx;
    };
    std::vector<category_entry> *categories;

    /* Pre-built JSON strings */
    char *ui_hierarchy_json;
    char *chain_params_json;

    /* Every registered parameter (and macro) as the loaded preset left it.
     * State is written relative to this, so it must be re-taken whenever a
     * patch is loaded, by us or by a MIDI program change. */
    float base_val[MAX_SURGE_PARAMS];
    float base_macro[n_customcontrollers];

    /* Remote UI: what the audio thread builds (RemoteBuild) and the worker
     * that writes it out (RemoteFiles); `rid` names this instance's files. */
    RemoteBuild *rb;
    RemoteFiles *remote;
    int rid;
    bool state_overflow_logged;

    /* Auto BPM Sync */
    bool sync_bpm;
    double bpm;
    double last_clock_time;
    int clock_count;
    double clock_time_sum;
} surge_instance_t;

/* =====================================================================
 * Utility functions
 * ===================================================================== */

static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[512];
        snprintf(buf, sizeof(buf), "[surge] %s", msg);
        g_host->log(buf);
    }
}

static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = (float)atof(pos);
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    if (*pos == '"') {
        pos++;
        int i = 0;
        while (*pos && *pos != '"' && i < out_len - 1) {
            if (*pos == '\\' && (pos[1] == '"' || pos[1] == '\\')) pos++;
            out[i++] = *pos++;
        }
        out[i] = '\0';
        return 0;
    }
    return -1;
}

/* =====================================================================
 * Parameter registry population
 * ===================================================================== */

static void remote_request(surge_instance_t *inst, unsigned bits);
static void remote_reshaped(surge_instance_t *inst);
enum { RF_PARAMS = 1, RF_WAVES = 2, RF_FILTERS = 4, RF_ALL = 7 };

/* The waveshaper's option list is the device UI's own spelling, not Surge's. */
static const char WS_TYPE_OPTIONS[] =
    "[\"Off\",\"Soft\",\"Hard\",\"Asymmetric\",\"Sine\",\"Digital\",\"OJD\",\"Fuzz\",\"Fuzz+Octave\","
    "\"K35\",\"Distortion\",\"Distortion+Asym\",\"Tube\",\"Tube2\",\"Clip\",\"Fold\",\"Waveshaper\",\"S-Curve\","
    "\"Sinusoid\",\"Chebyshev\",\"Chebyshev2\",\"Chebyshev3\",\"Chebyshev4\",\"Chebyshev5\",\"Chebyshev6\","
    "\"Symmetric Clip\",\"Symmetric Fold\",\"Asymmetric Clip\",\"Asymmetric Fold\",\"Soft Clip\",\"Soft Fold\","
    "\"Hard Clip\",\"Hard Fold\",\"Wavefolder\",\"Wavefolder 2\",\"Wavefolder 3\",\"Wavefolder 4\",\"Wavefolder 5\","
    "\"Wavefolder 6\",\"Bitcrusher\",\"Bitcrusher 2\",\"Sample & Hold\",\"Decimator\",\"Slew Rate Limiter\"]";

/* An entry's option texts as a JSON array in a fixed buffer: built one text
 * at a time, so the audio thread can spread a long list over several blocks.
 * A list that does not fit is no list, not half of one. */
struct OptList {
    char buf[sizeof(((surge_param_entry *)nullptr)->options_json)];
    size_t n = 0;
    bool fits = true;
    void begin() { n = 0; fits = true; buf[n++] = '['; }
    void add(const std::string &s) {
        if (!fits) return;
        if (n + s.size() * 2 + 4 >= sizeof(buf)) { fits = false; return; }
        if (n > 1) buf[n++] = ',';
        buf[n++] = '"';
        for (char c : s) {
            if (c == '"' || c == '\\') buf[n++] = '\\';
            buf[n++] = c;
        }
        buf[n++] = '"';
    }
    void finish_into(char *dst) {
        if (fits && n + 2 < sizeof(buf)) { buf[n++] = ']'; buf[n] = '\0'; memcpy(dst, buf, n + 1); }
        else dst[0] = '\0';
    }
};

/* The fields of an entry that follow the parameter's CURRENT type: an
 * oscillator, FX, filter or LFO type switch changes its dependants' value
 * type, range, control type and option texts. Audio thread only. */
static void set_entry_basics(surge_param_entry *entry, const Parameter *p) {
    entry->valtype = p->valtype;
    entry->min_val = (p->valtype == 2) ? p->val_min.f : (float)p->val_min.i;
    entry->max_val = (p->valtype == 2) ? p->val_max.f : (float)p->val_max.i;
    entry->ctrltype = (int)p->ctrltype;
}
/* Does the registry keep Surge's option texts for this entry? (the device UI
 * shows them, and set_param maps a text back to its index through them) */
static bool registry_has_options(const surge_param_entry *entry, const Parameter *p) {
    if (strcmp(entry->key, "ws_type") == 0) return false;   /* WS_TYPE_OPTIONS instead */
    if (!(p->valtype == 0 || p->valtype == 1) || p->ctrltype == ct_filtersubtype || p->ctrltype == ct_none) return false;
    const long long span = (long long)p->val_max.i - (long long)p->val_min.i; /* never int: INT_MIN..INT_MAX exists */
    return span > 0 && span < 128;
}
/* All of an entry's typed fields at once (create_instance). The Remote UI
 * rebuild does the same a few texts at a time -- see params_emit_step. */
static void fill_entry_types(surge_param_entry *entry, Parameter *p) {
    set_entry_basics(entry, p);
    entry->options_json[0] = '\0';
    if (strcmp(entry->key, "ws_type") == 0) {
        snprintf(entry->options_json, sizeof(entry->options_json), "%s", WS_TYPE_OPTIONS);
    } else if (registry_has_options(entry, p)) {
        OptList *list = new OptList();   /* 4 KB: not on the callback's stack */
        list->begin();
        const int lo = p->val_min.i, hi = p->val_max.i;
        for (int v = lo; v <= hi; v++) list->add(p->get_display(true, (float)(v - lo) / (float)(hi - lo)));
        list->finish_into(entry->options_json);
        delete list;
    }
}

static void register_one(surge_instance_t *inst, Parameter *p, int i) {
    SurgeSynthesizer::ID id;
    if (!inst->synth->fromSynthSideId(i, id)) return;
    if (inst->param_count >= MAX_SURGE_PARAMS) return;
    /* Surge's global fx_disable is an int bitmask with a range of INT_MIN..INT_MAX
     * and no control type: not a control, and a range no option list can walk.
     * (The 32-bit difference of that range overflowed to -1, passed a "< 128"
     * check, and the option loop below then ran until std::bad_alloc took the
     * whole Move process down -- 2026-09-05.) Scene A keeps its ct_none entries,
     * because their positions are what saved mod-slot destinations index. */
    if (p->ctrltype == ct_none && p->scene == 0 && p->ctrlgroup != cg_FX) return;
    surge_param_entry *entry = &inst->params[inst->param_count];

    /* KEYS. Scene A keeps its historical bare names ("osc1_pitch"): they are in
     * saved states, in chain_params and in the device UI. Scene B keeps Surge's
     * own "b_" prefix. Global parameters take "g_", except the FX slots, whose
     * storage names ("type", "p0") repeat per slot and are keyed by slot:
     * "fx<slot>_type", "fx<slot>_p<n>", slot in fxslot_positions order. */
    const char *sname = p->get_storage_name();
    if (p->scene == 1 && sname[0] == 'a' && sname[1] == '_') snprintf(entry->key, sizeof(entry->key), "%s", sname + 2);
    else if (p->scene == 2) snprintf(entry->key, sizeof(entry->key), "%s", sname);
    else if (p->ctrlgroup == cg_FX) {
        /* Surge's storage name already carries a ONE-based slot ("fx1_type",
         * "fx1_p0"); the key carries the zero-based slot index the page and
         * fxslot_positions use, and the rest of the name as Surge spells it. */
        const char *rest = strchr(sname, '_');
        snprintf(entry->key, sizeof(entry->key), "fx%d_%s", p->ctrlgroup_entry, rest ? rest + 1 : sname);
    } else snprintf(entry->key, sizeof(entry->key), "g_%s", sname);

    const char *fname = p->get_full_name();
    strncpy(entry->display_name, fname, sizeof(entry->display_name) - 1);
    entry->display_name[sizeof(entry->display_name) - 1] = '\0';

    entry->surge_id = id;
    entry->param_id_in_scene = p->param_id_in_scene;
    entry->scene = p->scene;
    entry->ctrlgroup = (int)p->ctrlgroup;
    entry->ctrlgroup_entry = p->ctrlgroup_entry;
    fill_entry_types(entry, p);
    inst->param_count++;
}

/* Built ONCE, in create_instance. Keys and Surge IDs do not move on a preset
 * load or a type switch (param_ptr is a fixed table), so nothing re-populates
 * it: what does change is refreshed per entry by fill_entry_types. */
static void populate_param_registry(surge_instance_t *inst) {
    if (!inst->synth) return;

    auto &patch = inst->synth->storage.getPatch();
    int n_params = (int)patch.param_ptr.size();
    inst->param_count = 0;

    /* Three passes so the order is Scene A, Scene B, global -- Scene A first
     * because mod-slot destinations in saved states are registry INDICES. */
    for (int pass = 0; pass < 3; pass++) {
        const int want = pass == 0 ? 1 : pass == 1 ? 2 : 0;
        for (int i = 0; i < n_params; i++) {
            Parameter *p = patch.param_ptr[i];
            if (!p || p->scene != want) continue;
            register_one(inst, p, i);
        }
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Registered %d parameters (all scenes, global, FX)", inst->param_count);
    plugin_log(msg);
}

/* =====================================================================
 * Category helpers
 * ===================================================================== */

static void find_category_names_recursive(const std::vector<PatchCategory> &cats, std::map<int, std::string> &map) {
    for (const auto &cat : cats) {
        map[cat.internalid] = cat.name;
        if (!cat.children.empty()) {
            find_category_names_recursive(cat.children, map);
        }
    }
}

static void update_category_list(surge_instance_t *inst) {
    if (!inst->synth || !inst->category_id_to_name || !inst->categories) return;

    auto &storage = inst->synth->storage;
    inst->category_id_to_name->clear();
    find_category_names_recursive(storage.patch_category, *inst->category_id_to_name);

    /* Sort patchOrdering by category then name */
    std::sort(storage.patchOrdering.begin(), storage.patchOrdering.end(), [&](int a, int b) {
        if (a < 0 || a >= (int)storage.patch_list.size()) return false;
        if (b < 0 || b >= (int)storage.patch_list.size()) return true;

        const auto &pa = storage.patch_list[a];
        const auto &pb = storage.patch_list[b];

        auto get_cat = [&](int id) {
            auto it = inst->category_id_to_name->find(id);
            /* Use a prefix that sorts "Unknown" to the end ({ is after Z in ASCII) */
            return (it != inst->category_id_to_name->end()) ? it->second : std::string("{Unknown}");
        };

        std::string catA = get_cat(pa.category);
        std::string catB = get_cat(pb.category);

        auto compare_ci = [](const std::string& s1, const std::string& s2) {
            return std::lexicographical_compare(
                s1.begin(), s1.end(), s2.begin(), s2.end(),
                [](unsigned char c1, unsigned char c2) { return std::tolower(c1) < std::tolower(c2); }
            );
        };

        if (catA != catB) {
            bool a_less_b = compare_ci(catA, catB);
            bool b_less_a = compare_ci(catB, catA);
            if (a_less_b || b_less_a) return a_less_b;
        }
        return compare_ci(pa.name, pb.name);
    });

    inst->categories->clear();
    std::string last_cat_name = "";

    for (int i = 0; i < (int)storage.patchOrdering.size(); i++) {
        int patch_idx = storage.patchOrdering[i];
        if (patch_idx < 0 || patch_idx >= (int)storage.patch_list.size()) continue;

        int cat_id = storage.patch_list[patch_idx].category;
        std::string cat_name = "Unknown";
        auto it = inst->category_id_to_name->find(cat_id);
        if (it != inst->category_id_to_name->end()) {
            cat_name = it->second;
        }

        if (cat_name != last_cat_name) {
            surge_instance_t::category_entry entry;
            strncpy(entry.name, cat_name.c_str(), sizeof(entry.name) - 1);
            entry.name[sizeof(entry.name) - 1] = '\0';
            entry.first_idx = i;
            inst->categories->push_back(entry);
            last_cat_name = cat_name;
        }
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Found %d patch categories", (int)inst->categories->size());
    plugin_log(msg);
}

static const char* get_current_preset_category(surge_instance_t *inst) {
    if (!inst->synth || inst->current_preset < 0) return "Unknown";

    auto &storage = inst->synth->storage;
    if (inst->current_preset >= (int)storage.patchOrdering.size()) return "Unknown";

    int patch_idx = storage.patchOrdering[inst->current_preset];
    int cat_id = storage.patch_list[patch_idx].category;

    if (inst->category_id_to_name) {
        auto it = inst->category_id_to_name->find(cat_id);
        if (it != inst->category_id_to_name->end()) {
            return it->second.c_str();
        }
    }
    return "Unknown";
}

/* Find a parameter entry by key. The pointer stays valid: the registry is a
 * fixed array, never re-populated, and only the audio thread writes it. */
static surge_param_entry* find_param(surge_instance_t *inst, const char *key) {
    for (int i = 0; i < inst->param_count; i++) {
        if (strcmp(inst->params[i].key, key) == 0) {
            return &inst->params[i];
        }
    }
    return nullptr;
}

/* =====================================================================
 * Preset loading
 * ===================================================================== */

static void derive_mod_slots(surge_instance_t *inst);

/* What the loaded preset sets every parameter and macro to: the base that
 * `state` is written against. Taken right after a load, before any edit. */
static void capture_baseline(surge_instance_t *inst) {
    for (int i = 0; i < inst->param_count; i++)
        inst->base_val[i] = inst->synth->getParameter01(inst->params[i].surge_id);
    for (int m = 0; m < n_customcontrollers; m++)
        inst->base_macro[m] = inst->synth->getMacroParameterTarget01(m);
}

static void set_preset_name_from_patch(surge_instance_t *inst) {
    const char *name = inst->synth->storage.getPatch().name.c_str();
    if (name && name[0]) {
        strncpy(inst->preset_name, name, sizeof(inst->preset_name) - 1);
        inst->preset_name[sizeof(inst->preset_name) - 1] = '\0';
    } else {
        snprintf(inst->preset_name, sizeof(inst->preset_name), "Init");
    }
}

/* Everything that follows a patch load, whoever loaded it. */
static void after_patch_load(surge_instance_t *inst) {
    set_preset_name_from_patch(inst);
    derive_mod_slots(inst);
    capture_baseline(inst);
    inst->synth->patch_loaded = false;   /* seen; see remote_service for loads we did not start */
    remote_reshaped(inst);
}

static void load_preset_by_display_index(surge_instance_t *inst, int display_idx) {
    if (!inst->synth) return;

    auto &storage = inst->synth->storage;
    if (display_idx < 0 || display_idx >= (int)storage.patchOrdering.size()) return;

    int raw_idx = storage.patchOrdering[display_idx];
    inst->synth->loadPatch(raw_idx);
    inst->current_preset = display_idx;
    after_patch_load(inst);
}

/* A patch Surge loaded on its own -- a MIDI program change, which Surge loads
 * on a thread of its own while halt_engine silences process(). Its display
 * index is found by NAME: Surge sets `patchid` after it has released the
 * engine, so it may still be the previous one when we look. */
static void adopt_foreign_patch_load(surge_instance_t *inst) {
    auto &storage = inst->synth->storage;
    const std::string &name = storage.getPatch().name;
    for (int i = 0; i < (int)storage.patchOrdering.size(); i++) {
        const int raw = storage.patchOrdering[i];
        if (raw >= 0 && raw < (int)storage.patch_list.size() && storage.patch_list[raw].name == name) {
            inst->current_preset = i;
            break;
        }
    }
    after_patch_load(inst);
}

/* The six device mod slots, as the loaded patch's first six live routings. */
static void derive_mod_slots(surge_instance_t *inst) {
    auto &patch = inst->synth->storage.getPatch();
    for (int i = 0; i < NUM_MOD_SLOTS; i++) {
        inst->mod_slots[i].enabled = false;
        inst->mod_slots[i].source_idx = 0;
        inst->mod_slots[i].dest_idx = 0;
        inst->mod_slots[i].amount = 0.0f;
        inst->mod_slots[i].is_active = false;
    }

    int slot_idx = 0;
    const std::vector<ModulationRouting> *mod_lists[] = {
        &patch.scene[0].modulation_voice,
        &patch.scene[0].modulation_scene,
        &patch.modulation_global
    };

    for (int L = 0; L < 3 && slot_idx < NUM_MOD_SLOTS; L++) {
        for (const auto &mr : *mod_lists[L]) {
            if (mr.depth != 0.0f && !mr.muted) {
                int dest_idx = 0;
                for (int p = 0; p < inst->param_count; p++) {
                    if (L < 2 && inst->params[p].scene != 1) continue;
                    long target_id = (L == 2) ? inst->params[p].surge_id.getSynthSideId() : inst->params[p].param_id_in_scene;
                    if (target_id == mr.destination_id) {
                        dest_idx = p + 1;
                        break;
                    }
                }
                if (dest_idx > 0) {
                    inst->mod_slots[slot_idx].enabled = true;
                    inst->mod_slots[slot_idx].source_idx = mr.source_id;
                    inst->mod_slots[slot_idx].dest_idx = dest_idx;
                    
                    long ptag = inst->params[dest_idx - 1].surge_id.getSynthSideId();
                    if (ptag >= 0 && ptag < (long)inst->synth->storage.getPatch().param_ptr.size()) {
                        Parameter* param = inst->synth->storage.getPatch().param_ptr[ptag];
                        if (param) {
                            float val_range = param->val_max.f - param->val_min.f;
                            float norm = (val_range > 0) ? (mr.depth / val_range) : 0.0f;
                            if (norm < -1.0f) norm = -1.0f;
                            if (norm > 1.0f) norm = 1.0f;
                            inst->mod_slots[slot_idx].amount = norm;
                        } else {
                            inst->mod_slots[slot_idx].amount = mr.depth;
                        }
                    } else {
                        inst->mod_slots[slot_idx].amount = mr.depth;
                    }
                    
                    inst->mod_slots[slot_idx].is_active = true;
                    inst->mod_slots[slot_idx].active_ptag = mr.destination_id;
                    inst->mod_slots[slot_idx].active_modsource = mr.source_id;
                    slot_idx++;
                    if (slot_idx >= NUM_MOD_SLOTS) break;
                }
            }
        }
    }
}

/* =====================================================================
 * Remote UI files
 *
 * The Remote UI (web_ui.html) sees the module through Schwung Manager,
 * which hands a browser two things: our `state` blob, flattened one field per
 * key, and `chain_params`. Both live inside the host's 64 KB parameter
 * buffer -- and chain_params, with Scene A alone, is already 60.8 KB of it.
 * So the VALUES go through `state` (as differences from the loaded preset, see
 * get_param "state"), and everything else the page needs is written to files
 * under <module_dir>/remote/<rid>/ -- one directory per instance, named by the
 * `rid` the state carries -- which the manager serves verbatim through its
 * module-assets route:
 *
 *   patch_params.json   every parameter: key, names, group, control type,
 *                       range, default, the loaded preset's value (`pv`), and
 *                       Surge's own displayInfo, so the page reproduces
 *                       Parameter::get_display exactly instead of guessing
 *                       units. Rebuilt on every preset load and after any type
 *                       change, because oscillator and FX parameters change
 *                       name, type and range with the type.
 *   presets_index.json  the patch list in display order, grouped by category.
 *   waves.json          a rendered cycle of each oscillator, the way Surge's
 *                       own waveform display renders it (spawn_osc + a block
 *                       loop), for both scenes.
 *   filters.json        the measured magnitude response of each filter, the
 *                       way Surge's filter analysis measures it (a log sweep
 *                       through the real filter unit).
 *
 * WHO TOUCHES WHAT. Only the audio thread reads Surge. Surge mutates its patch
 * there -- loadPatch, a type switch (process() spawns the new oscillator or
 * effect and deletes the old one), the modulation vectors -- and process()
 * itself holds storage.modRoutingMutex every block, so another thread can
 * neither read the patch safely nor lock it without stalling audio. Even a
 * copied Parameter is not enough: get_display follows user_data into the live
 * Effect, which an FX type switch deletes. So the audio thread does the
 * reading, in slices of at most REMOTE_BUDGET_NS per block (RemoteBuild): it
 * formats patch_params.json one parameter at a time, renders waves.json a few
 * oscillator blocks at a time, and copies four numbers per filter unit. A
 * finished string is handed to the worker by swapping buffers that were
 * reserved up front, so the hand-off neither allocates nor waits.
 *
 * The worker (RemoteFiles) then only does what needs no Surge: the filter
 * responses, from that copy, with the stateless sst filter code, and the
 * writes -- to a temp name, renamed, so a browser fetching mid-write sees the
 * old file or the new one. Each file carries a `rev`. The worker never touches
 * the instance, so it needs no lock with the audio thread at all; it runs at
 * SCHED_OTHER on cores 0-2, never at the callback's priority (see
 * remote_start).
 * ===================================================================== */

/* Which control types change OTHER parameters' names, types and ranges. */
static bool is_type_ctrl(int ct) {
    return ct == ct_osctype || ct == ct_fxtype || ct == ct_filtertype || ct == ct_wstype || ct == ct_lfotype;
}

/* JSON into a std::string whose capacity was reserved: none of these
 * allocate unless a file outgrows its reservation. */
static void jstr(std::string &out, const char *s) {
    out += '"';
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { out += '\\'; out += (char)*p; }
        else if (*p < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", *p); out += b; }
        else out += (char)*p;
    }
    out += '"';
}
static void jnum(std::string &out, double v) {
    char b[40];
    if (!std::isfinite(v)) { out += "null"; return; }
    /* nine significant digits: every float survives the round trip. Six lost
     * the tail of displayInfo.b (1/12) and moved a cutoff readout by 0.04 Hz. */
    snprintf(b, sizeof(b), "%.9g", v);
    out += b;
}
static void jint(std::string &out, long long v) {
    char b[24];
    snprintf(b, sizeof(b), "%lld", v);
    out += b;
}
static void jbool(std::string &out, bool v) { out += v ? "true" : "false"; }

/* A filter unit is fully described by four numbers; this is all the worker
 * gets of the patch. */
struct FilterSnap {
    int type[n_scenes][n_filterunits_per_scene];
    int sub[n_scenes][n_filterunits_per_scene];
    float cutoff[n_scenes][n_filterunits_per_scene];
    float res[n_scenes][n_filterunits_per_scene];
};

#define REMOTE_PARAMS_RESERVE (512 * 1024)   /* patch_params.json is ~340 KB */
#define REMOTE_WAVES_RESERVE  (64 * 1024)    /* waves.json is ~21 KB */
#define REMOTE_BUDGET_NS      100000         /* audio-thread time per 128-frame block, at most */
#define WAVE_POINTS 512
#define WAVE_AVG    4

/* ---- the worker's side ------------------------------------------------ */

struct RemoteFiles {
    pthread_t th{};
    bool started = false;
    std::mutex m;                       /* guards nothing but the condition variable */
    std::condition_variable cv;
    std::atomic<bool> quit{false};
    std::atomic<bool> want_presets{false};
    /* One hand-off slot per file: 0 = the audio thread owns it, 1 = the worker. */
    std::atomic<int> params_full{0}, waves_full{0}, filters_full{0};
    std::string params, waves;
    FilterSnap filters{};
    std::string presets;                /* built in create_instance; the list never changes after */
    char root[560] = {0};               /* <module_dir>/remote */
    char dir[600] = {0};                /* <module_dir>/remote/<rid> */
    int rev = 0;
    /* the worker's own buffers -- per instance, so two slots never share one */
    std::string params_local, waves_local, filters_json;
    FilterSnap filters_local{};
    std::vector<float> sweep, out;
    float delayBuffer[4][sst::filters::utilities::MAX_FB_COMB + sst::filters::utilities::SincTable::FIRipol_N];
};

static bool remote_has_work(RemoteFiles *rf) {
    return rf->quit.load() || rf->want_presets.load() || rf->params_full.load() || rf->waves_full.load() ||
           rf->filters_full.load();
}

/* Writes `body` (a JSON object) as <dir>/<name>, with this file's `rev`
 * spliced in after the opening brace. */
static bool remote_write(RemoteFiles *rf, const char *name, const std::string &body) {
    char path[700], tmp[720], head[40];
    snprintf(path, sizeof(path), "%s/%s", rf->dir, name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok;
    if (body.size() >= 2 && body[0] == '{') {
        const bool empty = body[1] == '}';
        const int n = snprintf(head, sizeof(head), "{\"rev\":%d%s", ++rf->rev, empty ? "" : ",");
        ok = fwrite(head, 1, (size_t)n, f) == (size_t)n &&
             fwrite(body.data() + 1, 1, body.size() - 1, f) == body.size() - 1;
    } else {
        ok = fwrite(body.data(), 1, body.size(), f) == body.size();
    }
    if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

/* filters.json: the magnitude response of each filter unit, measured the way
 * Surge's Filter Analysis measures it -- a logarithmic sine sweep through the
 * real quad filter unit with the real coefficient maker -- and read at 96
 * log-spaced frequencies by Goertzel (the analysis uses JUCE's FFT, which the
 * headless build does not have). Type 0 (Off) writes null. Reads nothing but
 * the FilterSnap the audio thread copied. */
static void remote_write_filters(RemoteFiles *rf) {
    const FilterSnap &fs = rf->filters_local;
    const int N = 16384, NF = 96;
    const float SR = 44100.f, F0 = 20.f, F1 = 20000.f;
    std::vector<float> &sweep = rf->sweep, &out = rf->out;
    sweep.assign(N, 0.f); out.assign(N, 0.f);
    const float beta = (float)N / std::log(F1 / F0);
    for (int i = 0; i < N; i++) {
        const float phase = 2.f * (float)M_PI * beta * F0 * (std::pow(F1 / F0, (float)i / (float)N) - 1.f);
        sweep[i] = 0.70710678f * std::sin((phase + (float)M_PI / 180.f) / SR);
    }
    float hz[NF];
    for (int k = 0; k < NF; k++) hz[k] = F0 * std::pow(F1 / F0, (float)k / (NF - 1));
    auto goertzel = [&](const std::vector<float> &x, float f) {
        const double w = 2.0 * M_PI * f / SR, c = 2.0 * cos(w);
        double s0, s1 = 0, s2 = 0;
        for (int i = 0; i < N; i++) { s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
        const double re = s1 - s2 * cos(w), im = s2 * sin(w);
        return sqrt(re * re + im * im);
    };
    double xm[NF];
    for (int k = 0; k < NF; k++) xm[k] = goertzel(sweep, hz[k]);
    std::string &j = rf->filters_json;
    j.clear();
    j += "{\"hz\":[";
    for (int k = 0; k < NF; k++) { if (k) j += ","; jnum(j, hz[k]); }
    j += "],\"scenes\":[";
    for (int s = 0; s < n_scenes; s++) {
        j += s ? ",[" : "[";
        for (int f = 0; f < n_filterunits_per_scene; f++) {
            if (rf->quit.load()) return;   /* each unit is a few ms; leave promptly on unload */
            int type = fs.type[s][f], sub = fs.sub[s][f];
            if (type < 0 || type >= sst::filters::num_filter_types) type = 0;
            /* a type switch lands before its subtype is clamped by Surge */
            if (sub < 0) sub = 0;
            if (sub >= sst::filters::fut_subcount[type]) sub = sst::filters::fut_subcount[type] > 0 ? sst::filters::fut_subcount[type] - 1 : 0;
            if (f) j += ",";
            j += "{\"type\":"; jint(j, type); j += ",\"sub\":"; jint(j, sub);
            j += ",\"cutoff\":"; jnum(j, fs.cutoff[s][f]);
            j += ",\"res\":"; jnum(j, fs.res[s][f]); j += ",\"db\":";
            auto fptr = sst::filters::GetQFPtrFilterUnit((sst::filters::FilterType)type, (sst::filters::FilterSubType)sub);
            if (type == 0 || !fptr) { j += "null}"; continue; }
            sst::filters::QuadFilterUnitState st{};
            for (int i = 0; i < 4; i++) st.DB[i] = &rf->delayBuffer[i][0];
            sst::filters::FilterCoefficientMaker<> cm;
            cm.setSampleRateAndBlockSize(SR, 512);
            cm.MakeCoeffs(fs.cutoff[s][f], fs.res[s][f], (sst::filters::FilterType)type, (sst::filters::FilterSubType)sub, nullptr, false);
            cm.updateState(st);
            std::fill(st.R, &st.R[sst::filters::n_filter_registers], SIMD_MM(setzero_ps)());
            for (int i = 0; i < 4; i++) { st.WP[i] = 0; st.active[i] = 0; }
            st.active[0] = 0xFFFFFFFF;
            for (int i = 0; i < N; i++) {
                auto y = fptr(&st, SIMD_MM(set_ps1)(sweep[i]));
                float ya alignas(16)[4];
                SIMD_MM(store_ps)(ya, y);
                out[i] = ya[0];
            }
            j += "[";
            for (int k = 0; k < NF; k++) {
                if (k) j += ",";
                const double ym = goertzel(out, hz[k]);
                const double db = (xm[k] > 0 && ym > 0) ? 20.0 * log10(ym / xm[k]) : -120.0;
                jnum(j, db);
            }
            j += "]}";
        }
        j += "]";
    }
    j += "]}\n";
    remote_write(rf, "filters.json", j);
}

/* create_instance runs on the host's SPI audio callback (SCHED_FIFO 70), and
 * a thread inherits its creator's policy and CPU set: left alone, the filter
 * sweeps and file writes below would run above Move's own Link thread
 * (FIFO 35). remote_start creates the thread with SCHED_OTHER and cores 0-2
 * explicitly, so it never runs an instruction at realtime; this repeats both
 * as the thread's first act, in case a libc ignores the attributes. Dropping
 * to SCHED_OTHER needs no privilege. */
static void remote_thread_setup() {
    sched_param sp{};
    sp.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c = 0; c < 3; c++) CPU_SET(c, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    pthread_setname_np(pthread_self(), "surge-remote");
#endif
}

static void *remote_thread_main(void *arg) {
    remote_thread_setup();
    RemoteFiles *rf = (RemoteFiles *)arg;
    mkdir(rf->root, 0777);
    mkdir(rf->dir, 0777);
    /* This thread lives inside Move's own process: an exception that escaped
     * here would be std::terminate for the whole device. */
    auto guarded = [](const char *what, auto fn) {
        try { fn(); }
        catch (const std::exception &e) { char m[200]; snprintf(m, sizeof(m), "remote %s failed: %s", what, e.what()); plugin_log(m); }
        catch (...) { char m[200]; snprintf(m, sizeof(m), "remote %s failed", what); plugin_log(m); }
    };
    for (;;) {
        {
            /* a timeout, so a notify the audio thread sent between our check
             * and our wait costs at most this long; quit is set under the lock,
             * so unloading is never kept waiting by it */
            std::unique_lock<std::mutex> l(rf->m);
            rf->cv.wait_for(l, std::chrono::milliseconds(100), [rf] { return remote_has_work(rf); });
            if (rf->quit.load()) break;
        }
        if (rf->want_presets.exchange(false))
            guarded("presets", [&] { remote_write(rf, "presets_index.json", rf->presets); });
        if (rf->params_full.load(std::memory_order_acquire)) {
            rf->params_local.swap(rf->params);
            rf->params_full.store(0, std::memory_order_release);
            guarded("params", [&] { remote_write(rf, "patch_params.json", rf->params_local); });
        }
        if (rf->quit.load()) break;
        if (rf->waves_full.load(std::memory_order_acquire)) {
            rf->waves_local.swap(rf->waves);
            rf->waves_full.store(0, std::memory_order_release);
            guarded("waves", [&] { remote_write(rf, "waves.json", rf->waves_local); });
        }
        if (rf->quit.load()) break;
        if (rf->filters_full.load(std::memory_order_acquire)) {
            rf->filters_local = rf->filters;
            rf->filters_full.store(0, std::memory_order_release);
            guarded("filters", [&] { remote_write_filters(rf); });
        }
    }
    return nullptr;
}

/* ---- the audio thread's side ------------------------------------------- */

#define PARAM_TEXTS_PER_STEP 16
enum { PE_HEAD, PE_OPTS, PE_TBL, PE_TAIL };   /* where a parameter's emit has got to */

struct RemoteBuild {
    unsigned want = 0;          /* RF_* asked for and not begun */
    unsigned gen = 0;           /* bumped by anything that re-shapes the patch */
    bool hold = false;          /* sit out the next block: Surge applies the switch in it */
    /* patch_params.json, one parameter -- or a few of its texts -- at a time */
    bool p_on = false, p_ready = false, p_reg = false;
    int p_i = 0, p_n = 0, p_phase = 0, p_v = 0, p_lo = 0, p_hi = 0;
    unsigned p_gen = 0;
    std::string p;
    surge_param_entry staged{};   /* the registry entry being rebuilt */
    OptList reg;                  /* ...and its option texts */
    /* waves.json, a few oscillator blocks at a time */
    bool w_on = false, w_ready = false, w_disp = false;
    unsigned w_gen = 0;
    int w_s = 0, w_o = 0, w_pt = -1, w_bp = 0, w_type = -1;
    Oscillator *w_osc = nullptr;
    std::string w;
    alignas(16) unsigned char oscbuffer[oscillator_buffer_size];
    alignas(16) float tmp[2][BLOCK_SIZE_OS];
    pdata tp[n_scene_params];
    sst::filters::HalfRate::HalfRateFilter hr{6, true};
    /* filters.json: four numbers per unit */
    bool f_ready = false;
    FilterSnap f{};
};

static inline int64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void remote_request(surge_instance_t *inst, unsigned bits) {
    if (inst && inst->rb) inst->rb->want |= bits;
}
/* A preset load, a restore or a type switch: whatever is half-built describes
 * a patch that no longer exists, so it starts over. */
static void remote_reshaped(surge_instance_t *inst) {
    if (!inst || !inst->rb) return;
    inst->rb->gen++;
    inst->rb->want |= RF_ALL;
    inst->rb->hold = true;
}

/* patch_params.json, header: everything but the parameters and routings. */
static void params_begin(surge_instance_t *inst) {
    RemoteBuild *b = inst->rb;
    auto &patch = inst->synth->storage.getPatch();
    std::string &j = b->p;
    j.clear();
    j += "{\"preset\":"; jint(j, inst->current_preset);
    j += ",\"preset_name\":"; jstr(j, inst->preset_name);
    j += ",\"category\":"; jstr(j, get_current_preset_category(inst));
    j += ",\"macros\":[";
    for (int i = 0; i < n_customcontrollers; i++) {
        if (i) j += ",";
        j += "{\"key\":\"macro"; jint(j, i + 1); j += "\",\"label\":"; jstr(j, patch.CustomControllerLabel[i]);
        j += ",\"pv\":"; jnum(j, inst->base_macro[i]); j += "}";
    }
    j += "],\"wavetables\":[";
    for (int s = 0; s < n_scenes; s++) {
        j += s ? ",[" : "[";
        for (int o = 0; o < n_oscs; o++) { if (o) j += ","; jstr(j, patch.scene[s].osc[o].wavetable_display_name.c_str()); }
        j += "]";
    }
    j += "],\"params\":[";
    b->p_i = 0;
    b->p_n = 0;
    b->p_phase = PE_HEAD;
    b->p_gen = b->gen;
    b->p_on = true;
    b->p_ready = false;
}

/* One parameter of patch_params.json, and its registry entry brought up to
 * the parameter's current type while we are looking at it -- resumable, so a
 * long list of Surge's texts (128 note names for a split key, 65 samples of an
 * FX readout) is spread over several blocks, PARAM_TEXTS_PER_STEP at a time.
 * Each text is formatted once and goes to both the file and the registry; the
 * registry entry is replaced whole when its parameter is done, so set_param
 * never sees a new range beside an old option list. Returns true when the
 * parameter is complete. */
static bool params_emit_step(surge_instance_t *inst) {
    RemoteBuild *b = inst->rb;
    auto &patch = inst->synth->storage.getPatch();
    const int i = b->p_i;
    surge_param_entry &e = inst->params[i];
    const long sid = e.surge_id.getSynthSideId();
    if (sid < 0 || sid >= (long)patch.param_ptr.size() || !patch.param_ptr[sid]) { b->p_phase = PE_HEAD; return true; }
    Parameter *p = patch.param_ptr[sid];
    std::string &j = b->p;

    if (b->p_phase == PE_HEAD) {
        b->staged = e;
        set_entry_basics(&b->staged, p);
        b->p_reg = registry_has_options(&e, p);
        if (strcmp(e.key, "ws_type") == 0) snprintf(b->staged.options_json, sizeof(b->staged.options_json), "%s", WS_TYPE_OPTIONS);
        else b->staged.options_json[0] = '\0';
        if (b->p_reg) b->reg.begin();

        if (b->p_n++) j += ",";
        j += "{\"k\":"; jstr(j, e.key);
        j += ",\"n\":"; jstr(j, p->get_full_name());
        j += ",\"s\":"; jstr(j, p->get_name());
        j += ",\"sc\":"; jint(j, p->scene); j += ",\"g\":"; jint(j, (int)p->ctrlgroup); j += ",\"ge\":"; jint(j, p->ctrlgroup_entry);
        j += ",\"ct\":"; jint(j, (int)p->ctrltype); j += ",\"vt\":"; jint(j, p->valtype);
        j += ",\"min\":"; jnum(j, p->valtype == vt_float ? p->val_min.f : (double)p->val_min.i);
        j += ",\"max\":"; jnum(j, p->valtype == vt_float ? p->val_max.f : (double)p->val_max.i);
        j += ",\"def\":"; jnum(j, p->valtype == vt_float ? p->val_default.f : (p->valtype == vt_bool ? (double)p->val_default.b : (double)p->val_default.i));
        j += ",\"pv\":"; jnum(j, inst->base_val[i]);
        j += ",\"bip\":"; jbool(j, p->is_bipolar());
        j += ",\"ext\":"; jbool(j, p->can_extend_range()); j += ",\"xr\":"; jbool(j, p->extend_range);
        j += ",\"ts\":"; jbool(j, p->can_temposync()); j += ",\"tsn\":"; jbool(j, p->temposync);
        j += ",\"ab\":"; jbool(j, p->can_be_absolute()); j += ",\"abs\":"; jbool(j, p->absolute);
        j += ",\"da\":"; jbool(j, p->can_deactivate()); j += ",\"dv\":"; jbool(j, p->deactivated);
        j += ",\"dt\":"; jint(j, (int)p->displayType);
        const auto &d = p->displayInfo;
        j += ",\"d\":{\"u\":"; jstr(j, d.unit); j += ",\"au\":"; jstr(j, d.absoluteUnit);
        j += ",\"sc\":"; jnum(j, d.scale); j += ",\"a\":"; jnum(j, d.a); j += ",\"b\":"; jnum(j, d.b);
        j += ",\"dec\":"; jint(j, d.decimals); j += ",\"f\":"; jint(j, (long long)d.customFeatures);
        j += ",\"minL\":"; jstr(j, d.minLabel); j += ",\"maxL\":"; jstr(j, d.maxLabel); j += ",\"defL\":"; jstr(j, d.defLabel);
        j += ",\"minV\":"; jnum(j, d.minLabelValue); j += ",\"maxV\":"; jnum(j, d.maxLabelValue);
        j += ",\"absF\":"; jnum(j, d.absoluteFactor); j += ",\"tsm\":"; jnum(j, d.tempoSyncNotationMultiplier);
        j += ",\"nn\":"; jbool(j, d.supportsNoteName); j += "}";

        b->p_phase = PE_TAIL;
        if (p->valtype != vt_float) {
            /* every option, spelled by Surge itself, for ranges a menu can hold */
            const long long span = (long long)p->val_max.i - (long long)p->val_min.i;
            if (p->ctrltype != ct_none && span >= 0 && span < 256) {
                j += ",\"o\":[";
                b->p_lo = p->val_min.i;
                b->p_hi = p->val_max.i;
                b->p_v = b->p_lo;
                b->p_phase = PE_OPTS;
            }
        } else if (p->displayType == Parameter::Custom || p->basicBlocksParamMetaData.has_value()) {
            /* the few float types Surge formats by hand: sample its own text */
            j += ",\"tbl\":[";
            b->p_v = 0;
            b->p_phase = PE_TBL;
        } else {
            /* five reference points of Surge's own text, so the page's port of
             * get_display can be checked against the real thing on every build */
            j += ",\"chk\":[";
            for (int t = 0; t < 5; t++) { if (t) j += ","; jstr(j, p->get_display(true, (float)t / 4.f).c_str()); }
            j += "]";
        }
        return false;   /* the head is a step of its own */
    }

    if (b->p_phase == PE_OPTS) {
        const int lo = b->p_lo, hi = b->p_hi;
        for (int k = 0; k < PARAM_TEXTS_PER_STEP && b->p_v <= hi; k++, b->p_v++) {
            const float ef = hi > lo ? (float)(b->p_v - lo) / (float)(hi - lo) : 0.f;
            const std::string s = p->get_display(true, ef);
            if (b->p_v > lo) j += ",";
            jstr(j, s.c_str());
            if (b->p_reg) b->reg.add(s);
        }
        if (b->p_v <= hi) return false;
        j += "]";
        b->p_phase = PE_TAIL;
    } else if (b->p_phase == PE_TBL) {
        for (int k = 0; k < PARAM_TEXTS_PER_STEP && b->p_v <= 64; k++, b->p_v++) {
            if (b->p_v) j += ",";
            jstr(j, p->get_display(true, (float)b->p_v / 64.f).c_str());
        }
        if (b->p_v <= 64) return false;
        j += "]";
        b->p_phase = PE_TAIL;
    }

    /* PE_TAIL */
    j += "}";
    if (b->p_reg) b->reg.finish_into(b->staged.options_json);
    e = b->staged;
    b->p_phase = PE_HEAD;
    return true;
}

/* patch_params.json, tail: the patch's modulation routings. One step: the
 * vectors are only ever changed on this thread, so they hold still for it. */
static void params_finish(surge_instance_t *inst) {
    RemoteBuild *b = inst->rb;
    auto &patch = inst->synth->storage.getPatch();
    std::string &j = b->p;
    j += "],\"routings\":[";
    bool first = true;
    auto emit = [&](const ModulationRouting &mr, int scene, const char *kind) {
        int idx = -1;
        for (int i = 0; i < inst->param_count; i++) {
            const surge_param_entry &e = inst->params[i];
            if (scene >= 0) { if (e.scene == scene + 1 && e.param_id_in_scene == mr.destination_id) { idx = i; break; } }
            else if (e.surge_id.getSynthSideId() == mr.destination_id) { idx = i; break; }
        }
        if (idx < 0 || mr.source_id < 0 || mr.source_id >= n_modsources) return;
        const int src_scene = mr.source_scene >= 0 ? mr.source_scene : (scene >= 0 ? scene : 0);
        const float depth = inst->synth->getModDepth01(inst->params[idx].surge_id.getSynthSideId(), (modsources)mr.source_id, src_scene, mr.source_index);
        if (!first) j += ",";
        first = false;
        j += "{\"src\":"; jint(j, mr.source_id); j += ",\"srcn\":"; jstr(j, modsource_names[mr.source_id]);
        j += ",\"si\":"; jint(j, mr.source_index); j += ",\"ss\":"; jint(j, src_scene);
        j += ",\"dst\":"; jstr(j, inst->params[idx].key);
        j += ",\"depth\":"; jnum(j, depth);
        j += ",\"muted\":"; jbool(j, mr.muted); j += ",\"kind\":\""; j += kind; j += "\"}";
    };
    for (int s = 0; s < n_scenes; s++) {
        for (const auto &mr : patch.scene[s].modulation_voice) emit(mr, s, "voice");
        for (const auto &mr : patch.scene[s].modulation_scene) emit(mr, s, "scene");
    }
    for (const auto &mr : patch.modulation_global) emit(mr, -1, "global");
    j += "]}\n";
    b->p_on = false;
    b->p_ready = true;
}

static void params_step(surge_instance_t *inst, int64_t deadline) {
    RemoteBuild *b = inst->rb;
    while (b->p_i < inst->param_count) {
        if (params_emit_step(inst)) b->p_i++;
        if (mono_ns() >= deadline) return;
    }
    params_finish(inst);
}

/* waves.json: one rendered cycle-run per oscillator, rendered exactly as
 * Surge's OscillatorWaveformDisplay renders it -- a spawned oscillator at the
 * display pitch, processed in oversampled blocks, halved by the half-rate
 * filter, then averaged four to a point. 512 points per oscillator. The
 * oscillator lives in this instance's own buffer across blocks; anything that
 * could pull its storage out from under it (a type switch, a patch load) bumps
 * `gen`, and the render starts over. */
static float wave_pitch(surge_instance_t *inst) {
    return (90.15f - 48.f) + 12.0f * log2f((float)inst->synth->storage.dsamplerate / 44100.0f);
}
static void waves_drop_osc(RemoteBuild *b) {
    if (b->w_osc) { b->w_osc->~Oscillator(); b->w_osc = nullptr; }
    b->w_pt = -1;
}
static void waves_begin(surge_instance_t *inst) {
    RemoteBuild *b = inst->rb;
    waves_drop_osc(b);
    b->w.clear();
    b->w += "{\"scenes\":[[";
    b->w_s = 0;
    b->w_o = 0;
    b->w_gen = b->gen;
    b->w_on = true;
    b->w_ready = false;
}
static void waves_open(surge_instance_t *inst) {
    RemoteBuild *b = inst->rb;
    auto &storage = inst->synth->storage;
    OscillatorStorage *od = &storage.getPatch().scene[b->w_s].osc[b->w_o];
    const int type = od->type.val.i;
    const bool known = type >= 0 && type < n_osc_types;
    std::string &j = b->w;
    if (b->w_o) j += ",";
    j += "{\"type\":"; jint(j, type); j += ",\"name\":"; jstr(j, known ? osc_type_names[type] : "");
    j += ",\"wt\":"; jstr(j, od->wavetable_display_name.c_str());
    j += ",\"w\":[";
    memset(b->tp, 0, sizeof(b->tp));
    b->tp[od->pitch.param_id_in_scene].f = 0;
    for (int i = 0; i < n_osc_params; i++) b->tp[od->p[i].param_id_in_scene].i = od->p[i].val.i;
    b->w_type = type;
    b->w_osc = known ? spawn_osc(type, &storage, od, b->tp, b->tp, b->oscbuffer) : nullptr;
    b->w_disp = b->w_osc && b->w_osc->allow_display();
    if (b->w_disp) b->w_osc->init(wave_pitch(inst), true, true);
    b->hr.load_coefficients();
    b->hr.reset();
    b->w_bp = BLOCK_SIZE;
    b->w_pt = 0;
}
static void waves_step(surge_instance_t *inst, int64_t deadline) {
    RemoteBuild *b = inst->rb;
    auto &storage = inst->synth->storage;
    const float pitch = wave_pitch(inst);
    for (;;) {
        if (b->w_pt < 0) {
            /* spawning and initialising an oscillator is a step of its own:
             * for some types it costs as much as rendering */
            waves_open(inst);
            if (mono_ns() >= deadline) return;
        }
        OscillatorStorage *od = &storage.getPatch().scene[b->w_s].osc[b->w_o];
        if (od->type.val.i != b->w_type) { waves_begin(inst); return; }   /* belt and braces: gen covers this */
        /* the wavetable lock is Surge's; never wait for it here */
        if (b->w_disp && !storage.waveTableDataMutex.try_lock()) return;
        /* 8 points = 32 samples = one oscillator block per step */
        const int end = std::min(WAVE_POINTS, b->w_pt + 8);
        for (; b->w_pt < end; b->w_pt++) {
            float v = 0.f;
            if (b->w_disp) {
                for (int a = 0; a < WAVE_AVG; a++) {
                    if (b->w_bp >= BLOCK_SIZE) {
                        b->w_osc->process_block(pitch);
                        memcpy(b->tmp[0], b->w_osc->output, sizeof(b->tmp[0]));
                        memcpy(b->tmp[1], b->w_osc->output, sizeof(b->tmp[1]));
                        b->hr.process_block_D2(b->tmp[0], b->tmp[1], BLOCK_SIZE_OS);
                        b->w_bp = 0;
                    }
                    v += b->tmp[0][b->w_bp++];
                }
                v /= WAVE_AVG;
            }
            if (b->w_pt) b->w += ",";
            char nb[16];
            snprintf(nb, sizeof(nb), "%.3f", v);
            b->w += nb;
        }
        if (b->w_disp) storage.waveTableDataMutex.unlock();
        if (b->w_pt >= WAVE_POINTS) {
            b->w += "]}";
            waves_drop_osc(b);
            if (++b->w_o >= n_oscs) {
                b->w_o = 0;
                b->w += "]";
                if (++b->w_s >= n_scenes) {
                    b->w += "]}\n";
                    b->w_on = false;
                    b->w_ready = true;
                    return;
                }
                b->w += ",[";
            }
        }
        if (mono_ns() >= deadline) return;
    }
}

static void filters_snapshot(surge_instance_t *inst) {
    RemoteBuild *b = inst->rb;
    auto &patch = inst->synth->storage.getPatch();
    for (int s = 0; s < n_scenes; s++)
        for (int f = 0; f < n_filterunits_per_scene; f++) {
            auto &fu = patch.scene[s].filterunit[f];
            b->f.type[s][f] = fu.type.val.i;
            b->f.sub[s][f] = fu.subtype.val.i;
            b->f.cutoff[s][f] = fu.cutoff.val.f;
            b->f.res[s][f] = fu.resonance.val.f;
        }
    b->f_ready = true;
}

/* Finished work to the worker: O(1) swaps of pre-reserved buffers, never a
 * wait. A slot the worker has not emptied yet is simply tried next block. */
static void remote_handoff(surge_instance_t *inst) {
    RemoteBuild *b = inst->rb;
    RemoteFiles *rf = inst->remote;
    bool kick = false;
    if (b->p_ready && rf->params_full.load(std::memory_order_acquire) == 0) {
        rf->params.swap(b->p);
        b->p.clear();
        b->p_ready = false;
        rf->params_full.store(1, std::memory_order_release);
        kick = true;
    }
    if (b->w_ready && rf->waves_full.load(std::memory_order_acquire) == 0) {
        rf->waves.swap(b->w);
        b->w.clear();
        b->w_ready = false;
        rf->waves_full.store(1, std::memory_order_release);
        kick = true;
    }
    if (b->f_ready && rf->filters_full.load(std::memory_order_acquire) == 0) {
        rf->filters = b->f;
        b->f_ready = false;
        rf->filters_full.store(1, std::memory_order_release);
        kick = true;
    }
    if (kick) rf->cv.notify_one();
}

/* Called at the end of every render_block. */
static void remote_service(surge_instance_t *inst) {
    RemoteBuild *b = inst->rb;
    if (!b || !inst->remote) return;
    SurgeSynthesizer *s = inst->synth;
    /* A MIDI program change: Surge fades out (patchid_queue set), then loads
     * the patch on a thread of its own with halt_engine set. Hands off the
     * patch until it is done. */
    if (s->halt_engine || s->patchid_queue >= 0) {
        waves_drop_osc(b);
        b->gen++;
        return;
    }
    if (s->patch_loaded) adopt_foreign_patch_load(inst);

    if (b->p_on && b->p_gen != b->gen) { b->p_on = false; b->want |= RF_PARAMS; }
    if (b->w_on && b->w_gen != b->gen) { waves_drop_osc(b); b->w_on = false; b->want |= RF_WAVES; }

    remote_handoff(inst);
    /* A type switch (or a load) set just before this block was applied by the
     * process() calls in it -- spawning an oscillator or effect, Surge's own
     * cost. Start the rebuild a block later rather than stack on top of it. */
    if (b->hold) { b->hold = false; return; }
    if (b->want & RF_FILTERS) { b->want &= ~RF_FILTERS; filters_snapshot(inst); }
    if ((b->want & RF_PARAMS) && !b->p_on) { b->want &= ~RF_PARAMS; params_begin(inst); }
    if ((b->want & RF_WAVES) && !b->w_on) { b->want &= ~RF_WAVES; waves_begin(inst); }
    if (b->p_on || b->w_on) {
        const int64_t deadline = mono_ns() + REMOTE_BUDGET_NS;
        if (b->p_on) params_step(inst, deadline);
        if (b->w_on && mono_ns() < deadline) waves_step(inst, deadline);
    }
    remote_handoff(inst);
}

/* presets_index.json: every patch in display order, grouped by category.
 * Built once, in create_instance: the patch list does not change after it. */
static void build_presets_json(surge_instance_t *inst, std::string &j) {
    auto &storage = inst->synth->storage;
    j.clear();
    j += "{\"count\":"; jint(j, (int)storage.patchOrdering.size()); j += ",\"categories\":[";
    int lastCat = -2;
    bool firstCat = true, firstP = true;
    for (size_t i = 0; i < storage.patchOrdering.size(); i++) {
        const int raw = storage.patchOrdering[i];
        if (raw < 0 || raw >= (int)storage.patch_list.size()) continue;
        const Patch &pt = storage.patch_list[raw];
        if (pt.category != lastCat) {
            if (lastCat != -2) j += "]}";
            lastCat = pt.category;
            std::string cname = "Uncategorised";
            auto it = inst->category_id_to_name->find(pt.category);
            if (it != inst->category_id_to_name->end()) cname = it->second;
            const char *kind = pt.category >= storage.firstUserCategory ? "user"
                             : pt.category >= storage.firstThirdPartyCategory ? "thirdparty" : "factory";
            j += firstCat ? "{" : ",{"; firstCat = false;
            j += "\"name\":"; jstr(j, cname.c_str()); j += ",\"kind\":\""; j += kind; j += "\",\"presets\":[";
            firstP = true;
        }
        if (!firstP) j += ",";
        firstP = false;
        j += "{\"i\":"; jint(j, (int)i); j += ",\"n\":"; jstr(j, pt.name.c_str());
        if (pt.isFavorite) j += ",\"fav\":true";
        j += "}";
    }
    if (lastCat != -2) j += "]}";
    j += "]}\n";
}

/* Instance ids name the per-instance directory: the lowest free one, so a
 * set switch reuses directories instead of accumulating them. */
static std::atomic<uint64_t> g_rids{0};
static int rid_alloc() {
    uint64_t cur = g_rids.load();
    for (;;) {
        if (cur == ~0ull) return -1;
        const int i = __builtin_ctzll(~cur);
        if (g_rids.compare_exchange_weak(cur, cur | (1ull << i))) return i;
    }
}
static void rid_free(int i) {
    if (i >= 0 && i < 64) g_rids.fetch_and(~(1ull << i));
}

static void remote_start(surge_instance_t *inst) {
    inst->rid = rid_alloc();
    if (inst->rid < 0) { plugin_log("remote: no free instance id, Remote UI files disabled"); return; }
    RemoteFiles *rf = new RemoteFiles();
    snprintf(rf->root, sizeof(rf->root), "%s/remote", inst->module_dir);
    snprintf(rf->dir, sizeof(rf->dir), "%s/%d", rf->root, inst->rid);
    rf->params.reserve(REMOTE_PARAMS_RESERVE);
    rf->params_local.reserve(REMOTE_PARAMS_RESERVE);
    rf->waves.reserve(REMOTE_WAVES_RESERVE);
    rf->waves_local.reserve(REMOTE_WAVES_RESERVE);
    rf->filters_json.reserve(16384);
    build_presets_json(inst, rf->presets);
    rf->want_presets.store(true);

    RemoteBuild *b = new RemoteBuild();
    b->p.reserve(REMOTE_PARAMS_RESERVE);
    b->w.reserve(REMOTE_WAVES_RESERVE);

    /* The filter coefficient maker the workers use (no tuning provider, so
     * sst's BasicTuningProvider) fills a function-local static 2^x table on
     * first use, guarded by a plain bool: two instances' workers filling it at
     * once is a data race (ThreadSanitizer, two slots). Fill it here, before
     * any worker exists; pthread_create orders this before their reads, and
     * after it the table is only read. */
    (void)sst::filters::detail::BasicTuningProvider::twoToThe(0.f);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    sched_param sp{};
    sp.sched_priority = 0;
    pthread_attr_setschedparam(&attr, &sp);
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c = 0; c < 3; c++) CPU_SET(c, &set);
    pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
#endif
    if (pthread_create(&rf->th, &attr, remote_thread_main, rf) == 0) rf->started = true;
    else plugin_log("remote: could not start the file thread, Remote UI files disabled");
    pthread_attr_destroy(&attr);

    inst->remote = rf;
    inst->rb = b;
    remote_reshaped(inst);
}

/* destroy_instance runs on the audio callback too. The worker waits on the
 * condition variable (never a bare sleep) and checks `quit` between files and
 * between filter units, so the join below waits for at most one unit of work:
 * a few milliseconds, not a 150 ms debounce. */
static void remote_stop(surge_instance_t *inst) {
    if (RemoteFiles *rf = inst->remote) {
        {
            std::lock_guard<std::mutex> g(rf->m);
            rf->quit.store(true);
        }
        rf->cv.notify_one();
        if (rf->started) pthread_join(rf->th, nullptr);
        delete rf;
        inst->remote = nullptr;
    }
    if (RemoteBuild *b = inst->rb) {
        waves_drop_osc(b);
        delete b;
        inst->rb = nullptr;
    }
    rid_free(inst->rid);
    inst->rid = -1;
}

/* =====================================================================
 * JSON builders for ui_hierarchy and chain_params
 * ===================================================================== */

static void build_ui_hierarchy(surge_instance_t *inst) {
    /* 256KB should be plenty for the hierarchy JSON */
    const int bufsize = 262144;
    inst->ui_hierarchy_json = (char*)malloc(bufsize);
    if (!inst->ui_hierarchy_json) return;

    snprintf(inst->ui_hierarchy_json, bufsize,
        "{"
        "\"modes\":null,"
        "\"levels\":{"
            "\"root\":{"
                "\"list_param\":\"preset\","
                "\"count_param\":\"preset_count\","
                "\"name_param\":\"preset_name\","
                "\"children\":\"main\","
                "\"knobs\":[\"filter1_cutoff\",\"filter1_resonance\",\"filter1_envmod\","
                    "\"env1_attack\",\"env1_decay\",\"env1_sustain\",\"env1_release\",\"volume\"],"
                "\"params\":[]"
            "},"
            "\"main\":{"
                "\"children\":null,"
                "\"knobs\":[\"filter1_cutoff\",\"filter1_resonance\",\"filter1_envmod\","
                    "\"env1_attack\",\"env1_decay\",\"env1_sustain\",\"env1_release\",\"volume\"],"
                "\"params\":["
                    "{\"level\":\"category_jump\",\"label\":\"Jump to Category\"},"
                    "{\"level\":\"osc1\",\"label\":\"Oscillator 1\"},"
                    "{\"level\":\"osc2\",\"label\":\"Oscillator 2\"},"
                    "{\"level\":\"osc3\",\"label\":\"Oscillator 3\"},"
                    "{\"level\":\"mixer\",\"label\":\"Mixer\"},"
                    "{\"level\":\"scene\",\"label\":\"Scene\"},"
                    "{\"level\":\"filter1\",\"label\":\"Filter 1\"},"
                    "{\"level\":\"filter2\",\"label\":\"Filter 2\"},"
                    "{\"level\":\"amp_env\",\"label\":\"Amp Envelope\"},"
                    "{\"level\":\"filt_env\",\"label\":\"Filter Envelope\"},"
                    "{\"level\":\"mpe\",\"label\":\"MPE\"},"
                    "{\"level\":\"lfo1\",\"label\":\"LFO 1\"},"
                    "{\"level\":\"lfo2\",\"label\":\"LFO 2\"},"
                    "{\"level\":\"lfo3\",\"label\":\"LFO 3\"},"
                    "{\"level\":\"lfo4\",\"label\":\"LFO 4\"},"
                    "{\"level\":\"lfo5\",\"label\":\"LFO 5\"},"
                    "{\"level\":\"lfo6\",\"label\":\"LFO 6\"},"
                    "{\"level\":\"slfo1\",\"label\":\"Scene LFO 1\"},"
                    "{\"level\":\"slfo2\",\"label\":\"Scene LFO 2\"},"
                    "{\"level\":\"slfo3\",\"label\":\"Scene LFO 3\"},"
                    "{\"level\":\"slfo4\",\"label\":\"Scene LFO 4\"},"
                    "{\"level\":\"slfo5\",\"label\":\"Scene LFO 5\"},"
                    "{\"level\":\"slfo6\",\"label\":\"Scene LFO 6\"},"
                    "{\"level\":\"mod_0\",\"label\":\"Mod Slot 1\"},"
                    "{\"level\":\"mod_1\",\"label\":\"Mod Slot 2\"},"
                    "{\"level\":\"mod_2\",\"label\":\"Mod Slot 3\"},"
                    "{\"level\":\"mod_3\",\"label\":\"Mod Slot 4\"},"
                    "{\"level\":\"mod_4\",\"label\":\"Mod Slot 5\"},"
                    "{\"level\":\"mod_5\",\"label\":\"Mod Slot 6\"}"
                "]"
            "},"
            "\"category_jump\":{"
                "\"label\":\"Jump to Category\","
                "\"items_param\":\"category_list\","
                "\"select_param\":\"jump_to_category\","
                "\"navigate_to\":\"root\","
                "\"children\":null,"
                "\"knobs\":[],"
                "\"params\":[]"
            "},"
            "\"osc1\":{"
                "\"children\":null,"
                "\"knobs\":[\"osc1_type\",\"osc1_pitch\",\"osc1_param0\",\"osc1_param1\","
                    "\"osc1_param2\",\"osc1_param3\",\"osc1_param4\",\"osc1_param5\"],"
                "\"params\":[\"osc1_type\",\"osc1_octave\",\"osc1_pitch\","
                    "\"osc1_param0\",\"osc1_param1\",\"osc1_param2\","
                    "\"osc1_param3\",\"osc1_param4\",\"osc1_param5\",\"osc1_param6\","
                    "\"osc1_keytrack\",\"osc1_retrigger\"]"
            "},"
            "\"osc2\":{"
                "\"children\":null,"
                "\"knobs\":[\"osc2_type\",\"osc2_pitch\",\"osc2_param0\",\"osc2_param1\","
                    "\"osc2_param2\",\"osc2_param3\",\"osc2_param4\",\"osc2_param5\"],"
                "\"params\":[\"osc2_type\",\"osc2_octave\",\"osc2_pitch\","
                    "\"osc2_param0\",\"osc2_param1\",\"osc2_param2\","
                    "\"osc2_param3\",\"osc2_param4\",\"osc2_param5\",\"osc2_param6\","
                    "\"osc2_keytrack\",\"osc2_retrigger\"]"
            "},"
            "\"osc3\":{"
                "\"children\":null,"
                "\"knobs\":[\"osc3_type\",\"osc3_pitch\",\"osc3_param0\",\"osc3_param1\","
                    "\"osc3_param2\",\"osc3_param3\",\"osc3_param4\",\"osc3_param5\"],"
                "\"params\":[\"osc3_type\",\"osc3_octave\",\"osc3_pitch\","
                    "\"osc3_param0\",\"osc3_param1\",\"osc3_param2\","
                    "\"osc3_param3\",\"osc3_param4\",\"osc3_param5\",\"osc3_param6\","
                    "\"osc3_keytrack\",\"osc3_retrigger\"]"
            "},"
            "\"mixer\":{"
                "\"children\":null,"
                "\"knobs\":[\"level_o1\",\"level_o2\",\"level_o3\",\"level_noise\","
                    "\"level_ring12\",\"level_ring23\",\"level_pfg\"],"
                "\"params\":[\"level_o1\",\"level_o2\",\"level_o3\","
                    "\"level_noise\",\"level_ring12\",\"level_ring23\",\"level_pfg\","
                    "\"route_o1\",\"route_o2\",\"route_o3\","
                    "\"route_noise\",\"route_ring12\",\"route_ring23\","
                    "\"mute_o1\",\"mute_o2\",\"mute_o3\","
                    "\"mute_noise\",\"mute_ring12\",\"mute_ring23\"]"
            "},"
            "\"filter1\":{"
                "\"children\":null,"
                "\"knobs\":[\"filter1_type\",\"filter1_cutoff\",\"filter1_resonance\","
                    "\"filter1_envmod\",\"filter1_keytrack\",\"filter1_subtype\"],"
                "\"params\":[\"filter1_type\",\"filter1_subtype\",\"filter1_cutoff\","
                    "\"filter1_resonance\",\"filter1_envmod\",\"filter1_keytrack\"]"
            "},"
            "\"filter2\":{"
                "\"children\":null,"
                "\"knobs\":[\"filter2_type\",\"filter2_cutoff\",\"filter2_resonance\","
                    "\"filter2_envmod\",\"filter2_keytrack\",\"filter2_subtype\"],"
                "\"params\":[\"filter2_type\",\"filter2_subtype\",\"filter2_cutoff\","
                    "\"filter2_resonance\",\"filter2_envmod\",\"filter2_keytrack\","
                    "\"f2_cf_is_offset\",\"f2_link_resonance\"]"
            "},"
            "\"amp_env\":{"
                "\"children\":null,"
                "\"knobs\":[\"env1_attack\",\"env1_decay\",\"env1_sustain\",\"env1_release\","
                    "\"env1_attack_shape\",\"env1_decay_shape\",\"env1_release_shape\",\"env1_mode\"],"
                "\"params\":[\"env1_attack\",\"env1_decay\",\"env1_sustain\",\"env1_release\","
                    "\"env1_attack_shape\",\"env1_decay_shape\",\"env1_release_shape\",\"env1_mode\"]"
            "},"
            "\"filt_env\":{"
                "\"children\":null,"
                "\"knobs\":[\"env2_attack\",\"env2_decay\",\"env2_sustain\",\"env2_release\","
                    "\"env2_attack_shape\",\"env2_decay_shape\",\"env2_release_shape\",\"env2_mode\"],"
                "\"params\":[\"env2_attack\",\"env2_decay\",\"env2_sustain\",\"env2_release\","
                    "\"env2_attack_shape\",\"env2_decay_shape\",\"env2_release_shape\",\"env2_mode\"]"
            "},"
            "\"lfo1\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo0_shape\",\"lfo0_rate\",\"lfo0_magnitude\",\"lfo0_deform\","
                    "\"lfo0_phase\",\"lfo0_delay\",\"lfo0_attack\",\"lfo0_decay\"],"
                "\"params\":[\"lfo0_shape\",\"lfo0_rate\",\"lfo0_phase\",\"lfo0_magnitude\","
                    "\"lfo0_deform\",\"lfo0_trigmode\",\"lfo0_unipolar\","
                    "\"lfo0_delay\",\"lfo0_attack\",\"lfo0_hold\","
                    "\"lfo0_decay\",\"lfo0_sustain\",\"lfo0_release\"]"
            "},"
            "\"lfo2\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo1_shape\",\"lfo1_rate\",\"lfo1_magnitude\",\"lfo1_deform\","
                    "\"lfo1_phase\",\"lfo1_delay\",\"lfo1_attack\",\"lfo1_decay\"],"
                "\"params\":[\"lfo1_shape\",\"lfo1_rate\",\"lfo1_phase\",\"lfo1_magnitude\","
                    "\"lfo1_deform\",\"lfo1_trigmode\",\"lfo1_unipolar\","
                    "\"lfo1_delay\",\"lfo1_attack\",\"lfo1_hold\","
                    "\"lfo1_decay\",\"lfo1_sustain\",\"lfo1_release\"]"
            "},"
            "\"lfo3\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo2_shape\",\"lfo2_rate\",\"lfo2_magnitude\",\"lfo2_deform\","
                    "\"lfo2_phase\",\"lfo2_delay\",\"lfo2_attack\",\"lfo2_decay\"],"
                "\"params\":[\"lfo2_shape\",\"lfo2_rate\",\"lfo2_phase\",\"lfo2_magnitude\","
                    "\"lfo2_deform\",\"lfo2_trigmode\",\"lfo2_unipolar\","
                    "\"lfo2_delay\",\"lfo2_attack\",\"lfo2_hold\","
                    "\"lfo2_decay\",\"lfo2_sustain\",\"lfo2_release\"]"
            "},"
            "\"lfo4\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo3_shape\",\"lfo3_rate\",\"lfo3_magnitude\",\"lfo3_deform\","
                    "\"lfo3_phase\",\"lfo3_delay\",\"lfo3_attack\",\"lfo3_decay\"],"
                "\"params\":[\"lfo3_shape\",\"lfo3_rate\",\"lfo3_phase\",\"lfo3_magnitude\","
                    "\"lfo3_deform\",\"lfo3_trigmode\",\"lfo3_unipolar\","
                    "\"lfo3_delay\",\"lfo3_attack\",\"lfo3_hold\","
                    "\"lfo3_decay\",\"lfo3_sustain\",\"lfo3_release\"]"
            "},"
            "\"lfo5\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo4_shape\",\"lfo4_rate\",\"lfo4_magnitude\",\"lfo4_deform\","
                    "\"lfo4_phase\",\"lfo4_delay\",\"lfo4_attack\",\"lfo4_decay\"],"
                "\"params\":[\"lfo4_shape\",\"lfo4_rate\",\"lfo4_phase\",\"lfo4_magnitude\","
                    "\"lfo4_deform\",\"lfo4_trigmode\",\"lfo4_unipolar\","
                    "\"lfo4_delay\",\"lfo4_attack\",\"lfo4_hold\","
                    "\"lfo4_decay\",\"lfo4_sustain\",\"lfo4_release\"]"
            "},"
            "\"lfo6\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo5_shape\",\"lfo5_rate\",\"lfo5_magnitude\",\"lfo5_deform\","
                    "\"lfo5_phase\",\"lfo5_delay\",\"lfo5_attack\",\"lfo5_decay\"],"
                "\"params\":[\"lfo5_shape\",\"lfo5_rate\",\"lfo5_phase\",\"lfo5_magnitude\","
                    "\"lfo5_deform\",\"lfo5_trigmode\",\"lfo5_unipolar\","
                    "\"lfo5_delay\",\"lfo5_attack\",\"lfo5_hold\","
                    "\"lfo5_decay\",\"lfo5_sustain\",\"lfo5_release\"]"
            "},"
            "\"slfo1\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo6_shape\",\"lfo6_rate\",\"lfo6_magnitude\",\"lfo6_deform\","
                    "\"lfo6_phase\",\"lfo6_delay\",\"lfo6_attack\",\"lfo6_decay\"],"
                "\"params\":[\"lfo6_shape\",\"lfo6_rate\",\"lfo6_phase\",\"lfo6_magnitude\","
                    "\"lfo6_deform\",\"lfo6_trigmode\",\"lfo6_unipolar\","
                    "\"lfo6_delay\",\"lfo6_attack\",\"lfo6_hold\","
                    "\"lfo6_decay\",\"lfo6_sustain\",\"lfo6_release\"]"
            "},"
            "\"slfo2\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo7_shape\",\"lfo7_rate\",\"lfo7_magnitude\",\"lfo7_deform\","
                    "\"lfo7_phase\",\"lfo7_delay\",\"lfo7_attack\",\"lfo7_decay\"],"
                "\"params\":[\"lfo7_shape\",\"lfo7_rate\",\"lfo7_phase\",\"lfo7_magnitude\","
                    "\"lfo7_deform\",\"lfo7_trigmode\",\"lfo7_unipolar\","
                    "\"lfo7_delay\",\"lfo7_attack\",\"lfo7_hold\","
                    "\"lfo7_decay\",\"lfo7_sustain\",\"lfo7_release\"]"
            "},"
            "\"slfo3\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo8_shape\",\"lfo8_rate\",\"lfo8_magnitude\",\"lfo8_deform\","
                    "\"lfo8_phase\",\"lfo8_delay\",\"lfo8_attack\",\"lfo8_decay\"],"
                "\"params\":[\"lfo8_shape\",\"lfo8_rate\",\"lfo8_phase\",\"lfo8_magnitude\","
                    "\"lfo8_deform\",\"lfo8_trigmode\",\"lfo8_unipolar\","
                    "\"lfo8_delay\",\"lfo8_attack\",\"lfo8_hold\","
                    "\"lfo8_decay\",\"lfo8_sustain\",\"lfo8_release\"]"
            "},"
            "\"slfo4\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo9_shape\",\"lfo9_rate\",\"lfo9_magnitude\",\"lfo9_deform\","
                    "\"lfo9_phase\",\"lfo9_delay\",\"lfo9_attack\",\"lfo9_decay\"],"
                "\"params\":[\"lfo9_shape\",\"lfo9_rate\",\"lfo9_phase\",\"lfo9_magnitude\","
                    "\"lfo9_deform\",\"lfo9_trigmode\",\"lfo9_unipolar\","
                    "\"lfo9_delay\",\"lfo9_attack\",\"lfo9_hold\","
                    "\"lfo9_decay\",\"lfo9_sustain\",\"lfo9_release\"]"
            "},"
            "\"slfo5\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo10_shape\",\"lfo10_rate\",\"lfo10_magnitude\",\"lfo10_deform\","
                    "\"lfo10_phase\",\"lfo10_delay\",\"lfo10_attack\",\"lfo10_decay\"],"
                "\"params\":[\"lfo10_shape\",\"lfo10_rate\",\"lfo10_phase\",\"lfo10_magnitude\","
                    "\"lfo10_deform\",\"lfo10_trigmode\",\"lfo10_unipolar\","
                    "\"lfo10_delay\",\"lfo10_attack\",\"lfo10_hold\","
                    "\"lfo10_decay\",\"lfo10_sustain\",\"lfo10_release\"]"
            "},"
            "\"slfo6\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo11_shape\",\"lfo11_rate\",\"lfo11_magnitude\",\"lfo11_deform\","
                    "\"lfo11_phase\",\"lfo11_delay\",\"lfo11_attack\",\"lfo11_decay\"],"
                "\"params\":[\"lfo11_shape\",\"lfo11_rate\",\"lfo11_phase\",\"lfo11_magnitude\","
                    "\"lfo11_deform\",\"lfo11_trigmode\",\"lfo11_unipolar\","
                    "\"lfo11_delay\",\"lfo11_attack\",\"lfo11_hold\","
                    "\"lfo11_decay\",\"lfo11_sustain\",\"lfo11_release\"]"
            "},"
            "\"scene\":{"
                "\"children\":null,"
                "\"knobs\":[\"sync_bpm\",\"bpm\",\"octave_transpose\",\"volume\",\"pan\",\"pan2\",\"portamento\","
                    "\"drift\",\"feedback\",\"ws_type\",\"ws_drive\"],"
                "\"params\":[\"sync_bpm\",\"bpm\",\"octave_transpose\",\"octave\",\"pitch\",\"portamento\",\"polymode\","
                    "\"volume\",\"pan\",\"pan2\","
                    "\"fm_switch\",\"fm_depth\",\"drift\",\"noisecol\","
                    "\"feedback\",\"fb_config\",\"f_balance\",\"lowcut\","
                    "\"ws_type\",\"ws_drive\","
                    "\"vca_level\",\"vca_velsense\","
                    "\"pbrange_up\",\"pbrange_dn\","
                    "\"send_fx_1\",\"send_fx_2\",\"send_fx_3\",\"send_fx_4\","
                    "\"octave_transpose\"]"
            "},"
            "\"mpe\":{"
                "\"children\":null,"
                "\"knobs\":[\"mpe_enabled\",\"mpe_pitch_bend_range\"],"
                "\"params\":[\"mpe_enabled\",\"mpe_pitch_bend_range\"]"
            "}"
        "}"
        "}");

    std::string mod_sections = "";
    for (int i = 0; i < NUM_MOD_SLOTS; i++) {
        char buf[512];
        snprintf(buf, sizeof(buf), 
            ",\"mod_%d\":{"
            "\"children\":null,"
            "\"knobs\":[\"mod_%d_enable\",\"mod_%d_source\",\"mod_%d_dest\",\"mod_%d_amount\"],"
            "\"params\":[\"mod_%d_enable\",\"mod_%d_source\",\"mod_%d_dest\",\"mod_%d_amount\"]}",
            i, i, i, i, i, i, i, i, i);
        mod_sections += buf;
    }

    std::string json = inst->ui_hierarchy_json;
    if (json.length() >= 2) {
        json.insert(json.length() - 2, mod_sections); // insert before closing } }
    }
    snprintf(inst->ui_hierarchy_json, bufsize, "%s", json.c_str());
}

static void apply_slot_to_synth(surge_instance_t *inst, int i) {
    auto &slot = inst->mod_slots[i];

    if (slot.is_active) {
        inst->synth->setModDepth01(slot.active_ptag, (modsources)slot.active_modsource, 0, 0, 0.0f);
        slot.is_active = false;
    }

    if (slot.enabled && slot.source_idx > 0 && slot.source_idx < n_modsources &&
        slot.dest_idx > 0 && slot.dest_idx <= inst->param_count) {
        int modsource = slot.source_idx; // 1-based matches modsources enum because 0 is original
        long ptag = inst->params[slot.dest_idx - 1].surge_id.getSynthSideId();
        
        inst->synth->setModDepth01(ptag, (modsources)modsource, 0, 0, slot.amount);
        
        slot.active_ptag = ptag;
        slot.active_modsource = modsource;
        slot.is_active = true;
    }
}

static void build_chain_params(surge_instance_t *inst) {
    /* Build chain_params JSON from the parameter registry.
     * Include preset/octave_transpose plus all registered Surge params. */
    const int bufsize = 262144;
    inst->chain_params_json = (char*)malloc(bufsize);
    if (!inst->chain_params_json) return;

    int offset = 0;
    offset += snprintf(inst->chain_params_json + offset, bufsize - offset,
        "[{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":9999}"
        ",{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-3,\"max\":3}"
        ",{\"key\":\"sync_bpm\",\"short_name\":\"Auto\",\"name\":\"Auto BPM\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]}"
        ",{\"key\":\"bpm\",\"name\":\"BPM\",\"type\":\"int\",\"min\":20,\"max\":300}"
        ",{\"key\":\"mpe_enabled\",\"short_name\":\"Enabled\",\"name\":\"MPE Enabled\",\"type\":\"int\",\"min\":0,\"max\":1}"
        ",{\"key\":\"mpe_pitch_bend_range\",\"short_name\":\"PB Range\",\"name\":\"MPE PB Range\",\"type\":\"int\",\"min\":1,\"max\":96}");

    /* SCENE A ONLY, exactly as before the registry grew: chain_params must fit
     * the host's 64 KB parameter buffer and Scene A alone is 60.8 KB of it. The
     * Remote UI takes its metadata from patch_params.json instead. */
    for (int i = 0; i < inst->param_count && offset < bufsize - 200; i++) {
        if (inst->params[i].scene != 1) continue;
        const char *type_str = (inst->params[i].valtype == 2) ? "float" :
                               (inst->params[i].valtype == 1) ? "int" : "int";
        if (inst->params[i].options_json[0] != '\0') {
            offset += snprintf(inst->chain_params_json + offset, bufsize - offset,
                ",{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"enum\",\"options\":%s}",
                inst->params[i].key,
                inst->params[i].display_name,
                inst->params[i].options_json);
} else {
            if (strcmp(inst->params[i].key, "ws_drive") == 0) {
                offset += snprintf(inst->chain_params_json + offset, bufsize - offset,
                    ",{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%f,\"max\":%f,\"unit\":\"dB\",\"step\":0.1}",
                    inst->params[i].key,
                    inst->params[i].display_name,
                    type_str,
                    inst->params[i].min_val,
                    inst->params[i].max_val);
            } else {
                offset += snprintf(inst->chain_params_json + offset, bufsize - offset,
                    ",{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%f,\"max\":%f}",
                    inst->params[i].key,
                    inst->params[i].display_name,
                    type_str,
                    inst->params[i].min_val,
                    inst->params[i].max_val);
            }
}
    }

    std::string dest_opts = "[\"none\"";
    for (int i = 0; i < inst->param_count; i++) {
        // avoid some settings to keep the JSON size down
        if (inst->params[i].scene != 1) continue;
        if (strncmp(inst->params[i].key, "mod_", 4) == 0) continue;
        dest_opts += ",\"";
        dest_opts += inst->params[i].key;
        dest_opts += "\"";
    }
    dest_opts += "]";

    std::string source_opts = "[\"none\",\"velocity\",\"keytrack\",\"polyaftertouch\",\"aftertouch\",\"pitchbend\",\"modwheel\",\"macro1\",\"macro2\",\"macro3\",\"macro4\",\"macro5\",\"macro6\",\"macro7\",\"macro8\",\"ampeg\",\"filtereg\",\"lfo1\",\"lfo2\",\"lfo3\",\"lfo4\",\"lfo5\",\"lfo6\",\"slfo1\",\"slfo2\",\"slfo3\",\"slfo4\",\"slfo5\",\"slfo6\",\"timbre\",\"releasevelocity\",\"random_bipolar\",\"random_unipolar\",\"alternate_bipolar\",\"alternate_unipolar\",\"breath\",\"expression\",\"sustain\",\"lowest_key\",\"highest_key\",\"latest_key\"]";

    for (int i = 0; i < NUM_MOD_SLOTS && offset < bufsize - 200; i++) {
        offset += snprintf(inst->chain_params_json + offset, bufsize - offset,
            ",{\"key\":\"mod_%d_enable\",\"name\":\"Mod %d Enable\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]}"
            ",{\"key\":\"mod_%d_source\",\"name\":\"Mod %d Source\",\"type\":\"enum\",\"options\":%s}"
            ",{\"key\":\"mod_%d_dest\",\"name\":\"Mod %d Dest\",\"type\":\"enum\",\"options\":%s}"
            ",{\"key\":\"mod_%d_amount\",\"name\":\"Mod %d Amount\",\"type\":\"float\",\"min\":-1,\"max\":1}",
            i, i, i, i, source_opts.c_str(), i, i, dest_opts.c_str(), i, i);
    }

    offset += snprintf(inst->chain_params_json + offset, bufsize - offset, "]");
}

/* =====================================================================
 * Plugin API v2 Implementation
 * ===================================================================== */

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;

    plugin_log("create_instance called");

    surge_instance_t *inst = (surge_instance_t*)calloc(1, sizeof(surge_instance_t));
    if (!inst) return nullptr;

    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    inst->remote = nullptr;
    inst->rb = nullptr;
    inst->rid = -1;
    inst->output_gain = 0.5f;
    inst->sync_bpm = true;
    inst->bpm = 120.0;
    inst->last_clock_time = 0.0;
    inst->clock_count = 0;
    inst->clock_time_sum = 0.0;
    snprintf(inst->preset_name, sizeof(inst->preset_name), "Init");
    inst->error_msg[0] = '\0';

    char msg[256];
    snprintf(msg, sizeof(msg), "module_dir: %s", module_dir);
    plugin_log(msg);

    /* Redirect Surge's paths to a writable location on Move.
     * Surge's sst-plugininfra uses HOME and XDG_DATA_HOME to find paths.
     * Without this, it tries to access /home/root/ which doesn't exist or
     * has wrong permissions on Move. We redirect both to ensure all path
     * lookups (like ~/.Surge XT and ~/.local/share/...) go to writable dirs. */
    char surge_home_path[512];
    snprintf(surge_home_path, sizeof(surge_home_path),
             "/data/UserData/schwung/surge-config");
    setenv("HOME", surge_home_path, 1);
    setenv("XDG_DATA_HOME", surge_home_path, 1);
    snprintf(msg, sizeof(msg), "Set HOME and XDG_DATA_HOME=%s", surge_home_path);
    plugin_log(msg);

    /* Create plugin layer */
    inst->plugin_layer = new MovePluginLayer();

    /* Create SurgeSynthesizer */
    char data_path[512];
    snprintf(data_path, sizeof(data_path), "%s/surge-data", module_dir);

    try {
        inst->synth = new SurgeSynthesizer(inst->plugin_layer, std::string(data_path));
        plugin_log("SurgeSynthesizer created OK");
    } catch (const std::exception &e) {
        snprintf(msg, sizeof(msg), "Exception: %s, trying minimal mode", e.what());
        plugin_log(msg);
        try {
            inst->synth = new SurgeSynthesizer(
                inst->plugin_layer,
                SurgeStorage::skipPatchLoadDataPathSentinel);
        } catch (...) {
            plugin_log("ERROR: All init attempts failed");
            snprintf(inst->error_msg, sizeof(inst->error_msg),
                     "Failed to initialize Surge engine");
            delete inst->plugin_layer;
            free(inst);
            return nullptr;
        }
    } catch (...) {
        plugin_log("Unknown exception, trying minimal mode");
        try {
            inst->synth = new SurgeSynthesizer(
                inst->plugin_layer,
                SurgeStorage::skipPatchLoadDataPathSentinel);
        } catch (...) {
            plugin_log("ERROR: All init attempts failed");
            delete inst->plugin_layer;
            free(inst);
            return nullptr;
        }
    }

    /* Configure for Move audio specs */
    inst->synth->setSamplerate((float)MOVE_SAMPLE_RATE);
    inst->synth->time_data.tempo = 120.0;
    inst->synth->time_data.ppqPos = 0;
    inst->synth->audio_processing_active = true;

    /* get_display asks Surge's user defaults for Middle C on every note-name
     * readout, and with no defaults file on disk each ask is a stat() and a
     * map rebuild. Read it once, here, and pin it: the display formatting now
     * runs on the audio thread. */
    if (auto *ud = inst->synth->storage.userDefaultsProvider.get())
        ud->addOverride(Surge::Storage::MiddleC,
                        Surge::Storage::getUserDefaultValue(&inst->synth->storage, Surge::Storage::MiddleC, 1));

    /* Build parameter registry */
    populate_param_registry(inst);

    /* Category handling */
    inst->category_id_to_name = new std::map<int, std::string>();
    inst->categories = new std::vector<surge_instance_t::category_entry>();
    update_category_list(inst);

    /* Count available patches (using sorted ordering) */
    inst->preset_count = (int)inst->synth->storage.patchOrdering.size();
    if (inst->preset_count > 0) {
        int default_idx = 0;
        for (int i = 0; i < inst->preset_count; i++) {
            int raw_idx = inst->synth->storage.patchOrdering[i];
            if (inst->synth->storage.patch_list[raw_idx].name == "Organ 2 Aftertouch") {
                default_idx = i;
                break;
            }
        }
        load_preset_by_display_index(inst, default_idx);
    }

    /* chain_params' option lists follow the default preset's types, as they
     * always have: bring the registry up to them before it is built (the
     * Remote UI rebuild would, but only once rendering starts). */
    {
        auto &patch = inst->synth->storage.getPatch();
        for (int i = 0; i < inst->param_count; i++) {
            const long sid = inst->params[i].surge_id.getSynthSideId();
            if (sid >= 0 && sid < (long)patch.param_ptr.size() && patch.param_ptr[sid])
                fill_entry_types(&inst->params[i], patch.param_ptr[sid]);
        }
    }

    /* Build JSON strings */
    build_ui_hierarchy(inst);
    build_chain_params(inst);

    /* No preset found: the engine's own init patch is the base. */
    if (inst->preset_count <= 0) capture_baseline(inst);

    /* The Remote UI's files: built on the audio thread, written by a
     * SCHED_OTHER worker */
    remote_start(inst);

    snprintf(msg, sizeof(msg), "Instance created: %d patches, %d categories, %d params",
             inst->preset_count, (int)inst->categories->size(), inst->param_count);
    plugin_log(msg);

    return inst;
}

static void v2_destroy_instance(void *instance) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst) return;

    remote_stop(inst);
    free(inst->ui_hierarchy_json);
    free(inst->chain_params_json);
    delete inst->categories;
    delete inst->category_id_to_name;
    delete inst->synth;
    delete inst->plugin_layer;
    free(inst);
    plugin_log("Instance destroyed");
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst || !inst->synth || len < 1) return;
    (void)source;

    /* Auto BPM Sync calculation */
    if (msg[0] == 0xF8) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + ts.tv_nsec / 1e9;
        
        if (inst->last_clock_time > 0.0) {
            double delta = now - inst->last_clock_time;
            if (delta > 0.005 && delta < 0.2) {
                inst->clock_time_sum += delta;
                inst->clock_count++;
                if (inst->clock_count >= 24) {
                    double avg_delta = inst->clock_time_sum / inst->clock_count;
                    double bpm = 60.0 / (avg_delta * 24.0);
                    if (bpm >= 20.0 && bpm <= 300.0) {
                        if (inst->sync_bpm) {
                            inst->bpm = bpm;
                            inst->synth->time_data.tempo = bpm;
                        }
                    }
                    inst->clock_time_sum = 0.0;
                    inst->clock_count = 0;
                }
            } else {
                inst->clock_time_sum = 0.0;
                inst->clock_count = 0;
            }
        }
        inst->last_clock_time = now;
        return;
    }

    if (len < 2) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t channel = msg[0] & 0x0F;
    uint8_t data1 = msg[1];
    uint8_t data2 = (len > 2) ? msg[2] : 0;

    int note = data1;
    if (status == 0x90 || status == 0x80) {
        note += inst->octave_transpose * 12;
        if (note < 0) note = 0;
        if (note > 127) note = 127;
    }

    switch (status) {
        case 0x90: /* Note On */
            if (data2 > 0) {
                inst->synth->playNote(channel, note, data2, 0);
            } else {
                inst->synth->releaseNote(channel, note, 0);
            }
            break;
        case 0x80: /* Note Off */
            inst->synth->releaseNote(channel, note, data2);
            break;
        case 0xB0: /* CC */
            inst->synth->channelController(channel, data1, data2);
            break;
        case 0xE0: { /* Pitch Bend */
            int bend = ((data2 << 7) | data1) - 8192;
            inst->synth->pitchBend(channel, bend);
            break;
        }
        case 0xD0: /* Channel Aftertouch */
            inst->synth->channelAftertouch(channel, data1);
            break;
        case 0xA0: /* Poly Aftertouch */
            inst->synth->polyAftertouch(channel, data1, data2);
            break;
        case 0xC0: /* Program Change */
            inst->synth->programChange(channel, data1);
            break;
    }
}

static void get_json_array_element(const char *json_array, int index, char *out, int max_len) {
    out[0] = '\0';
    const char *p = json_array;
    if (*p == '[') p++;
    int current = 0;
    while (*p && current < index) {
        if (*p == '"') {
            p++;
            while (*p && !(*p == '"' && *(p-1) != '\\')) p++;
            if (*p == '"') p++;
        } else if (*p == ',') {
            current++;
            p++;
        } else {
            p++;
        }
    }
    while (*p == ' ' || *p == ',') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && !(*p == '"' && *(p-1) != '\\') && i < max_len - 1) {
            if (*p == '\\' && *(p+1) == '"') { p++; } // unescape quote
            out[i++] = *p++;
        }
        out[i] = '\0';
    }
}

static int find_json_array_index(const char *json_array, const char *val) {
    const char *p = json_array;
    if (*p == '[') p++;
    int current = 0;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
        if (*p == '"') {
            p++;
            const char *start = p;
            while (*p && !(*p == '"' && *(p-1) != '\\')) p++;
            int len = p - start;
            
            bool match = true;
            if (len != (int)strlen(val)) match = false;
            else if (strncmp(start, val, len) != 0) match = false;
            
            if (match) return current;
            
            if (*p == '"') p++;
            current++;
        } else {
            p++;
        }
    }
    return 0; // fallback to 0
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst || !inst->synth) return;

    /* State restore */
    if (strcmp(key, "state") == 0) {
        float fval;

        /* Restore preset first (sets all engine params and default mod slots) */
        char pname[128];
        bool preset_loaded = false;
        if (json_get_string(val, "preset_name", pname, sizeof(pname)) == 0) {
            for (int i = 0; i < (int)inst->synth->storage.patchOrdering.size(); i++) {
                int raw_idx = inst->synth->storage.patchOrdering[i];
                if (inst->synth->storage.patch_list[raw_idx].name == pname) {
                    load_preset_by_display_index(inst, i);
                    preset_loaded = true;
                    break;
                }
            }
        }
        if (!preset_loaded && json_get_number(val, "preset", &fval) == 0) {
            int preset = (int)fval;
            load_preset_by_display_index(inst, preset);
        }
        if (json_get_number(val, "octave_transpose", &fval) == 0) {
            inst->octave_transpose = (int)fval;
            if (inst->octave_transpose < -3) inst->octave_transpose = -3;
            if (inst->octave_transpose > 3) inst->octave_transpose = 3;
        }
        if (json_get_number(val, "sync_bpm", &fval) == 0) {
            inst->sync_bpm = (fval != 0.0f);
        }
        if (json_get_number(val, "bpm", &fval) == 0) {
            inst->bpm = fval;
            if (!inst->sync_bpm && inst->synth) {
                inst->synth->time_data.tempo = inst->bpm;
            }
        }
        if (json_get_number(val, "mpe_enabled", &fval) == 0) {
            inst->synth->mpeEnabled = ((int)fval > 0);
        }
        if (json_get_number(val, "mpe_pitch_bend_range", &fval) == 0) {
            int range = (int)fval;
            if (range < 1) range = 1;
            if (range > 96) range = 96;
            inst->synth->storage.mpePitchBendRange = (float)range;
        }

        /* The saved parameters, over the preset just loaded -- TYPES FIRST.
         * Surge defers a type switch: an oscillator type waits in queue_type
         * for loadOscalgos, an FX type in fxsync for loadFx, both run from the
         * next process(), and each RESETS that oscillator's or slot's
         * parameters to the new type's defaults. Set the values in the same
         * pass and that reset lands on top of them. So: the types, then the
         * switches Surge would have deferred, done now (this is the audio
         * thread, where Surge runs them), then everything else. */
        auto restore_param = [&](int i) {
            if (json_get_number(val, inst->params[i].key, &fval) != 0) return;
            if (fval < 0.0f) fval = 0.0f;
            if (fval > 1.0f) fval = 1.0f;
            inst->synth->setParameter01(inst->params[i].surge_id, fval);
        };
        for (int i = 0; i < inst->param_count; i++)
            if (is_type_ctrl(inst->params[i].ctrltype)) restore_param(i);
        inst->synth->loadOscalgos();
        if (inst->synth->load_fx_needed) inst->synth->loadFx(false, false);
        for (int i = 0; i < inst->param_count; i++)
            if (!is_type_ctrl(inst->params[i].ctrltype)) restore_param(i);
        for (int m = 0; m < n_customcontrollers; m++) {
            char mk[16]; snprintf(mk, sizeof(mk), "macro%d", m + 1);
            if (json_get_number(val, mk, &fval) == 0) inst->synth->setMacroParameter01(m, fval < 0 ? 0 : fval > 1 ? 1 : fval);
        }

        /* Mod slots last: an oscillator type switch clears that oscillator's
         * modulation (loadOscalgos -> clear_osc_modulation). */
        for (int i = 0; i < NUM_MOD_SLOTS; i++) {
            char key_enable[32], key_source[32], key_dest[32], key_amount[32];
            snprintf(key_enable, sizeof(key_enable), "mod_%d_enable", i);
            snprintf(key_source, sizeof(key_source), "mod_%d_source", i);
            snprintf(key_dest, sizeof(key_dest), "mod_%d_dest", i);
            snprintf(key_amount, sizeof(key_amount), "mod_%d_amount", i);

            bool changed = false;
            if (json_get_number(val, key_enable, &fval) == 0) {
                inst->mod_slots[i].enabled = ((int)fval != 0);
                changed = true;
            }
            if (json_get_number(val, key_source, &fval) == 0) {
                inst->mod_slots[i].source_idx = (int)fval;
                changed = true;
            }
            if (json_get_number(val, key_dest, &fval) == 0) {
                inst->mod_slots[i].dest_idx = (int)fval;
                changed = true;
            }
            if (json_get_number(val, key_amount, &fval) == 0) {
                inst->mod_slots[i].amount = fval;
                changed = true;
            }
            if (changed) {
                apply_slot_to_synth(inst, i);
            }
        }
        remote_reshaped(inst);
        return;
    }

    /* Macros 1-8: Surge's assignable controllers, 0..1 */
    if (strncmp(key, "macro", 5) == 0 && key[5] >= '1' && key[5] <= '8' && key[6] == '\0') {
        float v = (float)atof(val);
        inst->synth->setMacroParameter01(key[5] - '1', v < 0 ? 0 : v > 1 ? 1 : v);
        return;
    }
    /* The page can ask for every file to be rewritten (e.g. after it reconnects) */
    if (strcmp(key, "remote_refresh") == 0) {
        remote_request(inst, RF_ALL);
        if (inst->remote) { inst->remote->want_presets.store(true); inst->remote->cv.notify_one(); }
        return;
    }

    /* Module-level params */
    if (strcmp(key, "preset") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < inst->preset_count && idx != inst->current_preset) {
            load_preset_by_display_index(inst, idx);
        }
        return;
    }
    if (strcmp(key, "sync_bpm") == 0) {
        inst->sync_bpm = (atoi(val) != 0);
        return;
    }

    if (strcmp(key, "bpm") == 0) {
        inst->bpm = atof(val);
        if (!inst->sync_bpm && inst->synth) {
            inst->synth->time_data.tempo = inst->bpm;
        }
        return;
    }

    if (strcmp(key, "octave_transpose") == 0) {
        inst->octave_transpose = atoi(val);
        if (inst->octave_transpose < -3) inst->octave_transpose = -3;
        if (inst->octave_transpose > 3) inst->octave_transpose = 3;
        return;
    }
    if (strcmp(key, "all_notes_off") == 0) {
        inst->synth->allNotesOff();
        return;
    }
    if (strcmp(key, "mpe_enabled") == 0) {
        bool enable = atoi(val) > 0;
        inst->synth->mpeEnabled = enable;
        char msg[128];
        snprintf(msg, sizeof(msg), "MPE %s", enable ? "enabled" : "disabled");
        plugin_log(msg);
        return;
    }
    if (strcmp(key, "mpe_pitch_bend_range") == 0) {
        int range = atoi(val);
        if (range < 1) range = 1;
        if (range > 96) range = 96;
        inst->synth->storage.mpePitchBendRange = (float)range;
        char msg[128];
        snprintf(msg, sizeof(msg), "MPE pitch bend range = %d semitones", range);
        plugin_log(msg);
        return;
    }

    if (strcmp(key, "jump_to_category") == 0) {
        int idx = atoi(val);
        if (inst->categories && idx >= 0 && idx < (int)inst->categories->size()) {
            load_preset_by_display_index(inst, (*inst->categories)[idx].first_idx);
        }
        return;
    }

    if (strncmp(key, "mod_", 4) == 0) {
        int i = -1;
        if (sscanf(key, "mod_%d_", &i) == 1 && i >= 0 && i < NUM_MOD_SLOTS) {
            if (strstr(key, "_enable")) inst->mod_slots[i].enabled = (strcmp(val, "On") == 0);
            else if (strstr(key, "_source")) {
                std::string source_opts = "[\"none\",\"velocity\",\"keytrack\",\"polyaftertouch\",\"aftertouch\",\"pitchbend\",\"modwheel\",\"macro1\",\"macro2\",\"macro3\",\"macro4\",\"macro5\",\"macro6\",\"macro7\",\"macro8\",\"ampeg\",\"filtereg\",\"lfo1\",\"lfo2\",\"lfo3\",\"lfo4\",\"lfo5\",\"lfo6\",\"slfo1\",\"slfo2\",\"slfo3\",\"slfo4\",\"slfo5\",\"slfo6\",\"timbre\",\"releasevelocity\",\"random_bipolar\",\"random_unipolar\",\"alternate_bipolar\",\"alternate_unipolar\",\"breath\",\"expression\",\"sustain\",\"lowest_key\",\"highest_key\",\"latest_key\"]";
                inst->mod_slots[i].source_idx = find_json_array_index(source_opts.c_str(), val);
            }
            else if (strstr(key, "_dest")) {
                int idx = 0;
                for (int p = 0; p < inst->param_count; p++) {
                    if (strcmp(inst->params[p].key, val) == 0) { idx = p + 1; break; }
                }
                inst->mod_slots[i].dest_idx = idx;
            }
            else if (strstr(key, "_amount")) inst->mod_slots[i].amount = (float)atof(val);
            apply_slot_to_synth(inst, i);
        }
        return;
    }


    /* Generic Surge parameter access */
    surge_param_entry *entry = find_param(inst, key);
    if (entry) {
        float norm_v;
        if (entry->options_json[0] != '\0') {
            int index = find_json_array_index(entry->options_json, val);
            float range = entry->max_val - entry->min_val;
            norm_v = (range > 0) ? ((float)index / range) : 0.0f;
        } else {
            float v = (float)atof(val);
            float range = entry->max_val - entry->min_val;
            norm_v = (range > 0) ? ((v - entry->min_val) / range) : 0.0f;
        }
        if (norm_v < 0.0f) norm_v = 0.0f;
        if (norm_v > 1.0f) norm_v = 1.0f;
        inst->synth->setParameter01(entry->surge_id, norm_v);
        /* A type change renames and retypes its dependants once process() has
         * applied it; the rebuild that follows refreshes the registry entries
         * as it goes. Oscillator and filter edits redraw their pictures. */
        if (is_type_ctrl(entry->ctrltype)) remote_reshaped(inst);
        else if (entry->ctrlgroup == cg_OSC) remote_request(inst, RF_WAVES);
        else if (entry->ctrlgroup == cg_FILTER) remote_request(inst, RF_FILTERS);
    }
}

/* Appends to a fixed buffer; once anything fails to fit, everything after is
 * refused and the whole write is reported as not fitting. */
struct StateOut {
    char *buf;
    int cap, off;
    bool ok;
};
static void sout(StateOut &o, const char *fmt, ...) {
    if (!o.ok) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(o.buf + o.off, (size_t)(o.cap - o.off), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= o.cap - o.off) { o.ok = false; return; }
    o.off += n;
}
static void sout_str(StateOut &o, const char *s) {
    sout(o, "\"");
    for (; *s && o.ok; s++) {
        if (*s == '"' || *s == '\\') sout(o, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) sout(o, "\\u%04x", (unsigned char)*s);
        else sout(o, "%c", *s);
    }
    sout(o, "\"");
}
/* a 0..1 value in as few bytes as %.6f's precision allows: 0.5, 1, 0.123457 */
static void sout_num(StateOut &o, float v) {
    char b[32];
    snprintf(b, sizeof(b), "%.6f", v);
    char *e = b + strlen(b);
    if (strchr(b, '.')) {
        while (e > b && e[-1] == '0') *--e = '\0';
        if (e > b && e[-1] == '.') *--e = '\0';
    }
    if (strcmp(b, "-0") == 0) strcpy(b, "0");
    sout(o, "%s", b);
}
static bool differs(float a, float b) { return fabsf(a - b) > 1e-6f; }

/* `state`: the slot's save blob, and the values the Remote UI is seeded with.
 *
 * It holds the preset (by name and index), the module fields, the six mod
 * slots, and every parameter and macro whose value DIFFERS from what the
 * loaded preset set -- `"sparse":1` tells a reader that an absent key means
 * "as the preset has it" (patch_params.json carries those values as `pv`).
 * Everything at once was ~25 KB; the host keeps at most 16 KB and dropped the
 * rest, edits and all. A restore loads the preset and applies what is here.
 *
 * One exception to "only what differs": where an oscillator, FX slot, filter
 * or LFO has a different TYPE from the preset's, every parameter of that unit
 * is written. The restore switches the type, and the switch resets the unit's
 * parameters to the new type's defaults -- so a value that happens to equal
 * the PRESET's would otherwise come back as the new type's default.
 *
 * A state that still does not fit is refused (-1), never truncated: a cut-off
 * blob is invalid JSON, and the host would store it. */
static int write_state(surge_instance_t *inst, char *buf, int buf_len) {
    if (!inst->synth || buf_len <= 0) return -1;
    /* Surge is loading a patch on its own thread (MIDI program change): the
     * values are in flux and the baseline is about to be re-taken. */
    if (inst->synth->halt_engine || inst->synth->patchid_queue >= 0) return -1;

    StateOut o{buf, std::min(buf_len, SURGE_STATE_MAX + 1), 0, true};
    sout(o, "{\"preset\":%d,\"preset_name\":", inst->current_preset);
    sout_str(o, inst->preset_name);
    sout(o, ",\"octave_transpose\":%d,\"sync_bpm\":%d,\"bpm\":%.1f,\"mpe_enabled\":%d,\"mpe_pitch_bend_range\":%d",
         inst->octave_transpose, inst->sync_bpm ? 1 : 0, inst->bpm,
         (int)inst->synth->mpeEnabled, (int)inst->synth->storage.mpePitchBendRange);
    sout(o, ",\"sparse\":1,\"rid\":%d", inst->rid);

    /* units whose type is not the preset's (the waveshaper's switch resets
     * nothing, so it does not count) */
    struct Unit { int scene, group, entry; };
    Unit retyped[96];
    int n_retyped = 0;
    for (int i = 0; i < inst->param_count && n_retyped < 96; i++) {
        const surge_param_entry &e = inst->params[i];
        if (!is_type_ctrl(e.ctrltype) || e.ctrltype == ct_wstype) continue;
        if (differs(inst->synth->getParameter01(e.surge_id), inst->base_val[i]))
            retyped[n_retyped++] = {e.scene, e.ctrlgroup, e.ctrlgroup_entry};
    }
    auto in_retyped_unit = [&](const surge_param_entry &e) {
        for (int k = 0; k < n_retyped; k++)
            if (retyped[k].scene == e.scene && retyped[k].group == e.ctrlgroup && retyped[k].entry == e.ctrlgroup_entry)
                return true;
        return false;
    };

    for (int i = 0; i < inst->param_count && o.ok; i++) {
        const surge_param_entry &e = inst->params[i];
        const float v = inst->synth->getParameter01(e.surge_id);
        if (!differs(v, inst->base_val[i]) && !in_retyped_unit(e)) continue;
        sout(o, ",\"%s\":", e.key);
        sout_num(o, v);
    }
    for (int m = 0; m < n_customcontrollers && o.ok; m++) {
        const float v = inst->synth->getMacroParameterTarget01(m);
        if (!differs(v, inst->base_macro[m])) continue;
        sout(o, ",\"macro%d\":", m + 1);
        sout_num(o, v);
    }
    for (int i = 0; i < NUM_MOD_SLOTS && o.ok; i++) {
        sout(o, ",\"mod_%d_enable\":%d,\"mod_%d_source\":%d,\"mod_%d_dest\":%d,\"mod_%d_amount\":",
             i, inst->mod_slots[i].enabled ? 1 : 0, i, inst->mod_slots[i].source_idx, i, inst->mod_slots[i].dest_idx, i);
        sout_num(o, inst->mod_slots[i].amount);
    }
    sout(o, "}");
    if (!o.ok) {
        if (!inst->state_overflow_logged) {
            char msg[160];
            snprintf(msg, sizeof(msg), "state does not fit in %d bytes; not saved (edits differ from the preset too much)", o.cap - 1);
            plugin_log(msg);
            inst->state_overflow_logged = true;
        }
        return -1;
    }
    inst->state_overflow_logged = false;
    return o.off;
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst) return -1;

    /* Module-level params */
    if (strncmp(key, "macro", 5) == 0 && key[5] >= '1' && key[5] <= '8' && key[6] == '\0' && inst->synth)
        return snprintf(buf, buf_len, "%.6f", inst->synth->getMacroParameterTarget01(key[5] - '1'));
    if (strcmp(key, "preset") == 0)
        return snprintf(buf, buf_len, "%d", inst->current_preset);
    if (strcmp(key, "sync_bpm") == 0)
        return snprintf(buf, buf_len, "%d", inst->sync_bpm ? 1 : 0);
    if (strcmp(key, "bpm") == 0)
        return snprintf(buf, buf_len, "%.1f", inst->bpm);
    if (strcmp(key, "preset_count") == 0)
        return snprintf(buf, buf_len, "%d", inst->preset_count);
    if (strcmp(key, "preset_name") == 0)
        return snprintf(buf, buf_len, "%s", inst->preset_name);
    if (strcmp(key, "name") == 0)
        return snprintf(buf, buf_len, "Surge XT");
    if (strcmp(key, "octave_transpose") == 0)
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
    if (strcmp(key, "mpe_enabled") == 0)
        return snprintf(buf, buf_len, "%d", inst->synth ? (int)inst->synth->mpeEnabled : 0);
    if (strcmp(key, "mpe_pitch_bend_range") == 0)
        return snprintf(buf, buf_len, "%d", inst->synth ? (int)inst->synth->storage.mpePitchBendRange : 48);

    if (strncmp(key, "mod_", 4) == 0) {
        int i = -1;
        if (sscanf(key, "mod_%d_", &i) == 1 && i >= 0 && i < NUM_MOD_SLOTS) {
            if (strstr(key, "_enable")) {
                return snprintf(buf, buf_len, "%s", inst->mod_slots[i].enabled ? "On" : "Off");
            } else if (strstr(key, "_source")) {
                std::string source_opts = "[\"none\",\"velocity\",\"keytrack\",\"polyaftertouch\",\"aftertouch\",\"pitchbend\",\"modwheel\",\"macro1\",\"macro2\",\"macro3\",\"macro4\",\"macro5\",\"macro6\",\"macro7\",\"macro8\",\"ampeg\",\"filtereg\",\"lfo1\",\"lfo2\",\"lfo3\",\"lfo4\",\"lfo5\",\"lfo6\",\"slfo1\",\"slfo2\",\"slfo3\",\"slfo4\",\"slfo5\",\"slfo6\",\"timbre\",\"releasevelocity\",\"random_bipolar\",\"random_unipolar\",\"alternate_bipolar\",\"alternate_unipolar\",\"breath\",\"expression\",\"sustain\",\"lowest_key\",\"highest_key\",\"latest_key\"]";
                char src_str[128];
                get_json_array_element(source_opts.c_str(), inst->mod_slots[i].source_idx, src_str, sizeof(src_str));
                return snprintf(buf, buf_len, "%s", src_str);
            } else if (strstr(key, "_dest")) {
                int d_idx = inst->mod_slots[i].dest_idx;
                if (d_idx > 0 && d_idx <= inst->param_count) {
                    return snprintf(buf, buf_len, "%s", inst->params[d_idx - 1].key);
                } else {
                    return snprintf(buf, buf_len, "none");
                }
            } else if (strstr(key, "_amount")) {
                return snprintf(buf, buf_len, "%.2f", inst->mod_slots[i].amount);
            }
        }
        return -1;
    }

    if (strcmp(key, "category_list") == 0) {
        if (!inst->categories) return snprintf(buf, buf_len, "[]");
        std::string json = "[";
        for (size_t i = 0; i < inst->categories->size(); i++) {
            if (i > 0) json += ",";
            json += "{\"index\":" + std::to_string(i) + ",\"label\":\"";
            for (const char *p = (*inst->categories)[i].name; *p; p++) {
                if (*p == '"' || *p == '\\') {
                    json += '\\';
                }
                json += *p;
            }
            json += "\"}";
        }
        json += "]";
        int len = (int)json.size();
        if (len < buf_len) {
            strcpy(buf, json.c_str());
            return len;
        }
        return -1;
    }

    if (strcmp(key, "bank_name") == 0) {
        return snprintf(buf, buf_len, "%s", get_current_preset_category(inst));
    }

    if (strcmp(key, "state") == 0) return write_state(inst, buf, buf_len);

    /* Pre-built JSON responses */
    if (strcmp(key, "ui_hierarchy") == 0 && inst->ui_hierarchy_json) {
        int len = strlen(inst->ui_hierarchy_json);
        if (len < buf_len) { strcpy(buf, inst->ui_hierarchy_json); return len; }
        return -1;
    }
    if (strcmp(key, "chain_params") == 0 && inst->chain_params_json) {
        int len = strlen(inst->chain_params_json);
        if (len < buf_len) { strcpy(buf, inst->chain_params_json); return len; }
        return -1;
    }

    /* Generic Surge parameter access */
    surge_param_entry *entry = find_param(inst, key);
    if (entry) {
        float norm_v = inst->synth->getParameter01(entry->surge_id);
        if (entry->options_json[0] != '\0') {
            float range = entry->max_val - entry->min_val;
            int index = (int)std::round(norm_v * range);
            char opt_str[128];
            get_json_array_element(entry->options_json, index, opt_str, sizeof(opt_str));
            return snprintf(buf, buf_len, "%s", opt_str);
        } else {
            float range = entry->max_val - entry->min_val;
            float abs_v = (norm_v * range) + entry->min_val;
            return snprintf(buf, buf_len, "%.6f", abs_v);
        }
    }

    return -1;
}

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst || !inst->synth) {
        memset(out_interleaved_lr, 0, frames * 4);
        return;
    }

    int out_idx = 0;
    int remaining = frames;

    while (remaining > 0) {
        int chunk = (remaining > BLOCK_SIZE) ? BLOCK_SIZE : remaining;

        inst->synth->process();

        for (int i = 0; i < chunk; i++) {
            float left = inst->synth->output[0][i] * inst->output_gain;
            float right = inst->synth->output[1][i] * inst->output_gain;

            int32_t l = (int32_t)(left * 32767.0f);
            int32_t r = (int32_t)(right * 32767.0f);
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;

            out_interleaved_lr[out_idx * 2] = (int16_t)l;
            out_interleaved_lr[out_idx * 2 + 1] = (int16_t)r;
            out_idx++;
        }

        remaining -= chunk;
    }

    /* the Remote UI's share of this block: bounded by REMOTE_BUDGET_NS */
    remote_service(inst);
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst || inst->error_msg[0] == '\0') return 0;
    return snprintf(buf, buf_len, "%s", inst->error_msg);
}

/* =====================================================================
 * Plugin API v2 export
 * ===================================================================== */

static plugin_api_v2_t g_plugin_api_v2;

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_plugin_api_v2, 0, sizeof(g_plugin_api_v2));
    g_plugin_api_v2.api_version = MOVE_PLUGIN_API_VERSION_2;
    g_plugin_api_v2.create_instance = v2_create_instance;
    g_plugin_api_v2.destroy_instance = v2_destroy_instance;
    g_plugin_api_v2.on_midi = v2_on_midi;
    g_plugin_api_v2.set_param = v2_set_param;
    g_plugin_api_v2.get_param = v2_get_param;
    g_plugin_api_v2.get_error = v2_get_error;
    g_plugin_api_v2.render_block = v2_render_block;

    return &g_plugin_api_v2;
}
