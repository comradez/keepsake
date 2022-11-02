#include "config.h"
#include "file_util.h"
#include "keyframe.h"
#include "test_util.h"
#include <iostream>
#include <toml.hpp>

struct ConfigServiceInternal
{
    void parse_file(const fs::path &file_path);
    void parse(std::string_view str);

    float load_float_field(const toml::node_view<const toml::node> &args, float time = 0.0f);
    vec2 load_vec2_field(const toml::node_view<const toml::node> &args, bool force_normalize, float time = 0.0f);
    vec3 load_vec3_field(const toml::node_view<const toml::node> &args, bool force_normalize, float time = 0.0f);
    Transform load_transform_field(const toml::node_view<const toml::node> &args, float time = 0.0f);

    fs::path output_directory() const;
    void run_all_tasks() const;

    toml::parse_result cfg;

    ConfigurableTable asset_table;

    std::unordered_map<const toml::node *, KeyframeFloat> float_fields;
    std::unordered_map<const toml::node *, KeyframeVec2> vec2_fields;
    std::unordered_map<const toml::node *, KeyframeVec3> vec3_fields;

    std::unordered_map<std::string, ConfigTask> task_factory;
};

void ConfigServiceInternal::parse_file(const fs::path &file_path)
{
    try {
        cfg = toml::parse_file(file_path.string());
    } catch (const toml::parse_error &err) {
        std::cerr << "Parsing failed:\n" << err << "\n";
    }
}

void ConfigServiceInternal::parse(std::string_view str)
{
    try {
        cfg = toml::parse(str);
    } catch (const toml::parse_error &err) {
        std::cerr << "Parsing failed:\n" << err << "\n";
    }
}

float ConfigServiceInternal::load_float_field(const toml::node_view<const toml::node> &args, float time)
{
    if (args.is_number()) {
        return *args.value<float>();
    } else {
        auto it = float_fields.find(args.node());
        if (it == float_fields.end()) {
            const toml::array &times = *args["times"].as_array();
            const toml::array &values = *args["values"].as_array();
            KeyframeFloat field;
            field.times.resize(times.size());
            field.values.resize(values.size());
            for (int i = 0; i < times.size(); ++i) {
                field.times[i] = *times[i].value<float>();
                field.values[i] = *values[i].value<float>();
            }
            it = float_fields.insert({args.node(), std::move(field)}).first;
        }
        return it->second.eval(time);
    }
}

vec2 ConfigServiceInternal::load_vec2_field(const toml::node_view<const toml::node> &args, bool force_normalize,
                                            float time)
{
    if (args.is_array()) {
        const toml::array &components = *args.as_array();
        vec2 v;
        for (int i = 0; i < 2; ++i)
            v[i] = *components[i].value<float>();
        if (force_normalize)
            v.normalize();
        return v;
    } else {
        auto it = vec2_fields.find(args.node());
        if (it == vec2_fields.end()) {
            const toml::array &times = *args["times"].as_array();
            const toml::array &values = *args["values"].as_array();
            KeyframeVec2 field;
            field.times.resize(times.size());
            field.values.resize(values.size());
            for (int i = 0; i < times.size(); ++i) {
                field.times[i] = *times[i].value<float>();
                const toml::array &value = *values[i].as_array();
                for (int j = 0; j < 3; ++j)
                    field.values[i][j] = *value[j].value<float>();
                if (force_normalize)
                    field.values[i].normalize();
            }
            it = vec2_fields.insert({args.node(), std::move(field)}).first;
        }
        return it->second.eval(time);
    }
}

vec3 ConfigServiceInternal::load_vec3_field(const toml::node_view<const toml::node> &args, bool force_normalize,
                                            float time)
{
    if (args.is_array()) {
        const toml::array &components = *args.as_array();
        vec3 v;
        for (int i = 0; i < 3; ++i)
            v[i] = *components[i].value<float>();
        if (force_normalize)
            v.normalize();
        return v;
    } else {
        auto it = vec3_fields.find(args.node());
        if (it == vec3_fields.end()) {
            const toml::array &times = *args["times"].as_array();
            const toml::array &values = *args["values"].as_array();
            KeyframeVec3 field;
            field.times.resize(times.size());
            field.values.resize(values.size());
            for (int i = 0; i < times.size(); ++i) {
                field.times[i] = *times[i].value<float>();
                const toml::array &value = *values[i].as_array();
                for (int j = 0; j < 3; ++j)
                    field.values[i][j] = *value[j].value<float>();
                if (force_normalize)
                    field.values[i].normalize();
            }
            it = vec3_fields.insert({args.node(), std::move(field)}).first;
        }
        return it->second.eval(time);
    }
}

