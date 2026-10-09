#pragma once
// Loads ingress adapters: built-ins by name, or shared libraries by path,
// all through the C API in ingress/api.h.
#include "ingress/api.h"
#include <memory>
#include <string>
#include <vector>

struct IngressSpec {
    std::string type;        // "boe", "jsonl", or "plugin"
    std::string path;        // plugin: shared library path
    std::string config_json; // this adapter's config object, as text
};

class IngressPlugin {
  public:
    // Built-in adapters register their entry points here.
    struct Entry {
        mktsim_ingress_init_fn init;
        mktsim_ingress_start_fn start;
        mktsim_ingress_stop_fn stop;
        mktsim_ingress_destroy_fn destroy;
    };

    IngressPlugin(std::string name, Entry entry, void *dl_handle);
    ~IngressPlugin();
    IngressPlugin(const IngressPlugin &) = delete;
    IngressPlugin &operator=(const IngressPlugin &) = delete;

    // Opens a shared library and resolves the four symbols. Throws on error.
    static std::unique_ptr<IngressPlugin> load(const std::string &path);
    static std::unique_ptr<IngressPlugin> builtin(const std::string &name, Entry entry);

    bool init(const mktsim_exchange_api *api, mktsim_exchange *ex, const std::string &config_json);
    bool start();
    void stop();
    const std::string &name() const { return name_; }

  private:
    std::string name_;
    Entry entry_;
    void *dl_;
    void *state_ = nullptr;
    bool started_ = false;
};
