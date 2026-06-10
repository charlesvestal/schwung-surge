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

/* Plugin API definitions */
extern "C" {
#include <stdint.h>

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

#define MAX_SURGE_PARAMS 300

struct surge_param_entry {
    char key[48];             /* Parameter key, e.g. "osc1_pitch" */
    char display_name[48];    /* Display name, e.g. "Osc 1 Pitch" */
    SurgeSynthesizer::ID surge_id;
    int valtype;              /* 0=int, 1=bool, 2=float */
    float min_val;
    float max_val;
    char options_json[4096];
    int param_id_in_scene;
};

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

/* =====================================================================
 * Parameter registry population
 * ===================================================================== */

static void populate_param_registry(surge_instance_t *inst) {
    if (!inst->synth) return;

    auto &patch = inst->synth->storage.getPatch();
    int n_params = (int)patch.param_ptr.size();
    inst->param_count = 0;

    for (int i = 0; i < n_params && inst->param_count < MAX_SURGE_PARAMS; i++) {
        Parameter *p = patch.param_ptr[i];
        if (!p) continue;
        if (p->scene != 1) continue; /* Scene A only */

        SurgeSynthesizer::ID id;
        if (!inst->synth->fromSynthSideId(i, id)) continue;

        surge_param_entry *entry = &inst->params[inst->param_count];

        /* Key = storage name minus "a_" prefix */
        const char *sname = p->get_storage_name();
        if (sname[0] == 'a' && sname[1] == '_') {
            strncpy(entry->key, sname + 2, sizeof(entry->key) - 1);
        } else {
            strncpy(entry->key, sname, sizeof(entry->key) - 1);
        }
        entry->key[sizeof(entry->key) - 1] = '\0';

        /* Display name from full name */
        const char *fname = p->get_full_name();
        strncpy(entry->display_name, fname, sizeof(entry->display_name) - 1);
        entry->display_name[sizeof(entry->display_name) - 1] = '\0';

        entry->surge_id = id;
        entry->valtype = p->valtype;
        entry->min_val = (p->valtype == 2) ? p->val_min.f : (float)p->val_min.i;
        entry->max_val = (p->valtype == 2) ? p->val_max.f : (float)p->val_max.i;
        entry->options_json[0] = '\0';
        entry->param_id_in_scene = p->param_id_in_scene;
                if (strcmp(entry->key, "ws_type") == 0) {
            std::string opts = "[";
            opts += "\"Off\",\"Soft\",\"Hard\",\"Asymmetric\",\"Sine\",\"Digital\",\"OJD\",\"Fuzz\",\"Fuzz+Octave\",";
            opts += "\"K35\",\"Distortion\",\"Distortion+Asym\",\"Tube\",\"Tube2\",\"Clip\",\"Fold\",\"Waveshaper\",\"S-Curve\",";
            opts += "\"Sinusoid\",\"Chebyshev\",\"Chebyshev2\",\"Chebyshev3\",\"Chebyshev4\",\"Chebyshev5\",\"Chebyshev6\",";
            opts += "\"Symmetric Clip\",\"Symmetric Fold\",\"Asymmetric Clip\",\"Asymmetric Fold\",\"Soft Clip\",\"Soft Fold\",";
            opts += "\"Hard Clip\",\"Hard Fold\",\"Wavefolder\",\"Wavefolder 2\",\"Wavefolder 3\",\"Wavefolder 4\",\"Wavefolder 5\",";
            opts += "\"Wavefolder 6\",\"Bitcrusher\",\"Bitcrusher 2\",\"Sample & Hold\",\"Decimator\",\"Slew Rate Limiter\"";
            opts += "]";
            strncpy(entry->options_json, opts.c_str(), sizeof(entry->options_json) - 1);
        } else if ((p->valtype == 0 || p->valtype == 1) && p->ctrltype != ct_filtersubtype) {
            int min_val = p->val_min.i;
            int max_val = p->val_max.i;
            if (max_val > min_val && max_val - min_val < 128) {
                std::string opts = "[";
                for (int v = min_val; v <= max_val; v++) {
                    if (v > min_val) opts += ",";
                    float ef = (max_val > min_val) ? (float)(v - min_val) / (float)(max_val - min_val) : 0.0f;
                    std::string s = p->get_display(true, ef);
                    std::string esc;
                    for (char c : s) {
                        if (c == '"') esc += "\\\"";
                        else if (c == '\\') esc += "\\\\";
                        else esc += c;
                    }
                    opts += "\"" + esc + "\"";
                }
                opts += "]";
                strncpy(entry->options_json, opts.c_str(), sizeof(entry->options_json) - 1);
            }
        }

        inst->param_count++;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Registered %d Scene A parameters", inst->param_count);
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

/* Find a parameter entry by key */
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

static void load_preset_by_display_index(surge_instance_t *inst, int display_idx) {
    if (!inst->synth) return;

    auto &storage = inst->synth->storage;
    if (display_idx < 0 || display_idx >= (int)storage.patchOrdering.size()) return;

    int raw_idx = storage.patchOrdering[display_idx];
    inst->synth->loadPatch(raw_idx);
    inst->current_preset = display_idx;

    auto &patch = storage.getPatch();
    const char *name = patch.name.c_str();
    if (name && name[0]) {
        strncpy(inst->preset_name, name, sizeof(inst->preset_name) - 1);
        inst->preset_name[sizeof(inst->preset_name) - 1] = '\0';
    } else {
        snprintf(inst->preset_name, sizeof(inst->preset_name), "Init");
    }

    /* Re-populate parameter registry (param IDs may shift after patch load) */
    populate_param_registry(inst);

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
                "\"knobs\":[\"volume\",\"pan\",\"pan2\",\"portamento\","
                    "\"drift\",\"feedback\",\"ws_type\",\"ws_drive\"],"
                "\"params\":[\"octave\",\"pitch\",\"portamento\",\"polymode\","
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

    if (slot.enabled && slot.source_idx > 0 && slot.dest_idx > 0) {
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
        ",{\"key\":\"mpe_enabled\",\"name\":\"MPE Enabled\",\"type\":\"int\",\"min\":0,\"max\":1}"
        ",{\"key\":\"mpe_pitch_bend_range\",\"name\":\"MPE PB Range\",\"type\":\"int\",\"min\":1,\"max\":96}");

    for (int i = 0; i < inst->param_count && offset < bufsize - 200; i++) {
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
    inst->output_gain = 0.5f;
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

    /* Build parameter registry */
    populate_param_registry(inst);

    /* Category handling */
    inst->category_id_to_name = new std::map<int, std::string>();
    inst->categories = new std::vector<surge_instance_t::category_entry>();
    update_category_list(inst);

    /* Count available patches (using sorted ordering) */
    inst->preset_count = (int)inst->synth->storage.patchOrdering.size();
    if (inst->preset_count > 0) {
        load_preset_by_display_index(inst, 0);
    }

    /* Build JSON strings */
    build_ui_hierarchy(inst);
    build_chain_params(inst);

    snprintf(msg, sizeof(msg), "Instance created: %d patches, %d categories, %d params",
             inst->preset_count, (int)inst->categories->size(), inst->param_count);
    plugin_log(msg);

    return inst;
}

static void v2_destroy_instance(void *instance) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst) return;

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
    if (!inst || !inst->synth || len < 2) return;
    (void)source;

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
        if (json_get_number(val, "preset", &fval) == 0) {
            int idx = (int)fval;
            if (idx >= 0 && idx < inst->preset_count) {
                load_preset_by_display_index(inst, idx);
            }
        }

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
        if (json_get_number(val, "octave_transpose", &fval) == 0) {
            inst->octave_transpose = (int)fval;
            if (inst->octave_transpose < -3) inst->octave_transpose = -3;
            if (inst->octave_transpose > 3) inst->octave_transpose = 3;
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
        /* Restore all registered params (overrides preset values with saved tweaks) */
        for (int i = 0; i < inst->param_count; i++) {
            if (json_get_number(val, inst->params[i].key, &fval) == 0) {
                if (fval < 0.0f) fval = 0.0f;
                if (fval > 1.0f) fval = 1.0f;
                inst->synth->setParameter01(inst->params[i].surge_id, fval);
            }
        }
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
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst) return -1;

    /* Module-level params */
    if (strcmp(key, "preset") == 0)
        return snprintf(buf, buf_len, "%d", inst->current_preset);
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

    /* State serialization — includes all registered params for full save/restore */
    if (strcmp(key, "state") == 0) {
        int offset = 0;
        offset += snprintf(buf + offset, buf_len - offset,
            "{\"preset\":%d,\"octave_transpose\":%d,\"mpe_enabled\":%d,\"mpe_pitch_bend_range\":%d",
            inst->current_preset, inst->octave_transpose,
            inst->synth ? (int)inst->synth->mpeEnabled : 0,
            inst->synth ? (int)inst->synth->storage.mpePitchBendRange : 48);

        for (int i = 0; i < inst->param_count && offset < buf_len - 60; i++) {
            float v = inst->synth->getParameter01(inst->params[i].surge_id);
            offset += snprintf(buf + offset, buf_len - offset,
                ",\"%s\":%.6f", inst->params[i].key, v);
        }

        for (int i = 0; i < NUM_MOD_SLOTS && offset < buf_len - 200; i++) {
            offset += snprintf(buf + offset, buf_len - offset,
                ",\"mod_%d_enable\":%d,\"mod_%d_source\":%d,\"mod_%d_dest\":%d,\"mod_%d_amount\":%.6f",
                i, inst->mod_slots[i].enabled ? 1 : 0,
                i, inst->mod_slots[i].source_idx,
                i, inst->mod_slots[i].dest_idx,
                i, inst->mod_slots[i].amount);
        }

        offset += snprintf(buf + offset, buf_len - offset, "}");
        return offset;
    }

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