Transform ConfigServiceInternal::load_transform_field(const toml::node_view<const toml::node> &args, float time)
{
    // TODO: keyframe this

    const toml::table &table = *args.as_table();

    Transform transform;
    vec3 scale = vec3::Ones();
    if (table.contains("scale")) {
        const auto &s = *table["scale"].as_array();
        for (int i = 0; i < 3; ++i)
            scale[i] = *s[i].value<float>();
    }
    quat rotation = quat::Identity();
    if (table.contains("rotation")) {
        const auto &r = *table["rotation"].as_array();
        float roll = to_radian(*r[0].value<float>());
        float pitch = to_radian(*r[1].value<float>());
        float yaw = to_radian(*r[2].value<float>());
        using Eigen::AngleAxisf;
        rotation = AngleAxisf(roll, vec3::UnitX()) * AngleAxisf(pitch, vec3::UnitY()) * AngleAxisf(yaw, vec3::UnitZ());
    }
    vec3 translation = vec3::Zero();
    if (table.contains("translation")) {
        const auto &t = *table["translation"].as_array();
        for (int i = 0; i < 3; ++i)
            translation[i] = *t[i].value<float>();
    }
    return Transform(scale_rotate_translate(scale, rotation, translation));
}

fs::path ConfigServiceInternal::output_directory() const
{
    if (cfg.contains("output_dir")) {
        return fs::path(*cfg["output_dir"].value<std::string>());
    } else {
        return fs::path(current_time_and_date());
    }
}

void ConfigServiceInternal::run_all_tasks() const
{
    fs::path output_dir = output_directory();
    if (!fs::is_directory(output_dir) || !fs::exists(output_dir)) {
        if (!fs::create_directory(output_dir)) {
            printf("Failed to create directory %s\n", output_dir.string().c_str());
            return;
        }
        printf("Created output directory [%s]\n", output_dir.string().c_str());
    }
    if (!cfg.contains("task")) {
        printf("No task to run\n");
        return;
    }
    const toml::array &task_array = *cfg["task"].as_array();
    for (int task_id = 0; task_id < (int)task_array.size(); ++task_id) {
        fs::path task_dir;
        toml::table task_table = *task_array[task_id].as_table();
        if (task_table.contains("task_dir")) {
            task_dir = fs::path(*task_table["task_dir"].value<std::string>());
        } else {
            task_dir = string_format("task_%d", task_id);
        }
        task_dir = output_dir / task_dir;

        if (!fs::is_directory(task_dir) || !fs::exists(task_dir)) {
            if (!fs::create_directory(task_dir)) {
                printf("Failed to create directory %s\n", task_dir.string().c_str());
                return;
            }
        }

        if (task_table.contains("override")) {
            int base_task_id = *task_table["override"].value<int>();
            ASSERT(base_task_id < task_id);
            const toml::table &base_task_table = *task_array[base_task_id].as_table();
            ASSERT(!base_task_table.contains("override"));
            toml::table override_table = base_task_table;
            task_table.for_each([&](const toml::key &key, auto &&val) {
                if (key != "override")
                    override_table.insert_or_assign(key, val);
            });
            task_table = std::move(override_table);
        }

        printf("Next task: \n");
        std::cout << task_table << "\n" << std::endl;
        std::ofstream write_config(task_dir / "config.toml");
        write_config << task_table << std::endl;

        std::string type = *task_table["type"].value<std::string>();
        const ConfigTask &task = task_factory.at(type);

        toml::node_view<const toml::node> view(task_table);
        ConfigArgs args(std::make_unique<ConfigArgsInternal>(const_cast<ConfigServiceInternal *>(this), view));
        task(args, task_dir, task_id);

        printf("Saving output to [%s]\n\n", task_dir.string().c_str());
    }
}

void ConfigurableTable::load(ConfigServiceInternal &service)
{
    for (const auto &[field, parser] : parsers) {
        if (service.cfg.contains(field)) {
            const toml::table &table = *service.cfg[field].as_table();
            table.for_each([&](const toml::key &key, const toml::table &val) {
                toml::node_view<const toml::node> view(val);
                ConfigArgs args(std::make_unique<ConfigArgsInternal>(&service, view));
                auto asset = parser(args);
                std::string path = field + "." + std::string(key.str());
                assets.insert({std::move(path), std::move(asset)});
            });
        }
    }
}

ConfigService::~ConfigService() = default;
ConfigService::ConfigService() : service(std::make_unique<ConfigServiceInternal>()) {}

void ConfigService::parse_file(const fs::path &file_path) { service->parse_file(file_path); }

void ConfigService::parse(std::string_view str) { service->parse(str); }

void ConfigService::register_asset(std::string_view prefix, const ConfigurableParser &parser)
{
    service->asset_table.register_parser(prefix, parser);
}

void ConfigService::register_task(std::string_view name, const ConfigTask &task)
{
    service->task_factory.insert({std::string(name), task});
}

void ConfigService::load_assets() { service->asset_table.load(*service); }

const ConfigurableTable &ConfigService::asset_table() const { return service->asset_table; }

fs::path ConfigService::output_directory() const { return service->output_directory(); }

void ConfigService::run_all_tasks() const { return service->run_all_tasks(); }

struct ConfigArgsInternal
{
    ConfigArgsInternal(ConfigServiceInternal *service, toml::node_view<const toml::node> args)
        : service(service), args(args)
    {}

