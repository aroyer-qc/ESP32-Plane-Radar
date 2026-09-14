#pragma once

namespace ui {

/** Reserve the off-screen frame buffer; call at boot, before WiFi/TLS. */
void radarDisplayInit();

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

}  // namespace ui
