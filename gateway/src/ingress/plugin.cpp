#include "ingress/plugin.hpp"
#include <dlfcn.h>
#include <stdexcept>

IngressPlugin::IngressPlugin(std::string name, Entry entry, void *dl_handle)
    : name_(std::move(name)), entry_(entry), dl_(dl_handle) {}

IngressPlugin::~IngressPlugin() {
    stop();
    if (state_ && entry_.destroy)
        entry_.destroy(state_);
    state_ = nullptr;
    if (dl_)
        ::dlclose(dl_);
}

std::unique_ptr<IngressPlugin> IngressPlugin::load(const std::string &path) {
    void *h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h)
        throw std::runtime_error(std::string("dlopen ") + path + ": " + ::dlerror());
    Entry e{};
    e.init = reinterpret_cast<mktsim_ingress_init_fn>(::dlsym(h, MKTSIM_INGRESS_INIT_SYMBOL));
    e.start = reinterpret_cast<mktsim_ingress_start_fn>(::dlsym(h, MKTSIM_INGRESS_START_SYMBOL));
    e.stop = reinterpret_cast<mktsim_ingress_stop_fn>(::dlsym(h, MKTSIM_INGRESS_STOP_SYMBOL));
    e.destroy = reinterpret_cast<mktsim_ingress_destroy_fn>(::dlsym(h, MKTSIM_INGRESS_DESTROY_SYMBOL));
    if (!e.init || !e.start || !e.stop || !e.destroy) {
        ::dlclose(h);
        throw std::runtime_error(path + ": missing mktsim_ingress_{init,start,stop,destroy}");
    }
    return std::unique_ptr<IngressPlugin>(new IngressPlugin(path, e, h));
}

std::unique_ptr<IngressPlugin> IngressPlugin::builtin(const std::string &name, Entry entry) {
    return std::unique_ptr<IngressPlugin>(new IngressPlugin(name, entry, nullptr));
}

bool IngressPlugin::init(const mktsim_exchange_api *api, mktsim_exchange *ex, const std::string &config_json) {
    return entry_.init(api, ex, config_json.c_str(), &state_) == 0;
}

bool IngressPlugin::start() {
    if (!state_)
        return false;
    started_ = entry_.start(state_) == 0;
    return started_;
}

void IngressPlugin::stop() {
    if (started_ && state_)
        entry_.stop(state_);
    started_ = false;
}
