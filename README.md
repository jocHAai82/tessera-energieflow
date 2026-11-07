# Energy flow

A live power flow for Tessera screens, laid out like Home Assistant's power-flow-card-plus: solar on top, the grid on
the left, the battery below and the house in the middle, with a wallbox and a heat pump as the house's own branches.
A branch that carries power is drawn in its colour, an idle one grey. Over the grid it shows the price of the hour with
the step of §14a module 3 (NT, ST, HT), under it the dynamic price.

The tile stands still: Tessera lets only popups move, so the flow shows by colour and value, not by running dots.

## Set up

1. In Tessera, open Plugins, choose **Add with a link** and paste the link of this repository.
2. Add the plugin to a screen and let it build.
3. In Layout, place **Energy flow** (3x3 or larger) and choose its sensors in the inspector:
   - Solar, grid, house, battery and wallbox power and the heat pump's power, in W or kW. The grid is positive while
     power comes from the grid; the battery positive while it discharges.
   - The battery's and the car's state of charge in %.
   - The heat pump's heating and hot water output, to show what it makes now.
   - The electricity price and the price with module 3, in EUR/kWh. Leave the module 3 price empty to hide it.
4. A sensor you leave empty hides its circle; the house always shows.