    int load_integer(std::string_view name) const;
    float load_float(std::string_view name) const;
    vec2 load_vec2(std::string_view name, bool force_normalize) const;
    vec3 load_vec3(std::string_view name, bool force_normalize) const;
    Transform load_transform(std::string_view name) const;
    bool load_bool(std::string_view name) const;
    std::string load_string(std::string_view name) const;

    int load_integer(int index) const;
    float load_float(int index) const;
    vec2 load_vec2(int index, bool force_normalize = false) const;
    vec3 load_vec3(int index, bool force_normalize = false) const;
    Transform load_transform(int index) const;
    bool load_bool(int index) const;
    std::string load_string(int index) const;

    ConfigServiceInternal *service;
    toml::node_view<const toml::node> args;
    mutable float time = 0.0f;
};

int ConfigArgsInternal::load_integer(std::string_view name) const { return *args[name].value<int>(); }

int ConfigArgsInternal::load_integer(int index) const { return *args[index].value<int>(); }

float ConfigArgsInternal::load_float(std::string_view name) const
{
    return service->load_float_field(args[name], time);
}

float ConfigArgsInternal::load_float(int index) const { return service->load_float_field(args[index], time); }

vec2 ConfigArgsInternal::load_vec2(std::string_view name, bool force_normalize) const
{
    return service->load_vec2_field(args[name], force_normalize, time);
}

vec2 ConfigArgsInternal::load_vec2(int index, bool force_normalize) const
{
    return service->load_vec2_field(args[index], force_normalize, time);
}

vec3 ConfigArgsInternal::load_vec3(std::string_view name, bool force_normalize) const
{
    return service->load_vec3_field(args[name], force_normalize, time);
}

vec3 ConfigArgsInternal::load_vec3(int index, bool force_normalize) const
{
    return service->load_vec3_field(args[index], force_normalize, time);
}

Transform ConfigArgsInternal::load_transform(std::string_view name) const
{
    return service->load_transform_field(args[name], time);
}

Transform ConfigArgsInternal::load_transform(int index) const
{
    return service->load_transform_field(args[index], time);
}

bool ConfigArgsInternal::load_bool(std::string_view name) const { return *args[name].value<bool>(); }

bool ConfigArgsInternal::load_bool(int index) const { return *args[index].value<bool>(); }

std::string ConfigArgsInternal::load_string(std::string_view name) const { return *args[name].value<std::string>(); }

std::string ConfigArgsInternal::load_string(int index) const { return *args[index].value<std::string>(); }

ConfigArgs::~ConfigArgs() = default;

ConfigArgs::ConfigArgs(std::unique_ptr<ConfigArgsInternal> &&args) : args(std::move(args)) {}

ConfigArgs::ConfigArgs(const ConfigArgs &other) : args(std::make_unique<ConfigArgsInternal>(*other.args)) {}

ConfigArgs ConfigArgs::operator[](std::string_view key) const
{
    toml::node_view<const toml::node> view = args->args[key];

    ConfigArgs child(std::make_unique<ConfigArgsInternal>(args->service, view));
    child.args->time = args->time;
    return child;
}

ConfigArgs ConfigArgs::operator[](int idx) const
{
    toml::node_view<const toml::node> view = args->args[idx];

    ConfigArgs child(std::make_unique<ConfigArgsInternal>(args->service, view));
    child.args->time = args->time;
    return child;
}

size_t ConfigArgs::array_size() const { return args->args.as_array()->size(); }

bool ConfigArgs::contains(std::string_view key) const { return args->args.as_table()->contains(key); }

int ConfigArgs::load_integer(std::string_view name) const { return args->load_integer(name); }

int ConfigArgs::load_integer(int index) const { return args->load_integer(index); }

float ConfigArgs::load_float(std::string_view name) const { return args->load_float(name); }

float ConfigArgs::load_float(int index) const { return args->load_float(index); }

vec2 ConfigArgs::load_vec2(std::string_view name, bool force_normalize) const
{
    return args->load_vec2(name, force_normalize);
}

vec2 ConfigArgs::load_vec2(int index, bool force_normalize) const { return args->load_vec2(index, force_normalize); }

vec3 ConfigArgs::load_vec3(std::string_view name, bool force_normalize) const
{
    return args->load_vec3(name, force_normalize);
}

vec3 ConfigArgs::load_vec3(int index, bool force_normalize) const { return args->load_vec3(index, force_normalize); }

Transform ConfigArgs::load_transform(std::string_view name) const { return args->load_transform(name); }

Transform ConfigArgs::load_transform(int index) const { return args->load_transform(index); }

bool ConfigArgs::load_bool(std::string_view name) const { return args->load_bool(name); }

bool ConfigArgs::load_bool(int index) const { return args->load_bool(index); }

std::string ConfigArgs::load_string(std::string_view name) const { return args->load_string(name); }

std::string ConfigArgs::load_string(int index) const { return args->load_string(index); }

fs::path ConfigArgs::load_path(std::string_view name) const { return fs::path(load_string(name)); }

fs::path ConfigArgs::load_path(int index) const { return args->load_string(index); }

void ConfigArgs::update_time(float t) const { args->time = t; }

const ConfigurableTable &ConfigArgs::asset_table() const { return args->service->asset_table; }