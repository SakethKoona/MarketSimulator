#pragma once
// Built-in synthetic order flow as an ingress adapter (see src/flowgen_ingress.cpp).
#include "ingress/plugin.hpp"
IngressPlugin::Entry flowgen_ingress_entry();
