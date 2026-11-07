#pragma once
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::energieflow {

// The plugin: an ESPHome component and a tessera::Plugin. It registers its tile types in setup().
class Energieflow : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
};

}  // namespace esphome::energieflow
